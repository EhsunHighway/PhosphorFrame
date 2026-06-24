#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "scheduler.h"

Scheduler *scheduler_create(size_t capacity) {
    if (capacity == 0) {
        return NULL;
    }

    Scheduler *s = malloc(sizeof(Scheduler));
    if (!s) {
        return NULL;
    }

    memset(s->handlers, 0, sizeof(s->handlers));
    s->eq = event_queue_create(capacity);
    if (!s->eq) {
        free(s);
        return NULL;
    }

    s->now     = 0;
    s->running = 0; 
    return s;
}

void       scheduler_free(Scheduler *s) {
    if (!s) {
        return;
    }

    Event *e;
    while ((e = event_queue_pop(s->eq)) != NULL) {
        event_free(e);
    }

    event_queue_free(s->eq);
    free(s);
}


void       scheduler_register(Scheduler *s, EventType type, EventHandler fn, void *ctx) {
    if (!s || type < 0 || type >= EVT_TYPE_COUNT) {
        return;
    }

    s->handlers[type].fn  = fn;
    s->handlers[type].ctx = ctx;
}

int       scheduler_schedule(Scheduler *s, Event *e) {
    if (!s || !e) {
        return -1;
    }
    return event_queue_push(s->eq, e);
}

int        scheduler_step(Scheduler *s) {
    Event *e = event_queue_pop(s->eq);
    if (!e) {
        return 0;
    }

    if (e->timestamp > s->now) {
        s->now = e->timestamp;
    }

    if (e->handler) {
        e->handler(e, e->handler_ctx);
    } else if (e->type >= 0 && e->type < EVT_TYPE_COUNT) {
        HandlerEntry entry = s->handlers[e->type];
        if (entry.fn) {
            entry.fn(e, entry.ctx);
        }
    }

    event_free(e);
    return 1;
}

int        scheduler_run(Scheduler *s) {
    s->running = 1;
    while ((scheduler_step(s) == 1) && (s->running == 1)) {
        // keep stepping until no events or running is set to 0
    }
    s->running = 0;
    return 0;
}

void       scheduler_stop(Scheduler *s) {
    if (s) {
        s->running = 0;
    }
}

uint64_t   scheduler_now(const Scheduler *s) {
    if (!s) {
        return 0;
    }
    return s->now;
}
