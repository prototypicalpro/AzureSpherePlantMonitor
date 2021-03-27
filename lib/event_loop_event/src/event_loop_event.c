#include <sys/eventfd.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>

#include <applibs/log.h>

#include "event_loop_event.h"

// This satisfies the EventLoopIoCallback signature.
static void TimerCallback(EventLoop* el, int fd, EventLoop_IoEvents events, void* context)
{
	EventLoopEvent_t* event = (EventLoopEvent_t*)context;
	event->_handler(event, event->_ctx);
}

EventLoopEvent_t* CreateEventLoopEvent(EventLoop* loop, EventLoopEventHandler handler, void* ctx) {
	EventLoopEvent_t* event = malloc(sizeof(EventLoopEvent_t));
	if (event == NULL)
		return NULL;
	
	event->_event_loop = loop;
	event->_handler = handler;
	event->_ctx = ctx;

	event->_fd = eventfd(0, 0);
	if (event->_fd == -1) {
		Log_Debug("Failed to create eventfd with error %i\n", errno);
		goto failed;
	}
	
	event->_registration = EventLoop_RegisterIo(event->_event_loop, event->_fd, EventLoop_Input, TimerCallback, event);
	if (event->_registration == NULL) {
		Log_Debug("Failed to create event registration for event with error %i", errno);
		goto failed;
	}

	return event;

failed:
	DisposeEventLoopEvent(event);
	return NULL;
}

int PostEventLoopEvent(EventLoopEvent_t* event) {
	return eventfd_write(event->_fd, 1);
}

int ConsumeEventLoopEvent(EventLoopEvent_t* event, eventfd_t* out) {
	return eventfd_read(event->_fd, out);
}

void DisposeEventLoopEvent(EventLoopEvent_t* event) {
	if (event->_registration) 
		EventLoop_UnregisterIo(event->_event_loop, event->_registration);
	if (event->_fd != -1) 
		close(event->_fd);
	free(event);
}