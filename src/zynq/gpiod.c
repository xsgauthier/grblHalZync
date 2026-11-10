#include <gpiod.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "driver.h"
#include "gpiod.h"

#define CHIP_PATH "/dev/gpiochip0"

static int gpiod_line_request_wait_edge_events_wrapper(gpiod_handle_t *handle, int64_t timeout)
{
    if (!handle || !handle->in_request)
        return -1;

    return gpiod_line_request_wait_edge_events(handle->in_request, timeout);
}

static int gpiod_line_request_read_edge_events_wrapper(gpiod_handle_t *handle, int max_events)
{
    if (!handle || !handle->in_request || !handle->event_buffer)
        return -1;

    return gpiod_line_request_read_edge_events(handle->in_request, handle->event_buffer, max_events > 0 ? max_events : handle->event_buffer_size);
}

static int gpiod_process_events_callback(gpiod_handle_t *handle, void (*callback_function)(gpiod_handle_t *handle, gpiod_event_t *event))
{
    if (!handle || !handle->event_buffer)
        return -1;
    
    int num_events = gpiod_line_request_read_edge_events_wrapper(handle, handle->event_buffer_size);
    if (num_events < 0) {
        perror("Error reading edge events");
        return -1;
    }

    // Iterate through all retrieved events and call the callback function for each event
    for (int i = 0; i < num_events; i++) {
        struct gpiod_edge_event *event = gpiod_edge_event_buffer_get_event(handle->event_buffer, i);
        int ev_type = gpiod_edge_event_get_event_type(event);
        uint64_t timestamp = gpiod_edge_event_get_timestamp(event);
        int line_offset = gpiod_edge_event_get_line_offset(event);

        gpiod_event_t gpiod_event = {
            .event_type = ev_type,
            .timestamp_ns = timestamp,
            .line_offset = line_offset
        };

        callback_function(handle, &gpiod_event);
    }

    return 0;
}

