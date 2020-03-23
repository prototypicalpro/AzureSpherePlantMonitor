#include "poll.h"

static void poll_cb(uev_t* w, void* arg, int events) {
	if (events == UEV_ERROR) {
		Log_Debug("Error in polling timer\n");
	}
	else {
		poll_t* data = (poll_t*)arg;
		// check if the network is operating
		int res = data->poll_cb(w, data->poll_arg, events);

		if (res < 0)
			Log_Debug("Call to poll_cb failed\n");
		else if (res > 0) {
			if (data->w_event)
				uev_event_post(data->w_event);
		}
		else {
			uev_timer_set(w, data->poll_interval, UEV_NONE);
			uev_timer_start(w);
		}
	}
}

void poll_configure(poll_t* c, poll_cb_t* poll_cb, void* poll_arg, uev_t* w_event, int poll_interval_ms) {
	c->poll_cb = poll_cb;
	c->poll_arg = poll_arg;
	c->w_event = w_event;
	c->poll_interval = poll_interval_ms;
	c->did_start = 0;
}

int poll_start(uev_ctx_t* ctx, poll_t* data) {
	data->did_start = 1;
	return uev_timer_init(ctx, &data->w, poll_cb, data, data->poll_interval, UEV_NONE);
}

int poll_stop(poll_t* data) {
	if (data->did_start)
		return uev_timer_stop(&data->w);
	return 0;
}