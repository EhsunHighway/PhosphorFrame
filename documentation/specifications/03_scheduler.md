# Module 03 - Scheduler

**Files:** `src/engine/scheduler.c`, `src/engine/scheduler.h`
**Status:** Implemented
**Depends on:** `event`

## Concepts First

The event queue answers:

```text
Which event happens next?
```

The scheduler answers:

```text
When that event happens, how does the simulator execute it?
```

The scheduler owns the virtual clock, the event queue, and a fallback dispatch
table. Every delayed action in the simulator eventually passes through the
scheduler.

### Virtual Clock

`Scheduler.now` is simulated time.

When the scheduler pops an event, it advances `now` to the event timestamp if
the event timestamp is greater than the current time.

The current implementation never moves time backward:

```c
if (e->timestamp > s->now) {
    s->now = e->timestamp;
}
```

If a caller schedules an event in the past, the scheduler still executes it, but
`now` stays where it is.

### Per-Event Dispatch

An event may carry its own callback:

```c
e->handler(e, e->handler_ctx)
```

This is the owner-specific path. It is the right shape for delayed work such as:

- one TCP connection's retransmission timer
- one switch's MAC aging timer
- one protocol instance's timeout
- one link delivery callback

The callback context is the event's `self` pointer. The scheduler does not know
or inspect its concrete type.

### Fallback Type Dispatch

The scheduler also has a handler table indexed by `EventType`:

```text
handlers[EVT_ARP_REQUEST] -> function + context
handlers[EVT_ARP_REPLY]   -> function + context
handlers[EVT_RENDER]      -> function + context
```

This is fallback dispatch for simple event classes. It is not a replacement for
per-event callbacks when multiple owners of the same event type can exist.

Dispatch order is:

1. If `event->handler != NULL`, call the event-specific callback.
2. Otherwise, if `event->type` is valid and a fallback handler exists, call it.
3. Otherwise, do nothing for dispatch.

In every case where an event was popped, the scheduler frees the `Event` record
after dispatch.

### Running Versus Stopping

`scheduler_run` sets `running = 1`, repeatedly steps events, and exits when:

- no more events are available, or
- a handler calls `scheduler_stop`

`scheduler_stop` only sets `running = 0`. If called inside a handler, the
current event finishes first, then the run loop stops before the next event.

## Purpose

The scheduler module owns simulation-time execution of queued events.

It provides:

- scheduler allocation
- scheduler destruction
- fallback handler registration
- event scheduling
- single-event execution
- run-loop execution
- stop signal
- current virtual time query

It does not:

- allocate events directly
- own packets inside events
- own opaque event data
- know concrete types behind event pointers
- decide link delay or protocol timer values

## Architecture Boundary

| Responsibility | Owner |
| --- | --- |
| Store events in timestamp order | EventQueue |
| Own the queue used by a simulator | Scheduler |
| Advance virtual time | Scheduler |
| Execute per-event callbacks | Scheduler |
| Execute fallback handlers | Scheduler |
| Create packet/link/protocol events | Caller modules |
| Free popped `Event` records | Scheduler |
| Free packets or event data | Event handler / owning module |

`scheduler_free` drains pending events and frees the `Event` records. It does
not free packets or opaque data stored in those events.

## Data Model

### `EventHandler`

```c
typedef EventCallback EventHandler;
```

Scheduler handlers use the same function shape as event callbacks:

```c
void handler(const Event *e, void *ctx);
```

### `HandlerEntry`

```c
typedef struct {
    EventHandler  fn;
    void         *ctx;
} HandlerEntry;
```

`fn == NULL` means no fallback handler is registered for that event type.

`ctx` is an opaque context passed to `fn`.

### `Scheduler`

```c
typedef struct Scheduler {
    EventQueue   *eq;
    uint64_t      now;
    int           running;
    HandlerEntry  handlers[EVT_TYPE_COUNT];
} Scheduler;
```

Field meanings:

| Field | Meaning |
| --- | --- |
| `eq` | Owned event queue. |
| `now` | Current simulated time. |
| `running` | `1` while `scheduler_run` should keep stepping. |
| `handlers` | Fallback handler table indexed by valid `EventType`. |

## Ownership And Lifetime

`scheduler_create` allocates:

- one `Scheduler`
- one owned `EventQueue`
- the queue's event-pointer array through `event_queue_create`

`scheduler_free`:

- pops all pending events
- shallow-frees each pending `Event`
- frees the queue
- frees the scheduler

It does not free:

- `event->packet`
- `event->data`
- `event->src_device`
- `event->dst_device`
- `event->handler_ctx`

The current implementation expects `scheduler_step`, `scheduler_run`, and
`scheduler_free` to receive a scheduler whose `eq` is valid.

## Public API

```c
Scheduler *scheduler_create(size_t capacity);

void       scheduler_free(Scheduler *s);

void       scheduler_register(Scheduler    *s,
                              EventType     type,
                              EventHandler  fn,
                              void         *ctx);

int        scheduler_schedule(Scheduler *s, Event *e);

int        scheduler_step(Scheduler *s);

int        scheduler_run(Scheduler *s);

void       scheduler_stop(Scheduler *s);

uint64_t   scheduler_now(const Scheduler *s);
```

