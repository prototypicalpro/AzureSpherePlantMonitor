#include <sys/eventfd.h>

#include <applibs/eventloop.h>

struct EventLoopEvent;
typedef struct EventLoopEvent EventLoopEvent_t;

typedef void (*EventLoopEventHandler)(EventLoopEvent_t* event, void* ctx);

struct EventLoopEvent {
    EventLoop* _event_loop;
    EventLoopEventHandler _handler;
    void* _ctx;
    int _fd;
    EventRegistration* _registration;
};

EventLoopEvent_t* CreateEventLoopEvent(EventLoop* loop, EventLoopEventHandler handler, void* ctx);
int PostEventLoopEvent(EventLoopEvent_t* event);
int ConsumeEventLoopEvent(EventLoopEvent_t* event, eventfd_t* out);
void DisposeEventLoopEvent(EventLoopEvent_t* event);