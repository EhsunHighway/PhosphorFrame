# Module 02 - Event System

**Files:** `src/engine/event.c`, `src/engine/event.h`
**Status:** Implemented
**Depends on:** C standard library: `stdint.h`, `stddef.h`, `stdlib.h`,
`string.h`

## Concepts First

A discrete-event simulator does not advance time continuously. It jumps from
one scheduled event to the next scheduled event.

An `Event` is a timestamped instruction:

```text
At simulated time T, something should happen.
```

Examples:

```text
time 100: deliver packet to interface eth1
time 250: run TCP retransmission timer
time 300: age switch MAC table
time 500: send routing update
```

The event module provides two things:

- the `Event` record
- a sorted `EventQueue`

The scheduler owns the main simulation loop. The event module only stores,
orders, creates, and frees event records.

### Virtual Time

`timestamp` is simulated time, not wall-clock time.

If the event queue contains events at times `10`, `15`, and `40`, the simulator
can jump directly from `10` to `15` to `40`. No real sleeping is required.

### Event Type Versus Callback

`EventType` classifies the event:

```text
EVT_PACKET_RECEIVE
EVT_TIMER_EXPIRED
EVT_TCP_RETRANSMIT
EVT_RIP_UPDATE
```

This is useful for logging, filtering, tests, and fallback dispatch.

But a type alone is not always enough. There may be many TCP retransmission
events alive at the same time, each belonging to a different TCP connection.
That is why an event can also carry:

```c
EventCallback handler;
void         *handler_ctx;
```

If `handler != NULL`, the scheduler can call that exact function with the exact
context for this one event.

### Opaque Pointers

`Event` stores `src_device`, `dst_device`, `packet`, and `data` as `void *`.

That is deliberate. `event.h` should not include every network, protocol, and
display header. The module that creates or handles the event knows the concrete
types and casts them back.

Example:

```text
packet receive event:
  src_device might be Interface *
  dst_device might be Interface *
  packet     might be Packet *
  data       might be NULL
```

The event module does not inspect those pointers.

### Sorted Array Queue

The current queue is a sorted array of `Event *`.

```text
events[0]  timestamp 10
events[1]  timestamp 15
events[2]  timestamp 15
events[3]  timestamp 40
```

`event_queue_push` inserts the new event at the correct position by shifting
later events to the right.

`event_queue_pop` removes `events[0]`, shifts the rest left, and returns the
earliest event.

This is `O(n)`, but simple and deterministic for this simulator.

### Tie Order

When two events have the same timestamp, insertion order is preserved.

The implementation shifts only events with a strictly greater timestamp:

```c
eq->events[i]->timestamp > e->timestamp
```

Equal timestamps are not shifted past each other.

## Purpose

The event module provides basic event allocation and a timestamp-sorted queue.

It provides:

- event creation
- event creation with per-event callback/context
- shallow event free
- event queue creation
- event queue destruction
- sorted push
- earliest-event pop
- earliest-event peek
- empty check

It does not:

- execute callbacks
- advance scheduler time
- own packet memory
- own protocol-specific `data`
- know concrete types behind `void *`
- free events still stored inside a queue when the queue itself is freed

## Architecture Boundary

| Responsibility | Owner |
| --- | --- |
| Store timestamp and dispatch metadata | `Event` |
| Keep events sorted by timestamp | `EventQueue` |
| Decide when to pop and execute events | Scheduler |
| Call `handler` or type-registered handler | Scheduler |
| Own packet lifetime | Protocol/link/scheduler path, not `event_free` |
| Own `data` lifetime | Event creator/handler |
| Free queued event objects | Queue owner before or after queue destruction |

`event_queue_free` frees the queue array and the queue struct. It does not walk
the queue and free each `Event *`.

## Data Model

### `EventType`

`EventType` is an enum of known event classes. `EVT_TYPE_COUNT` is a sentinel
and must remain last because scheduler handler tables can use it as their size.

Important groups:

- packet delivery events
- ARP events
- link state events
- generic timers
- render/reset events
- MAC aging
- TCP retransmission
- routing protocol timers and updates
- NAT garbage collection

### `EventCallback`

```c
typedef void (*EventCallback)(const Event *e, void *ctx);
```

The event is passed as read-only to the callback. The context pointer is opaque
and belongs to the code that scheduled the event.

### `Event`

