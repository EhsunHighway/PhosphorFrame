#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "event.h"
#include <stdint.h>
#include <stddef.h>

typedef EventCallback EventHandler;

/* 
 * One entry in the handler table — a callback + its user context. 
 */
typedef struct {
    EventHandler  fn;       // NULL means "no fallback handler registered"
    void         *ctx;      // fallback context
} HandlerEntry;

typedef struct Scheduler {
    EventQueue   *eq;                       // queue ordered by timestamp
    uint64_t      now;                      // current simulated time
    int           running;                  // 1 while run-loop is active
    HandlerEntry  handlers[EVT_TYPE_COUNT]; // fallback handlers by EventType
} Scheduler;


/*@
    behavior bad:
        assumes capacity == 0;
        assigns \nothing;
        ensures \result == \null;
    behavior ok:
        assumes capacity > 0;
        allocates \result;
        ensures \result == \null || \valid(\result);
        ensures \result != \null ==>
            (\valid(\result) &&
             \valid(\result->eq) &&
             \result->now     == 0 &&
             \result->running == 0);
    complete behaviors;
    disjoint behaviors;
*/
Scheduler *scheduler_create(size_t capacity);

/*@
    requires s == \null || \valid(s);
    assigns  \nothing;
*/
void       scheduler_free(Scheduler *s);

/*@
    behavior null:
        assumes s == \null;
        assigns \nothing;
    behavior valid:
        assumes \valid(s);
        assigns  s->handlers[(int)type];
        ensures  s->handlers[(int)type].fn  == fn;
        ensures  s->handlers[(int)type].ctx == ctx;
    complete behaviors;
    disjoint behaviors;
*/
void       scheduler_register(Scheduler    *s,
                              EventType     type,
                              EventHandler  fn,
                              void         *ctx);

/*@
    behavior null:
        assumes s == \null || e == \null;
        assigns \nothing;
        ensures \result == -1;
    behavior valid:
        assumes \valid(s) && \valid(e);
        assigns  s->eq->events, s->eq->count;
        ensures  \result ==  0 ==> s->eq->count == \old(s->eq->count) + 1;
        ensures  \result == -1 ==> s->eq->count == \old(s->eq->count);
    complete behaviors;
    disjoint behaviors;
*/
int        scheduler_schedule(Scheduler *s, Event *e);

/*@
    requires \valid(s);
    assigns  s->eq->events, s->eq->count, s->now;
    ensures  \result == 0 ==> s->eq->count == \old(s->eq->count);
    ensures  \result == 1 ==>
        (s->eq->count == \old(s->eq->count) - 1 &&
         s->now       >= \old(s->now));
*/
int        scheduler_step(Scheduler *s);

/*@
    requires \valid(s);
    assigns  s->eq->events, s->eq->count, s->now, s->running;
    ensures  s->eq->count == 0 || s->running == 0;
*/
int        scheduler_run(Scheduler *s);

/*@
    behavior null:
        assumes s == \null;
        assigns \nothing;
    behavior valid:
        assumes \valid(s);
        assigns  s->running;
        ensures  s->running == 0;
    complete behaviors;
    disjoint behaviors;
*/
void       scheduler_stop(Scheduler *s);

/*@
    behavior null:
        assumes s == \null;
        assigns \nothing;
        ensures \result == 0;
    behavior valid:
        assumes \valid_read(s);
        assigns  \nothing;
        ensures  \result == s->now;
    complete behaviors;
    disjoint behaviors;
*/
uint64_t   scheduler_now(const Scheduler *s);

#endif
