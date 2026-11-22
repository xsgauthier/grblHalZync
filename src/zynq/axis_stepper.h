#pragma once
#include "../platform.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define BURST_SIZE_WORDS 256

typedef struct axis_stepper_s axis_stepper_t;
typedef axis_stepper_t *p_axis_stepper_t;

struct axis_stepper_s
{
    volatile uint32_t buffer[BURST_SIZE_WORDS];
    volatile size_t count;
    volatile int fd;
    int (*write)(p_axis_stepper_t handle,
                      uint32_t step_mask,
                      uint32_t dir_mask,
                      uint32_t delay_cycles);
    int (*flush)(p_axis_stepper_t handle);
};

extern p_axis_stepper_t axis_stepper_initialize(void);
extern void axis_stepper_close(p_axis_stepper_t handle);