```c
struct Event {
    EventType     type;
    uint64_t      timestamp;
    void         *src_device;
    void         *dst_device;
    void         *packet;
    void         *data;
    EventCallback handler;
    void         *handler_ctx;
};
```

Field meanings:

| Field | Meaning |
| --- | --- |
| `type` | Classification key for event kind. |
| `timestamp` | Simulated time when the event should fire. |
| `src_device` | Opaque source pointer, typed by the event creator. |
| `dst_device` | Opaque destination pointer, typed by the event creator. |
| `packet` | Opaque packet pointer, usually `Packet *`. |
| `data` | Opaque extra payload or state pointer. |
| `handler` | Optional event-specific callback. |
| `handler_ctx` | Opaque context passed to `handler`. |

### `EventQueue`

```c
typedef struct EventQueue {
    Event   **events;
    size_t    count;
    size_t    capacity;
} EventQueue;
```

`events` points to an allocated array of `Event *`.

`count` is the number of occupied slots.

`capacity` is the number of allocated slots.

The valid occupied range is:

```text
events[0 .. count - 1]
```

The queue invariant is:

```text
0 <= count <= capacity
events[0 .. count - 1] are sorted by nondecreasing timestamp
```

## Ownership And Lifetime

`event_create` and `event_create_callback` allocate one `Event`.

`event_free` frees only that `Event`.

`event_free` does not free:

- `event->packet`
- `event->data`
- `event->src_device`
- `event->dst_device`
- `event->handler_ctx`

`event_queue_create` allocates:

- one `EventQueue`
- one `Event **` array

`event_queue_free` frees the array and the queue. It does not free queued event
objects.

The current implementation does not defend against `NULL` in
`event_queue_push`, `event_queue_pop`, `event_queue_peek`, or
`event_queue_is_empty`. Callers must pass a valid queue.

## Public API

```c
EventQueue *event_queue_create(size_t capacity);

void        event_queue_free(EventQueue *eq);

int         event_queue_push(EventQueue *eq, Event *e);

Event      *event_queue_pop(EventQueue *eq);

Event      *event_queue_peek(const EventQueue *eq);

int         event_queue_is_empty(const EventQueue *eq);

Event      *event_create(EventType  type,
                         uint64_t   timestamp,
                         void      *src,
                         void      *dst,
                         void      *packet,
                         void      *data);

Event      *event_create_callback(EventType     type,
                                  uint64_t      timestamp,
                                  void         *src,
                                  void         *dst,
                                  void         *packet,
                                  void         *data,
                                  EventCallback handler,
                                  void         *handler_ctx);

void        event_free(Event *e);
```

## Function Behavior

### `event_create_callback`

Required behavior:

- Allocate one `Event`.
- If allocation fails, return `NULL`.
- Store all arguments directly into the new event.
- Return the new event.

The event module does not validate whether `type` is a known enum value.

### `event_create`

Required behavior:

- Create an event with the same fields as `event_create_callback`.
- Set `handler == NULL`.
- Set `handler_ctx == NULL`.

This function exists for callers that want type-based scheduler dispatch instead
of a per-event callback.

### `event_free`

Required behavior:

- If `e == NULL`, return immediately.
- Free only the `Event` object.

### `event_queue_create`

Required behavior:

- Caller should pass `capacity > 0`.
- Allocate one `EventQueue`.
- Allocate an array of `capacity` `Event *` slots.
- If either allocation fails, return `NULL` and leak nothing from this call.
- Set `count == 0`.
- Set `capacity == capacity`.
- Store the array pointer in `events`.

The implementation currently assumes useful queues have nonzero capacity. New
callers and tests should treat `capacity == 0` as outside the supported API.

### `event_queue_free`

Required behavior:

- If `eq == NULL`, return immediately.
- Free `eq->events`.
- Free `eq`.
- Do not free any `Event *` stored in the queue.

### `event_queue_push`

Required behavior:

- Caller must pass a valid queue.
- Caller must pass a valid event.
- If `count >= capacity`, grow the `events` array with `realloc`.
- Growth doubles the previous capacity.
- If growth fails, return `-1` and leave `count` unchanged.
- Insert the new event so timestamps remain sorted in nondecreasing order.
- Preserve insertion order for events with equal timestamps.
- Increment `count`.
- Return `0`.

The implementation stores event pointers. It does not copy `Event` objects.

### `event_queue_pop`

Required behavior:

- Caller must pass a valid queue.
- If `count == 0`, return `NULL`.
- Save `events[0]`.
- Shift remaining event pointers left by one slot.
- Decrement `count`.
- Return the saved earliest event.

