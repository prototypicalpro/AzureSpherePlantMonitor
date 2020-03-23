/** simple pulse library to fade an LED */

#include <uev.h>
#include <applibs/log.h>
#include <applibs/pwm.h>

#ifndef PULSE_H
#define PULSE_H

typedef struct {
	uev_t w;
	int PWM_fd;
	PWM_ChannelId PWM_channel;
	unsigned int max_cycle_nsec;
	unsigned int fade_inc_nsec;
	unsigned int fade_msec;
	bool cur_direction;
	PwmState cur_state;
} pulse_t;

void pulse_configure(	pulse_t* data, 
						int PWM_fd,
						PWM_ChannelId channel, 
						unsigned int period_nsec, 
						unsigned int max_cycle_nsec, 
						unsigned int fade_inc_nsec,
						unsigned int fade_msec);

int pulse_start(uev_ctx_t* ctx, pulse_t* data);

int pulse_stop(pulse_t* data, bool on_or_off);

#endif