gpiod_handle_t *init_gpiod_driver(void)
{
    gpiod_handle_t *handle = calloc(1, sizeof(*handle));
    if (!handle)
        return NULL;

    // 1. Open gpiochip0 via its character device path (libgpiod v2 style)
    handle->chip = gpiod_chip_open(CHIP_PATH);
    if (!handle->chip) {
        perror("Failed to open GPIO chip");
        goto cleanup;
    }

    // 2. Configure and Request Output Pins
    struct gpiod_line_settings *out_settings = gpiod_line_settings_new();
    if (!out_settings) goto cleanup;
    gpiod_line_settings_set_direction(out_settings, GPIOD_LINE_DIRECTION_OUTPUT);
    gpiod_line_settings_set_output_value(out_settings, GPIOD_LINE_VALUE_INACTIVE); // Default to low

    struct gpiod_line_config *out_line_cfg = gpiod_line_config_new();
        if (!out_line_cfg) {
        gpiod_line_settings_free(out_settings);
        goto cleanup;
    }
    unsigned int out_offsets[] = { 
        SPINDLE_ENABLE_PIN, 
        SPINDLE_DIRECTION_PIN, 
        COOLANT_FLOOD_PIN, 
        COOLANT_MIST_PIN, 
        X_STEP_PIN, 
        Y_STEP_PIN, 
        Z_STEP_PIN, 
        X_DIRECTION_PIN, 
        Y_DIRECTION_PIN, 
        Z_DIRECTION_PIN, 
        X_ENABLE_PIN, 
        Y_ENABLE_PIN, 
        Z_ENABLE_PIN
    };
    int num_out_offsets = sizeof(out_offsets) / sizeof(out_offsets[0]);
    gpiod_line_config_add_line_settings(out_line_cfg, out_offsets, num_out_offsets, out_settings);

    struct gpiod_request_config *out_req_cfg = gpiod_request_config_new();
    if (!out_req_cfg) {
        gpiod_line_config_free(out_line_cfg);
        gpiod_line_settings_free(out_settings);
        goto cleanup;
    }
    gpiod_request_config_set_consumer(out_req_cfg, "gpiod_output_lines");

    handle->out_request = gpiod_chip_request_lines(handle->chip, out_req_cfg, out_line_cfg);

    // Free config objects immediately after requesting lines (libgpiod v2 idiom)
    gpiod_request_config_free(out_req_cfg);
    gpiod_line_config_free(out_line_cfg);
    gpiod_line_settings_free(out_settings);

    if (!handle->out_request) {
        perror("Failed to request output GPIO line");
        goto cleanup;
    }



    // 3. Configure and Request Input Pin (Offset 51) with Edge Interrupts
    struct gpiod_line_settings *in_settings = gpiod_line_settings_new();
    if (!in_settings) goto cleanup;
    gpiod_line_settings_set_direction(in_settings, GPIOD_LINE_DIRECTION_INPUT);
    gpiod_line_settings_set_edge_detection(in_settings, GPIOD_LINE_EDGE_BOTH); // Catch low->high and high->low

    struct gpiod_line_config *in_line_cfg = gpiod_line_config_new();
    if (!in_line_cfg) {
        gpiod_line_settings_free(in_settings);
        goto cleanup;
    }
    unsigned int in_offsets[] = {
        X_LIMIT_PIN,
        Y_LIMIT_PIN,
        Z_LIMIT_PIN, 
        RESET_PIN, 
        FEED_HOLD_PIN, 
        CYCLE_START_PIN, 
        SAFETY_DOOR_PIN, 
        ESTOP_PIN, 
        PROBE_PIN, 
        PROBE_CONNECTED_PIN,
    };
    int num_in_offsets = sizeof(in_offsets) / sizeof(in_offsets[0]);
    gpiod_line_config_add_line_settings(in_line_cfg, in_offsets, num_in_offsets, in_settings);

    struct gpiod_request_config *in_req_cfg = gpiod_request_config_new();
    if (!in_req_cfg) {
        gpiod_line_config_free(in_line_cfg);
        gpiod_line_settings_free(in_settings);
        goto cleanup;
    }
    gpiod_request_config_set_consumer(in_req_cfg, "gpiod_input_lines");

    handle->in_request = gpiod_chip_request_lines(handle->chip, in_req_cfg, in_line_cfg);

    gpiod_request_config_free(in_req_cfg);
    gpiod_line_config_free(in_line_cfg);
    gpiod_line_settings_free(in_settings);

    if (!handle->in_request) {
        perror("Failed to request input GPIO line");
        goto cleanup;
    }

    // 4. Allocate Event Buffer
    handle->event_buffer_size = 16 * num_in_offsets; // Allocate buffer for 16 events per input line
    handle->event_buffer = gpiod_edge_event_buffer_new(handle->event_buffer_size);
    if (!handle->event_buffer) {
        perror("Failed to allocate edge event buffer");
        goto cleanup;
    }

    handle->fn_wait_for_event = gpiod_line_request_wait_edge_events_wrapper;
    handle->process_events_callback = gpiod_process_events_callback;

    return handle;

cleanup:
    if (handle->event_buffer)
        gpiod_edge_event_buffer_free(handle->event_buffer);
    if (handle->in_request)
        gpiod_line_request_release(handle->in_request);
    if (handle->out_request)
        gpiod_line_request_release(handle->out_request);
    if (handle->chip)
        gpiod_chip_close(handle->chip);
    free(handle);

    return NULL;
}

void deinit_gpiod_driver(gpiod_handle_t *handle)
{
    if (!handle)
        return;

    if (handle->event_buffer)
        gpiod_edge_event_buffer_free(handle->event_buffer);
    if (handle->in_request)
        gpiod_line_request_release(handle->in_request);
    if (handle->out_request)
        gpiod_line_request_release(handle->out_request);
    if (handle->chip)
        gpiod_chip_close(handle->chip);

    free(handle);
}