The returned event is no longer owned by the queue.

### `event_queue_peek`

Required behavior:

- Caller must pass a valid queue.
- If `count == 0`, return `NULL`.
- Return `events[0]`.
- Do not change queue state.

The returned event remains stored in the queue.

### `event_queue_is_empty`

Required behavior:

- Caller must pass a valid queue.
- Return `1` if `count == 0`.
- Return `0` otherwise.
- Do not change queue state.

## Flow Charts

### Scheduling A Delayed Packet Delivery

```text
link computes arrival timestamp
  |
  +-- event_create_callback(...)
  |     |
  |     +-- event stores timestamp, packet, dst interface, handler, context
  |
  +-- scheduler_schedule(...)
        |
        +-- event_queue_push(...)
              |
              +-- insert by timestamp
```

### Scheduler Pulls The Next Event

```text
scheduler step
  |
  +-- event_queue_pop(queue)
  |     |
  |     +-- returns earliest event or NULL
  |
  +-- if event exists:
        |
        +-- scheduler time becomes event timestamp
        |
        +-- if event->handler exists:
        |     call event->handler(event, event->handler_ctx)
        |
        +-- otherwise:
              use scheduler's registered handler for event->type
```

### Queue Push

```text
event_queue_push(eq, e)
  |
  +-- if full:
  |     |
  |     +-- realloc to double capacity
  |     +-- fail: return -1
  |
  +-- scan backward while existing timestamp > new timestamp
  |
  +-- shift greater timestamps right
  |
  +-- place new event
  |
  +-- count++
  |
  +-- return 0
```

## ACSL Contracts

The contracts belong in `event.h`. The queue stores pointers, so the useful
properties are queue shape, sortedness, count changes, and shallow ownership.

### Shared Predicates

```c
/*@
    predicate event_type_valid(EventType type) =
        0 <= type && type < EVT_TYPE_COUNT;

    predicate event_queue_storage(EventQueue *eq) =
        \valid(eq) &&
        eq->events != \null &&
        0 <= eq->count &&
        eq->count <= eq->capacity &&
        eq->capacity > 0 &&
        \valid(eq->events + (0 .. eq->capacity - 1));

    predicate event_queue_sorted(EventQueue *eq) =
        event_queue_storage(eq) &&
        (\forall integer i, j;
            0 <= i && i <= j && j < eq->count ==>
                eq->events[i]->timestamp <= eq->events[j]->timestamp);

    predicate event_queue_entries_valid(EventQueue *eq) =
        event_queue_storage(eq) &&
        (\forall integer i;
            0 <= i && i < eq->count ==> \valid(eq->events[i]));

    predicate event_queue_well_formed(EventQueue *eq) =
        event_queue_sorted(eq) && event_queue_entries_valid(eq);
*/
```

### `event_queue_create`

```c
/*@
    requires capacity > 0;
    allocates \result;
    ensures \result == \null || event_queue_storage(\result);
    ensures \result != \null ==> \result->count == 0;
    ensures \result != \null ==> \result->capacity == capacity;
*/
EventQueue *event_queue_create(size_t capacity);
```

### `event_queue_free`

```c
/*@
    assigns \nothing;
*/
void event_queue_free(EventQueue *eq);
```

ACSL does not model the shallow free cleanly in the lightweight style used here.
The implementation rule is: free only `eq->events` and `eq`; do not free queued
events.

### `event_queue_push`

```c
/*@
    requires event_queue_well_formed(eq);
    requires \valid(e);
    assigns eq->events, eq->events[0 .. eq->capacity - 1],
            eq->count, eq->capacity;
    ensures \result == 0 || \result == -1;
    ensures \result == 0 ==> eq->count == \old(eq->count) + 1;
    ensures \result == -1 ==> eq->count == \old(eq->count);
    ensures \result == 0 ==> event_queue_well_formed(eq);
    ensures \result == 0 ==>
        \exists integer i; 0 <= i && i < eq->count && eq->events[i] == e;
*/
int event_queue_push(EventQueue *eq, Event *e);
```

Additional required proof/test property:

- If two events have equal timestamps, their relative insertion order is
  preserved.
- If `realloc` fails, the old `events` pointer remains the queue storage.

### `event_queue_pop`

