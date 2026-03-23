/*
 * time_stubs.c — time() / clock_gettime() stubs for ARM Cortex-M4.
 *
 * The edge worker uses time() only for the heartbeat interval counter.
 * We delegate to arm_edge_time_ms(), a weak BSP hook that returns a
 * millisecond tick.  The default returns 0; override it in your BSP:
 *
 *   uint32_t arm_edge_time_ms(void) { return HAL_GetTick(); }   // STM32
 *   uint32_t arm_edge_time_ms(void) { return xTaskGetTickCount(); } // FreeRTOS
 */

#include <time.h>
#include <stdint.h>

__attribute__((weak)) uint32_t arm_edge_time_ms(void) { return 0; }

time_t time(time_t *t)
{
    time_t now = (time_t)(arm_edge_time_ms() / 1000u);
    if (t) *t = now;
    return now;
}

int clock_gettime(int clk_id, struct timespec *ts)
{
    (void)clk_id;
    uint32_t ms = arm_edge_time_ms();
    ts->tv_sec  = (time_t)(ms / 1000u);
    ts->tv_nsec = (long)((ms % 1000u) * 1000000L);
    return 0;
}
