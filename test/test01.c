#include <gpiod.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define CHIP_PATH "/dev/gpiochip0"
#define INPUT_PIN_OFFSET  51
#define OUTPUT_PIN_OFFSET 37

int main(void) {
    struct gpiod_chip *chip = NULL;
    struct gpiod_line_request *out_request = NULL;
    struct gpiod_line_request *in_request = NULL;
    struct gpiod_edge_event_buffer *event_buffer = NULL;

    int exit_code = EXIT_FAILURE;

    // 1. Open gpiochip0 via its character device path (libgpiod v2 style)
    chip = gpiod_chip_open(CHIP_PATH);
    if (!chip) {
        perror("Failed to open GPIO chip");
        return EXIT_FAILURE;
    }

    // 2. Configure and Request Output Pin (Offset 37)
    struct gpiod_line_settings *out_settings = gpiod_line_settings_new();
    if (!out_settings) goto cleanup;
    gpiod_line_settings_set_direction(out_settings, GPIOD_LINE_DIRECTION_OUTPUT);
    gpiod_line_settings_set_output_value(out_settings, GPIOD_LINE_VALUE_INACTIVE); // Default to low

    struct gpiod_line_config *out_line_cfg = gpiod_line_config_new();
    if (!out_line_cfg) {
        gpiod_line_settings_free(out_settings);
        goto cleanup;
    }
    unsigned int out_offsets[] = { OUTPUT_PIN_OFFSET };
    gpiod_line_config_add_line_settings(out_line_cfg, out_offsets, 1, out_settings);

    struct gpiod_request_config *out_req_cfg = gpiod_request_config_new();
    if (!out_req_cfg) {
        gpiod_line_config_free(out_line_cfg);
        gpiod_line_settings_free(out_settings);
        goto cleanup;
    }
    gpiod_request_config_set_consumer(out_req_cfg, "signal_mirror_output");

    out_request = gpiod_chip_request_lines(chip, out_req_cfg, out_line_cfg);

    // Free config objects immediately after requesting lines (libgpiod v2 idiom)
    gpiod_request_config_free(out_req_cfg);
    gpiod_line_config_free(out_line_cfg);
    gpiod_line_settings_free(out_settings);

    if (!out_request) {
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
    unsigned int in_offsets[] = { INPUT_PIN_OFFSET };
    gpiod_line_config_add_line_settings(in_line_cfg, in_offsets, 1, in_settings);

    struct gpiod_request_config *in_req_cfg = gpiod_request_config_new();
    if (!in_req_cfg) {
        gpiod_line_config_free(in_line_cfg);
        gpiod_line_settings_free(in_settings);
        goto cleanup;
    }
    gpiod_request_config_set_consumer(in_req_cfg, "signal_mirror_input");

    in_request = gpiod_chip_request_lines(chip, in_req_cfg, in_line_cfg);

    gpiod_request_config_free(in_req_cfg);
    gpiod_line_config_free(in_line_cfg);
    gpiod_line_settings_free(in_settings);

    if (!in_request) {
        perror("Failed to request input GPIO line");
        goto cleanup;
    }

    // 4. Allocate Event Buffer
    event_buffer = gpiod_edge_event_buffer_new(16);
    if (!event_buffer) {
        perror("Failed to allocate edge event buffer");
        goto cleanup;
    }

    printf("Mirror active. Watching input %d -> Writing output %d...\n", INPUT_PIN_OFFSET, OUTPUT_PIN_OFFSET);

    // 5. Main Processing Loop
    while (1) {
        // Blocks until an edge interrupt arrives on input 51
        int ret = gpiod_line_request_wait_edge_events(in_request, -1);
        if (ret < 0) {
            perror("Error waiting for edge events");
            break;
        }

        // Read up to 16 events into the buffer
        int num_events = gpiod_line_request_read_edge_events(in_request, event_buffer, 16);
        if (num_events < 0) {
            perror("Error reading edge events");
            break;
        }

        // Iterate through all retrieved events
        for (int i = 0; i < num_events; i++) {
            struct gpiod_edge_event *event = gpiod_edge_event_buffer_get_event(event_buffer, i);
            int ev_type = gpiod_edge_event_get_event_type(event);

            // Mirror the value directly to the output pin
            if (ev_type == GPIOD_EDGE_EVENT_RISING_EDGE) {
                gpiod_line_request_set_value(out_request, OUTPUT_PIN_OFFSET, GPIOD_LINE_VALUE_ACTIVE);
                printf("[Mirror] Input WENT HIGH -> Output SET HIGH\n");
            } else if (ev_type == GPIOD_EDGE_EVENT_FALLING_EDGE) {
                gpiod_line_request_set_value(out_request, OUTPUT_PIN_OFFSET, GPIOD_LINE_VALUE_INACTIVE);
                printf("[Mirror] Input WENT LOW  -> Output SET LOW\n");
            }
        }
    }

    exit_code = EXIT_SUCCESS;

cleanup:
    if (event_buffer)
        gpiod_edge_event_buffer_free(event_buffer);
    if (in_request)
        gpiod_line_request_release(in_request);
    if (out_request)
        gpiod_line_request_release(out_request);
    if (chip)
        gpiod_chip_close(chip);

    return exit_code;
}
