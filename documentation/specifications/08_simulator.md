# Module 08 - Simulator

**Files:** `src/engine/simulator.c`, `src/engine/simulator.h`
**Status:** Implemented
**Depends on:** `topology`, `scheduler`, `event`, `device`, `packet`

## Concepts First

The simulator is the top-level runtime object.

Topology answers:

```text
What devices and links exist?
```

Scheduler answers:

```text
What event happens next, and when?
```

Simulator binds them together:

```text
Simulator
  |
  +-- Topology
  |
  +-- Scheduler
  |
  +-- optional end time
```

Most higher-level modules want one context pointer that can reach both the
network graph and the event engine. That context is usually `Simulator *`.

### Simulator As Runtime Context

Protocol/event callbacks often receive a `void *ctx`. When that context is a
`Simulator *`, the callback can reach:

- `sim->topo` for devices and links
- `sim->sched` for time and event scheduling

This avoids global variables while still giving protocol code access to shared
simulation state.

### Ownership Root

After `simulator_create(topo, sched)` succeeds, the simulator owns both
pointers.

`simulator_free` frees:

1. scheduler
2. topology
3. simulator

The current order is scheduler first, then topology. That is acceptable because
`scheduler_free` shallow-frees pending events and does not dereference topology
objects stored inside them.

### End Time

`end_time` is a stop condition for `simulator_run`.

```text
end_time == 0  means no time limit
end_time > 0   means stop when scheduler time reaches it
```

The simulator compares `end_time` to `sim->sched->now`.

### Inject Packet Helper

`simulator_inject_packet` is a convenience helper for tests or simple drivers.

The current implementation creates an event with:

```c
EVT_PACKET_SEND
timestamp = sim->sched->now + delay_us
src_device = src
dst_device = dst
packet = pkt
data = NULL
handler = NULL
handler_ctx = NULL
```

It then schedules that event with the scheduler.

This helper does not resolve interfaces, clone packets, call Ethernet, or attach
a per-event callback. A registered fallback handler for `EVT_PACKET_SEND` must
exist if this event should do real work.

## Purpose

The simulator module provides one owner/context object for running a simulation.

It provides:

- simulator allocation
- simulator destruction
- run loop
- single-step execution
- stop signal
- end-time setter
- current-time accessor
- packet-event injection helper
- fallback handler registration wrapper

It does not:

- create topology or scheduler internally
- parse packets
- deliver packets by itself
- choose routes
- resolve interfaces
- clone injected packets
- free injected packets on schedule failure in the current implementation

## Architecture Boundary

| Responsibility | Owner |
| --- | --- |
| Own topology after simulator creation | Simulator |
| Own scheduler after simulator creation | Simulator |
| Store simulation end time | Simulator |
| Execute event dispatch | Scheduler |
| Store network graph | Topology |
| Register fallback event handlers | Scheduler, through Simulator wrapper |
| Create real link delivery events | Link |
| Interpret injected packet event | Registered handler |

Simulator is orchestration glue. It should not absorb protocol or forwarding
logic.

## Data Model

### `Simulator`

```c
typedef struct Simulator {
    Topology  *topo;
    Scheduler *sched;
    uint64_t   end_time;
} Simulator;
```

Field meanings:

| Field | Meaning |
| --- | --- |
| `topo` | Owned topology pointer. |
| `sched` | Owned scheduler pointer. |
| `end_time` | Stop time. `0` means no limit. |

Required shape:

```text
topo != NULL
sched != NULL
end_time is any uint64_t
```

## Ownership And Lifetime

`simulator_create` does not create a topology or scheduler. It receives them.

On success, ownership transfers to the simulator.

On failure, ownership does not transfer. The caller remains responsible for the
topology and scheduler.

`simulator_free(NULL)` is valid and does nothing.

`simulator_free` frees the scheduler before the topology in the current
implementation.

## Public API

