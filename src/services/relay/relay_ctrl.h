#ifndef SSH_BRIDGE_RELAY_CTRL_H
#define SSH_BRIDGE_RELAY_CTRL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Open COM/tty, send A0 set (logical level + io_inverted), close. */
int relay_ctrl_set(const char *port, int baudrate, int channel, int logical_on,
                   int io_inverted, int post_write_delay_ms, char *err, size_t errcap);

/* pulse: idle/pulse from polarity_inverted, hold_seconds, then idle. */
int relay_ctrl_pulse(const char *port, int baudrate, int channel, double hold_seconds,
                     int polarity_inverted, int io_inverted, int post_write_delay_ms,
                     char *err, size_t errcap);

/* mode: hold_on | hold_off | pulse */
int relay_ctrl_run_mode(const char *port, int baudrate, int channel, const char *mode,
                        double hold_seconds, int polarity_inverted, int io_inverted,
                        int post_write_delay_ms, char *err, size_t errcap);

#ifdef __cplusplus
}
#endif

#endif
