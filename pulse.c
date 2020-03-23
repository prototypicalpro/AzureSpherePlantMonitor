#include "pulse.h"

static void pulse_cb(uev_t* w, void* arg, int events) {
	if (events == UEV_ERROR) {
		Log_Debug("Error event in pulsecb\n");
	}
	else {
		pulse_t* data = (pulse_t*)arg;
		// switch directions if we hit either maximum
		if (data->cur_state.dutyCycle_nsec < data->fade_inc_nsec
			|| data->cur_state.dutyCycle_nsec + data->fade_inc_nsec > data->max_cycle_nsec)
			data->cur_direction = !data->cur_direction;
		// increment or decrement the nanosecond counter
		if (data->cur_direction)
			data->cur_state.dutyCycle_nsec += data->fade_inc_nsec;
		else
			data->cur_state.dutyCycle_nsec -= data->fade_inc_nsec;
		data->cur_state.enabled = (data->cur_state.dutyCycle_nsec != 0);
		// apply the changes
		PWM_Apply(data->PWM_fd, data->PWM_channel, &data->cur_state);
	}
}

void pulse_configure(	pulse_t* data,
						int PWM_fd,
						PWM_ChannelId channel,
						unsigned int period_nsec,
						unsigned int max_cycle_nsec,
						unsigned int fade_inc_nsec,
						unsigned int fade_msec) {
	// initialize properties
	data->PWM_fd = PWM_fd;
	data->PWM_channel = channel;
	data->max_cycle_nsec = max_cycle_nsec;
	data->fade_inc_nsec = fade_inc_nsec;
	data->fade_msec = fade_msec;
	data->cur_direction = true;
	// initialize PwmState to the correct values
	data->cur_state.dutyCycle_nsec = max_cycle_nsec;
	data->cur_state.enabled = true;
	data->cur_state.period_nsec = period_nsec;
	data->cur_state.polarity = PWM_Polarity_Inversed;
}

int pulse_start(uev_ctx_t* ctx, pulse_t* data) {
	return uev_timer_init(ctx, &data->w, pulse_cb, data, (int)data->fade_msec, (int)data->fade_msec);
}

int pulse_stop(pulse_t* data, bool on_or_off) {
	uev_timer_stop(&data->w);
	data->cur_state.enabled = true;
	data->cur_state.dutyCycle_nsec = 0;
	data->cur_state.period_nsec = 100000;
	data->cur_state.polarity = on_or_off ? PWM_Polarity_Normal : PWM_Polarity_Inversed;
	return PWM_Apply(data->PWM_fd, data->PWM_channel, &data->cur_state);
}