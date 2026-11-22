#ifndef ZYNQ_LOGGER_H
#define ZYNQ_LOGGER_H

#include <stdio.h>

int logger_fprintf(FILE *stream, const char *format, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
;

void logger_shutdown(void);

#endif