## Function Behavior

### `scheduler_create`

Required behavior:

- If `capacity == 0`, return `NULL`.
- Allocate one `Scheduler`.
- Clear all fallback handler slots.
- Create an event queue with the requested capacity.
- If queue creation fails, free the scheduler and return `NULL`.
- Set `now == 0`.
- Set `running == 0`.
- Return the scheduler.

### `scheduler_free`

Required behavior:

- If `s == NULL`, return immediately.
- Pop all pending events from `s->eq`.
- Free each popped `Event` record with `event_free`.
- Free the owned event queue.
- Free the scheduler.

The pending events are shallow-freed. Their packets and opaque data are not
freed here.

### `scheduler_register`

Required behavior:

- If `s == NULL`, return without changing state.
- If `type < 0` or `type >= EVT_TYPE_COUNT`, return without changing state.
- Otherwise, replace the fallback handler slot:
  - `handlers[type].fn = fn`
  - `handlers[type].ctx = ctx`

Registering `fn == NULL` is allowed and means the fallback slot is cleared.

### `scheduler_schedule`

Required behavior:

- If `s == NULL`, return `-1`.
- If `e == NULL`, return `-1`.
- Insert the event into `s->eq` through `event_queue_push`.
- Return the result from `event_queue_push`.

On success, the scheduler queue owns the event until it is popped.

### `scheduler_step`

Required behavior:

- Caller must pass a valid scheduler with a valid queue.
- Pop one event from `s->eq`.
- If no event exists, return `0`.
- If the event timestamp is greater than `s->now`, update `s->now`.
- If `event->handler != NULL`, call it with `event->handler_ctx`.
- Otherwise, if `event->type` is valid:
  - read the fallback handler entry
  - if `entry.fn != NULL`, call `entry.fn(event, entry.ctx)`
- Free the popped event record.
- Return `1`.

`scheduler_step` does not check `s == NULL`.

### `scheduler_run`

Required behavior:

- Caller must pass a valid scheduler.
- Set `running = 1`.
- Repeatedly call `scheduler_step`.
- Continue while:
  - `scheduler_step` returns `1`, and
  - `running == 1`
- Before returning, set `running = 0`.
- Return `0`.

### `scheduler_stop`

Required behavior:

- If `s == NULL`, return immediately.
- Set `running = 0`.

### `scheduler_now`

Required behavior:

- If `s == NULL`, return `0`.
- Otherwise return `s->now`.

## Flow Charts

### Create Scheduler

```text
scheduler_create(capacity)
  |
  +-- capacity == 0: return NULL
  |
  +-- allocate Scheduler
  |
  +-- clear handler table
  |
  +-- create EventQueue
  |     |
  |     +-- fail: free Scheduler, return NULL
  |
  +-- now = 0
  +-- running = 0
  |
  +-- return Scheduler
```

### Execute One Event

```text
scheduler_step(s)
  |
  +-- e = event_queue_pop(s->eq)
  |
  +-- no event: return 0
  |
  +-- if e->timestamp > s->now:
  |     s->now = e->timestamp
  |
  +-- if e->handler exists:
  |     call e->handler(e, e->handler_ctx)
  |
  +-- else if e->type is valid and fallback fn exists:
  |     call fallback fn(e, fallback ctx)
  |
  +-- event_free(e)
  |
  +-- return 1
```

### Run Loop

```text
scheduler_run(s)
  |
  +-- running = 1
  |
  +-- while scheduler_step(s) returns 1 and running == 1:
  |     keep going
  |
  +-- running = 0
  |
  +-- return 0
```

## ACSL Contracts

The contracts belong in `scheduler.h`. They rely on the event module's queue
predicates.

### Shared Predicates

```c
/*@
    predicate scheduler_handlers_cleared(Scheduler *s) =
        \forall integer i; 0 <= i && i < EVT_TYPE_COUNT ==>
            s->handlers[i].fn == \null &&
            s->handlers[i].ctx == \null;

    predicate scheduler_well_formed(Scheduler *s) =
        \valid(s) &&
        event_queue_well_formed(s->eq) &&
        (s->running == 0 || s->running == 1);
*/
```

### `scheduler_create`

```c
/*@
    behavior bad:
        assumes capacity == 0;
        assigns \nothing;
        ensures \result == \null;

    behavior ok:
        assumes capacity > 0;
        allocates \result;
        ensures \result == \null || scheduler_well_formed(\result);
        ensures \result != \null ==> \result->now == 0;
        ensures \result != \null ==> \result->running == 0;
        ensures \result != \null ==> scheduler_handlers_cleared(\result);

    complete behaviors;
    disjoint behaviors;
*/
Scheduler *scheduler_create(size_t capacity);
```

### `scheduler_free`

```c
/*@
    assigns \nothing;
*/
void scheduler_free(Scheduler *s);
```

Implementation rule: accept `NULL`; otherwise free pending event records,
queue storage, and scheduler storage. Do not free packets or opaque data stored
inside events.

### `scheduler_register`

