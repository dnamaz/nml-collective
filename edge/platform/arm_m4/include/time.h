/*
 * Minimal time.h for NML Edge Worker — ARM Cortex-M4 bare metal.
 *
 * time() and clock_gettime() are stubbed; the edge worker uses them only for
 * the heartbeat timer.  Override arm_edge_time_ms() in your BSP to supply a
 * real millisecond tick (e.g. from SysTick or a hardware RTC).
 *
 * When using gcc-arm-embedded + newlib-nano, remove this file.
 */
#ifndef _EDGE_TIME_H
#define _EDGE_TIME_H

#include <stdint.h>

typedef long          time_t;
typedef long          clock_t;
typedef long          suseconds_t;

struct timespec {
    time_t tv_sec;
    long   tv_nsec;
};

struct timeval {
    time_t      tv_sec;
    suseconds_t tv_usec;
};

#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

/*
 * BSP hook — override this to return milliseconds since boot.
 * Default stub returns 0, which means the heartbeat fires on every tick.
 */
uint32_t arm_edge_time_ms(void);

time_t time(time_t *t);
int    clock_gettime(int clk_id, struct timespec *ts);

#endif /* _EDGE_TIME_H */