```c
Simulator *simulator_create(Topology *topo, Scheduler *sched);

void       simulator_free(Simulator *sim);

int        simulator_run(Simulator *sim);

int        simulator_step(Simulator *sim);

void       simulator_stop(Simulator *sim);

void       simulator_set_end_time(Simulator *sim, uint64_t end_us);

uint64_t   simulator_now(const Simulator *sim);

int        simulator_inject_packet(Simulator *sim,
                                   Device    *src,
                                   Device    *dst,
                                   Packet    *pkt,
                                   uint64_t   delay_us);

void       simulator_register_handler(Simulator   *sim,
                                      EventType    type,
                                      EventHandler fn,
                                      void        *ctx);
```

## Function Behavior

### `simulator_create`

Required behavior:

- If `topo == NULL`, return `NULL`.
- If `sched == NULL`, return `NULL`.
- Allocate one `Simulator`.
- If allocation fails, return `NULL`.
- Store `topo`.
- Store `sched`.
- Set `end_time == 0`.
- Return the simulator.

### `simulator_free`

Required behavior:

- If `sim == NULL`, return immediately.
- Call `scheduler_free(sim->sched)`.
- Call `topology_free(sim->topo)`.
- Free `sim`.

### `simulator_run`

Required behavior:

- Caller must pass a valid simulator with a valid scheduler.
- Set `sim->sched->running = 1`.
- While `sim->sched->running == 1`:
  - call `scheduler_step(sim->sched)`
  - if step returns `0`, set `running = 0`
  - if `end_time > 0` and `sched->now >= end_time`, set `running = 0`
- Return `0`.

The current implementation does not check `sim == NULL`.

### `simulator_step`

Required behavior:

- If `sim == NULL`, return `-1`.
- Otherwise return `scheduler_step(sim->sched)`.

### `simulator_stop`

Required behavior:

- If `sim == NULL`, return immediately.
- Otherwise call `scheduler_stop(sim->sched)`.

### `simulator_set_end_time`

Required behavior:

- If `sim == NULL`, return immediately.
- Otherwise set `sim->end_time = end_us`.

`end_us == 0` clears the time limit.

### `simulator_now`

Required behavior:

- If `sim == NULL`, return `0`.
- Otherwise return `sim->sched->now`.

### `simulator_inject_packet`

Required behavior:

- If `sim == NULL`, return `-1`.
- If `src == NULL`, return `-1`.
- If `dst == NULL`, return `-1`.
- If `pkt == NULL`, return `-1`.
- Create an event with:
  - type `EVT_PACKET_SEND`
  - timestamp `sim->sched->now + delay_us`
  - source pointer `src`
  - destination pointer `dst`
  - packet pointer `pkt`
  - data pointer `NULL`
- If event creation fails, return `-1`.
- Schedule the event with `scheduler_schedule`.
- Return the scheduler result.

Current implementation note: if event creation succeeds but scheduling fails,
the event is not freed inside `simulator_inject_packet`. That is a real behavior
to fix in code later, not something the spec should hide.

### `simulator_register_handler`

Required behavior:

- If `sim == NULL`, return without changing state.
- If `fn == NULL`, return without changing state.
- If `type < 0` or `type >= EVT_TYPE_COUNT`, return without changing state.
- Otherwise call `scheduler_register(sim->sched, type, fn, ctx)`.

## Flow Charts

### Create Simulator

```text
simulator_create(topo, sched)
  |
  +-- reject NULL topo or sched
  |
  +-- allocate Simulator
  |
  +-- sim->topo = topo
  +-- sim->sched = sched
  +-- sim->end_time = 0
  |
  +-- return Simulator
```

### Run Loop

```text
simulator_run(sim)
  |
  +-- sched->running = 1
  |
  +-- while running == 1:
        |
        +-- step_state = scheduler_step(sched)
        |
        +-- if step_state == 0:
        |     running = 0
        |
        +-- if end_time > 0 and sched->now >= end_time:
              running = 0
  |
  +-- return 0
```