```c
/*@
    requires event_queue_well_formed(eq);
    assigns eq->events[0 .. eq->capacity - 1], eq->count;
    ensures \result == \null || \valid(\result);
    ensures \old(eq->count) == 0 ==> \result == \null;
    ensures \old(eq->count) == 0 ==> eq->count == \old(eq->count);
    ensures \old(eq->count) > 0 ==> \result == \old(eq->events[0]);
    ensures \old(eq->count) > 0 ==> eq->count == \old(eq->count) - 1;
    ensures event_queue_well_formed(eq);
*/
Event *event_queue_pop(EventQueue *eq);
```

### `event_queue_peek`

```c
/*@
    requires event_queue_well_formed((EventQueue *)eq);
    assigns \nothing;
    ensures \result == \null || \valid_read(\result);
    ensures eq->count == 0 ==> \result == \null;
    ensures eq->count > 0 ==> \result == eq->events[0];
*/
Event *event_queue_peek(const EventQueue *eq);
```

### `event_queue_is_empty`

```c
/*@
    requires event_queue_storage((EventQueue *)eq);
    assigns \nothing;
    ensures \result == 1 <==> eq->count == 0;
*/
int event_queue_is_empty(const EventQueue *eq);
```

### `event_create`

```c
/*@
    allocates \result;
    ensures \result == \null || \valid(\result);
    ensures \result != \null ==> \result->type == type;
    ensures \result != \null ==> \result->timestamp == timestamp;
    ensures \result != \null ==> \result->src_device == src;
    ensures \result != \null ==> \result->dst_device == dst;
    ensures \result != \null ==> \result->packet == packet;
    ensures \result != \null ==> \result->data == data;
    ensures \result != \null ==> \result->handler == \null;
    ensures \result != \null ==> \result->handler_ctx == \null;
*/
Event *event_create(EventType type,
                    uint64_t  timestamp,
                    void     *src,
                    void     *dst,
                    void     *packet,
                    void     *data);
```

### `event_create_callback`

```c
/*@
    allocates \result;
    ensures \result == \null || \valid(\result);
    ensures \result != \null ==> \result->type == type;
    ensures \result != \null ==> \result->timestamp == timestamp;
    ensures \result != \null ==> \result->src_device == src;
    ensures \result != \null ==> \result->dst_device == dst;
    ensures \result != \null ==> \result->packet == packet;
    ensures \result != \null ==> \result->data == data;
    ensures \result != \null ==> \result->handler == handler;
    ensures \result != \null ==> \result->handler_ctx == handler_ctx;
*/
Event *event_create_callback(EventType     type,
                             uint64_t      timestamp,
                             void         *src,
                             void         *dst,
                             void         *packet,
                             void         *data,
                             EventCallback handler,
                             void         *handler_ctx);
```

### `event_free`

```c
/*@
    assigns \nothing;
*/
void event_free(Event *e);
```

Implementation rule: free only the event record. Do not free any opaque pointer
stored in the event.

## KLEVA Verification Plan

Minimum KLEVA tests:

1. `event_create` returns either `NULL` or an event with all fields copied.
2. `event_create` sets `handler == NULL`.
3. `event_create` sets `handler_ctx == NULL`.
4. `event_create_callback` preserves `handler`.
5. `event_create_callback` preserves `handler_ctx`.
6. `event_free(NULL)` does not crash.
7. `event_queue_create(1)` returns either `NULL` or a queue with `count == 0`.
8. Created queue capacity equals requested capacity.
9. Push into empty queue succeeds and increments count.
10. Push keeps timestamps sorted.
11. Push preserves equal-timestamp insertion order.
12. Push grows a full queue.
13. Push failure from `realloc` leaves `count` unchanged.
14. Pop from empty queue returns `NULL` and leaves count unchanged.
15. Pop from non-empty queue returns the earliest event.
16. Pop decrements count by one.
17. Peek from empty queue returns `NULL`.
18. Peek from non-empty queue returns earliest event without changing count.
19. `event_queue_is_empty` returns `1` exactly when `count == 0`.
20. `event_queue_free(NULL)` does not crash.

## Common Mistakes

- Do not free `event->packet` inside `event_free`.
- Do not free `event->data` inside `event_free`.
- Do not assume `EventType` alone is enough for owner-specific timers.
- Do not make `event.h` include packet, interface, TCP, RIP, or scheduler
  headers just to type the opaque pointers.
- Do not break FIFO order for equal timestamps.
- Do not make `event_queue_free` silently free events unless every caller is
  updated to transfer ownership to the queue.
