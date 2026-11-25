#ifndef ZYNQ_AXIS_STEPPER_H
#define ZYNQ_AXIS_STEPPER_H

#include <stdint.h>

typedef struct axis_stepper_s *p_axis_stepper_t;

p_axis_stepper_t axis_stepper_initialize(void);
int axis_stepper_enqueue(p_axis_stepper_t handle,
                         uint32_t step_mask,
                         uint32_t dir_mask,
                         uint32_t delay_cycles);
int axis_ioexpander_outputs_enqueue(p_axis_stepper_t handle, 
    uint16_t pcf_address, 
    uint16_t value);

int axis_stepper_flush(p_axis_stepper_t handle);
void axis_stepper_input_complete(p_axis_stepper_t handle);
void axis_stepper_start_output(p_axis_stepper_t handle);
void axis_stepper_set_feed_hold(p_axis_stepper_t handle, int active);
void *axis_stepper_writer_thread(void *arg);
void axis_stepper_stop(p_axis_stepper_t handle);
void axis_stepper_close(p_axis_stepper_t handle);

#endif