### Inject Packet Event

```text
simulator_inject_packet(sim, src, dst, pkt, delay_us)
  |
  +-- reject NULL inputs
  |
  +-- event_create(EVT_PACKET_SEND,
  |                sim->sched->now + delay_us,
  |                src,
  |                dst,
  |                pkt,
  |                NULL)
  |
  +-- scheduler_schedule(sim->sched, event)
  |
  +-- return scheduler result
```

## ACSL Contracts

The contracts belong in `simulator.h`.

### Shared Predicates

```c
/*@
    predicate simulator_well_formed(Simulator *sim) =
        \valid(sim) &&
        topology_well_formed(sim->topo) &&
        scheduler_well_formed(sim->sched);
*/
```

### `simulator_create`

```c
/*@
    behavior null:
        assumes topo == \null || sched == \null;
        assigns \nothing;
        ensures \result == \null;

    behavior ok:
        assumes topology_well_formed(topo);
        assumes scheduler_well_formed(sched);
        allocates \result;
        ensures \result == \null || simulator_well_formed(\result);
        ensures \result != \null ==> \result->topo == topo;
        ensures \result != \null ==> \result->sched == sched;
        ensures \result != \null ==> \result->end_time == 0;

    complete behaviors;
    disjoint behaviors;
*/
Simulator *simulator_create(Topology *topo, Scheduler *sched);
```

### `simulator_free`

```c
/*@
    assigns \nothing;
*/
void simulator_free(Simulator *sim);
```

Implementation rule: accept `NULL`; otherwise free scheduler, topology, and
simulator storage.

### `simulator_run`

```c
/*@
    requires simulator_well_formed(sim);
    assigns sim->sched->eq->events[0 .. sim->sched->eq->capacity - 1],
            sim->sched->eq->count,
            sim->sched->now,
            sim->sched->running;
    ensures \result == 0;
    ensures sim->sched->running == 0;
    ensures simulator_well_formed(sim);
*/
int simulator_run(Simulator *sim);
```

Additional required proof/test property:

- On exit, the queue is empty, the simulator was stopped, or end time was
  reached.

### `simulator_step`

```c
/*@
    behavior null:
        assumes sim == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes simulator_well_formed(sim);
        assigns sim->sched->eq->events[0 .. sim->sched->eq->capacity - 1],
                sim->sched->eq->count,
                sim->sched->now;
        ensures \result == 0 || \result == 1;
        ensures \result == 0 ==> sim->sched->eq->count == \old(sim->sched->eq->count);
        ensures \result == 1 ==> sim->sched->eq->count == \old(sim->sched->eq->count) - 1;
        ensures sim->sched->now >= \old(sim->sched->now);
        ensures simulator_well_formed(sim);

    complete behaviors;
    disjoint behaviors;
*/
int simulator_step(Simulator *sim);
```

### `simulator_stop`

```c
/*@
    behavior null:
        assumes sim == \null;
        assigns \nothing;

    behavior valid:
        assumes simulator_well_formed(sim);
        assigns sim->sched->running;
        ensures sim->sched->running == 0;

    complete behaviors;
    disjoint behaviors;
*/
void simulator_stop(Simulator *sim);
```

### `simulator_set_end_time`

```c
/*@
    behavior null:
        assumes sim == \null;
        assigns \nothing;

    behavior valid:
        assumes \valid(sim);
        assigns sim->end_time;
        ensures sim->end_time == end_us;

    complete behaviors;
    disjoint behaviors;
*/
void simulator_set_end_time(Simulator *sim, uint64_t end_us);
```

### `simulator_now`

```c
/*@
    behavior null:
        assumes sim == \null;
        assigns \nothing;
        ensures \result == 0;

    behavior valid:
        assumes \valid_read(sim) && \valid_read(sim->sched);
        assigns \nothing;
        ensures \result == sim->sched->now;

    complete behaviors;
    disjoint behaviors;
*/
uint64_t simulator_now(const Simulator *sim);
```

