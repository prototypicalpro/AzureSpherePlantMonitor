/** handler which polls a function until it returns 1 or -1 */

#include <uev.h>
#include <applibs/log.h>

#ifndef POLL_H
#define POLL_H

typedef int poll_cb_t(uev_t* w, void* arg, int events);

typedef struct {
	uev_t w;
	poll_cb_t* poll_cb;
	void* poll_arg;
	uev_t* w_event;
	int poll_interval;
	uint8_t did_start;
} poll_t;

void poll_configure(poll_t* c, poll_cb_t* poll_cb, void* poll_arg, uev_t* w_event, int poll_interval);
int poll_start(uev_ctx_t* ctx, poll_t* data);
int poll_stop(poll_t* data);

#endif