#pragma once

typedef struct {
    int event_type; // GPIOD_EDGE_EVENT_RISING_EDGE or GPIOD_EDGE_EVENT_FALLING_EDGE
    uint64_t timestamp_ns; // Timestamp of the event in nanoseconds
    int line_offset; // The offset of the GPIO line that triggered the event
} gpiod_event_t;


typedef struct {
    struct gpiod_chip *chip;
    struct gpiod_line_request *out_request;
    struct gpiod_line_request *in_request;
    struct gpiod_edge_event_buffer *event_buffer;
    size_t event_buffer_size;

    int (*fn_wait_for_event)(gpiod_handle_t *handle, int64_t timeout_ns);
    int (*process_events_callback)(gpiod_handle_t *handle, void (*callback_function)(gpiod_handle_t *handle, gpiod_event_t *event));
} gpiod_handle_t;

gpiod_handle_t *init_gpiod_driver(void);
void deinit_gpiod_driver(gpiod_handle_t *handle);