### `simulator_inject_packet`

```c
/*@
    behavior null:
        assumes sim == \null || src == \null || dst == \null || pkt == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes simulator_well_formed(sim);
        assumes device_well_formed(src);
        assumes device_well_formed(dst);
        assumes packet_layout(pkt);
        assigns sim->sched->eq->events,
                sim->sched->eq->events[0 .. sim->sched->eq->capacity - 1],
                sim->sched->eq->count,
                sim->sched->eq->capacity;
        ensures \result == 0 || \result == -1;
        ensures \result == 0 ==> sim->sched->eq->count == \old(sim->sched->eq->count) + 1;
        ensures \result == -1 ==> sim->sched->eq->count == \old(sim->sched->eq->count);

    complete behaviors;
    disjoint behaviors;
*/
int simulator_inject_packet(Simulator *sim,
                            Device *src,
                            Device *dst,
                            Packet *pkt,
                            uint64_t delay_us);
```

Additional required proof/test property:

- On success, the queued event type is `EVT_PACKET_SEND`.
- On success, the queued event timestamp is old `sim->sched->now + delay_us`.
- On success, the queued event stores the exact `src`, `dst`, and `pkt`
  pointers.

### `simulator_register_handler`

```c
/*@
    behavior null_or_bad:
        assumes sim == \null || type < 0 || type >= EVT_TYPE_COUNT || fn == \null;
        assigns \nothing;

    behavior valid:
        assumes simulator_well_formed(sim);
        assumes 0 <= type && type < EVT_TYPE_COUNT;
        assumes fn != \null;
        assigns sim->sched->handlers[(int)type];
        ensures sim->sched->handlers[(int)type].fn == fn;
        ensures sim->sched->handlers[(int)type].ctx == ctx;

    complete behaviors;
    disjoint behaviors;
*/
void simulator_register_handler(Simulator *sim,
                                EventType type,
                                EventHandler fn,
                                void *ctx);
```

## KLEVA Verification Plan

Minimum KLEVA tests:

1. `simulator_create(NULL, sched)` returns `NULL`.
2. `simulator_create(topo, NULL)` returns `NULL`.
3. Successful create stores topology pointer.
4. Successful create stores scheduler pointer.
5. Successful create sets `end_time == 0`.
6. `simulator_free(NULL)` does not crash.
7. `simulator_step(NULL)` returns `-1`.
8. `simulator_step(valid)` delegates to scheduler and preserves scheduler
   step result.
9. `simulator_stop(NULL)` does not crash.
10. `simulator_stop(valid)` sets scheduler running to `0`.
11. `simulator_set_end_time(NULL, value)` is a no-op.
12. `simulator_set_end_time(valid, value)` stores the value.
13. `simulator_now(NULL)` returns `0`.
14. `simulator_now(valid)` returns scheduler time.
15. `simulator_inject_packet` rejects NULL simulator, source, destination, or
   packet.
16. Successful injection increments scheduler queue count.
17. Successful injection creates `EVT_PACKET_SEND`.
18. Successful injection stores timestamp `old now + delay_us`.
19. Successful injection stores exact source, destination, and packet pointers.
20. `simulator_register_handler` rejects NULL simulator.
21. `simulator_register_handler` rejects NULL function.
22. `simulator_register_handler` rejects invalid event type.
23. Valid register stores function and context in scheduler.
24. `simulator_run` stops when queue becomes empty.
25. `simulator_run` stops when end time is reached.

## Common Mistakes

- Do not say `simulator_inject_packet` creates `EVT_PACKET_RECEIVE`; current
  code creates `EVT_PACKET_SEND`.
- Do not say injected packets are cloned; current code stores the original
  packet pointer.
- Do not hide the schedule-failure event leak in `simulator_inject_packet`.
- Do not put protocol logic inside the simulator.
- Do not treat `end_time == 0` as an immediate stop condition.
