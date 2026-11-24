#ifndef ZYNQ_AXIS_STEPPER_DIAGNOSTICS_H
#define ZYNQ_AXIS_STEPPER_DIAGNOSTICS_H

#include <stddef.h>
#include <stdint.h>

typedef struct axis_stepper_diagnostics_s *axis_stepper_diagnostics_t;

typedef struct {
	int enabled;
	axis_stepper_diagnostics_t (*create)(void);
	void (*destroy)(axis_stepper_diagnostics_t diagnostics);
	uint64_t (*now_ns)(void);
	void (*queue_depth)(axis_stepper_diagnostics_t diagnostics, size_t count);
	void (*producer_wait_begin)(axis_stepper_diagnostics_t diagnostics);
	void (*producer_wait_end)(axis_stepper_diagnostics_t diagnostics, uint64_t wait_ns);
	void (*fifo_wait)(axis_stepper_diagnostics_t diagnostics, uint64_t wait_ns);
	void (*write_begin)(axis_stepper_diagnostics_t diagnostics);
	void (*write_end)(axis_stepper_diagnostics_t diagnostics);
	void (*write_error)(axis_stepper_diagnostics_t diagnostics);
	void (*words_written)(axis_stepper_diagnostics_t diagnostics, size_t count);
	void (*report)(axis_stepper_diagnostics_t diagnostics,
				   size_t queue_count,
				   size_t queue_capacity);
} axis_stepper_diagnostics_api_t;

extern const axis_stepper_diagnostics_api_t axis_stepper_diagnostics_default_api;
extern const axis_stepper_diagnostics_api_t axis_stepper_diagnostics_dummy_api;

// Select before axis_stepper_initialize() to use a custom or dummy API.
void axis_stepper_diagnostics_set_api(const axis_stepper_diagnostics_api_t *api);
const axis_stepper_diagnostics_api_t *axis_stepper_diagnostics_get_api(void);

axis_stepper_diagnostics_t axis_stepper_diagnostics_create(void);
void axis_stepper_diagnostics_destroy(axis_stepper_diagnostics_t diagnostics);
uint64_t axis_stepper_diagnostics_now_ns(void);
void axis_stepper_diagnostics_queue_depth(axis_stepper_diagnostics_t diagnostics, size_t count);
void axis_stepper_diagnostics_producer_wait_begin(axis_stepper_diagnostics_t diagnostics);
void axis_stepper_diagnostics_producer_wait_end(axis_stepper_diagnostics_t diagnostics, uint64_t wait_ns);
void axis_stepper_diagnostics_fifo_wait(axis_stepper_diagnostics_t diagnostics, uint64_t wait_ns);
void axis_stepper_diagnostics_write_begin(axis_stepper_diagnostics_t diagnostics);
void axis_stepper_diagnostics_write_end(axis_stepper_diagnostics_t diagnostics);
void axis_stepper_diagnostics_write_error(axis_stepper_diagnostics_t diagnostics);
void axis_stepper_diagnostics_words_written(axis_stepper_diagnostics_t diagnostics, size_t count);
void axis_stepper_diagnostics_report(axis_stepper_diagnostics_t diagnostics,
									size_t queue_count,
									size_t queue_capacity);

#endif