```c
/*@
    behavior null_or_bad_type:
        assumes s == \null || type < 0 || type >= EVT_TYPE_COUNT;
        assigns \nothing;

    behavior valid:
        assumes scheduler_well_formed(s);
        assumes 0 <= type && type < EVT_TYPE_COUNT;
        assigns s->handlers[(int)type];
        ensures s->handlers[(int)type].fn == fn;
        ensures s->handlers[(int)type].ctx == ctx;

    complete behaviors;
    disjoint behaviors;
*/
void scheduler_register(Scheduler   *s,
                        EventType    type,
                        EventHandler fn,
                        void        *ctx);
```

### `scheduler_schedule`

```c
/*@
    behavior null:
        assumes s == \null || e == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes scheduler_well_formed(s) && \valid(e);
        assigns s->eq->events, s->eq->events[0 .. s->eq->capacity - 1],
                s->eq->count, s->eq->capacity;
        ensures \result == 0 || \result == -1;
        ensures \result == 0 ==> s->eq->count == \old(s->eq->count) + 1;
        ensures \result == -1 ==> s->eq->count == \old(s->eq->count);
        ensures \result == 0 ==> scheduler_well_formed(s);

    complete behaviors;
    disjoint behaviors;
*/
int scheduler_schedule(Scheduler *s, Event *e);
```

### `scheduler_step`

```c
/*@
    requires scheduler_well_formed(s);
    assigns s->eq->events[0 .. s->eq->capacity - 1],
            s->eq->count,
            s->now;
    ensures \result == 0 || \result == 1;
    ensures \result == 0 ==> s->eq->count == \old(s->eq->count);
    ensures \result == 1 ==> s->eq->count == \old(s->eq->count) - 1;
    ensures s->now >= \old(s->now);
    ensures scheduler_well_formed(s);
*/
int scheduler_step(Scheduler *s);
```

Additional required proof/test property:

- If the popped event has `handler != NULL`, that handler is called and the
  fallback table is not used.
- If the popped event has no per-event handler and has a valid type with a
  fallback handler, the fallback handler is called.
- If event type is invalid, no fallback table access occurs.

### `scheduler_run`

```c
/*@
    requires scheduler_well_formed(s);
    assigns s->eq->events[0 .. s->eq->capacity - 1],
            s->eq->count,
            s->now,
            s->running;
    ensures \result == 0;
    ensures s->running == 0;
    ensures scheduler_well_formed(s);
*/
int scheduler_run(Scheduler *s);
```

Additional required proof/test property:

- On return, either the queue is empty or `scheduler_stop` caused the loop to
  stop.

### `scheduler_stop`

```c
/*@
    behavior null:
        assumes s == \null;
        assigns \nothing;

    behavior valid:
        assumes \valid(s);
        assigns s->running;
        ensures s->running == 0;

    complete behaviors;
    disjoint behaviors;
*/
void scheduler_stop(Scheduler *s);
```

### `scheduler_now`

```c
/*@
    behavior null:
        assumes s == \null;
        assigns \nothing;
        ensures \result == 0;

    behavior valid:
        assumes \valid_read(s);
        assigns \nothing;
        ensures \result == s->now;

    complete behaviors;
    disjoint behaviors;
*/
uint64_t scheduler_now(const Scheduler *s);
```

## KLEVA Verification Plan

Minimum KLEVA tests:

1. `scheduler_create(0)` returns `NULL`.
2. `scheduler_create(1)` returns either `NULL` or a scheduler with `now == 0`.
3. Successful create sets `running == 0`.
4. Successful create clears every fallback handler slot.
5. `scheduler_register(NULL, ...)` does not crash.
6. `scheduler_register` ignores negative event types.
7. `scheduler_register` ignores `type >= EVT_TYPE_COUNT`.
8. Valid `scheduler_register` stores `fn`.
9. Valid `scheduler_register` stores `ctx`.
10. `scheduler_schedule(NULL, event)` returns `-1`.
11. `scheduler_schedule(s, NULL)` returns `-1`.
12. Successful schedule increments queue count.
13. Failed schedule leaves queue count unchanged.
14. `scheduler_step` on an empty queue returns `0`.
15. `scheduler_step` with one event returns `1` and decrements queue count.
16. `scheduler_step` advances `now` to a future event timestamp.
17. `scheduler_step` does not move `now` backward for an old event.
18. Per-event handler is called before fallback dispatch.
19. Fallback handler is called when event handler is `NULL`.
20. Invalid event type does not index the handler table.
21. `scheduler_stop(NULL)` does not crash.
22. `scheduler_stop(s)` sets `running == 0`.
23. `scheduler_now(NULL)` returns `0`.
24. `scheduler_now(s)` returns `s->now`.
25. `scheduler_free(NULL)` does not crash.

## Common Mistakes

- Do not execute both per-event and fallback handlers for the same event.
- Do not move `now` backward.
- Do not free packets from `scheduler_step` or `scheduler_free`.
- Do not index `handlers[type]` unless `type` is valid.
- Do not treat `running == 0` as meaning the queue is empty.
- Do not make the scheduler know concrete protocol context types.
