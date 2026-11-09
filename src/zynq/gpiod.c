#include <gpiod.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define CHIP_PATH "/dev/gpiochip0"

int init_gpiod_driver(void)
{
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