# Module 05 - Link

**Files:** `src/network/link.c`, `src/network/link.h`
**Status:** Implemented
**Depends on:** `interface`, `packet`, `event`, `scheduler`

## Concepts First

A link is the simulated cable between two interfaces.

```text
Interface A <---- Link ----> Interface B
```

When one side transmits a packet, the link does not deliver it immediately.
Instead, it creates a future receive event for the other side.

That is the core discrete-event model:

```text
send now
  |
  +-- link computes arrival time
  |
  +-- scheduler stores receive event
  |
  +-- later, scheduler executes receive callback
```

### Link Versus Interface

The interface is the endpoint. The link is the connection between endpoints.

The link stores borrowed pointers to exactly two interfaces:

```c
Interface *end_a;
Interface *end_b;
```

The link does not own or free those interfaces.

### Symmetric Link

The current `Link` object is symmetric:

- same bandwidth in both directions
- same delay in both directions
- same loss rate in both directions
- one shared up/down flag

If a future simulation needs asymmetric links, it should introduce a different
model instead of hiding asymmetry inside this struct.

### Packet Ownership Across A Link

`link_transmit` clones the packet before scheduling delivery.

```text
caller owns original packet
scheduled receive event owns/contains clone
```

This prevents the sender and receiver from accidentally sharing the same
`Packet *`. The caller keeps responsibility for the original packet. The
receive path becomes responsible for the clone after the scheduled event fires.

### Receive Callback

The link does not know Ethernet. Instead, the caller supplies the callback that
should run when the packet arrives:

```c
EventCallback rx_handler;
void         *rx_ctx;
```

For Ethernet delivery, the caller usually passes `ethernet_receive_event` and a
simulator/network context. The link just stores those values in the event.

### Timing In Current Code

The current implementation schedules arrival at:

```text
arrival = now + link->delay_ms
```

`bandwidth_mbps` and `loss_rate` are stored but not used by
`link_transmit` yet.

Do not describe serialization delay or random packet loss as implemented
behavior until the code actually uses those fields.

## Purpose

The link module models a two-ended connection and delayed packet delivery.

It provides:

- link allocation
- link destruction
- link up/down setter and getter
- peer lookup
- packet clone and receive-event scheduling

It does not:

- own interfaces
- attach itself to interface fields
- parse Ethernet
- free the caller's original packet
- compute serialization delay in the current implementation
- apply random loss in the current implementation

## Architecture Boundary

| Responsibility | Owner |
| --- | --- |
| Store two link endpoints | Link |
| Store bandwidth/delay/loss fields | Link |
| Attach link pointer to interface | Caller/topology via `interface_set_link` |
| Clone transmitted packet | Link |
| Create receive event | Link |
| Schedule receive event | Link through Scheduler |
| Execute receive callback | Scheduler |
| Interpret received bytes | Callback/protocol path |
| Free original packet | Caller |
| Free delivered clone | Receive path |

`link.h` includes `interface.h`, `packet.h`, and `event.h`. It forward-declares
`struct Scheduler` to avoid exposing scheduler internals in the link header.

## Data Model

### `Link`

```c
typedef struct Link {
    Interface *end_a;
    Interface *end_b;
    uint32_t   bandwidth_mbps;
    uint32_t   delay_ms;
    float      loss_rate;
    int        up;
} Link;
```

Field meanings:

| Field | Meaning |
| --- | --- |
| `end_a` | Borrowed pointer to one endpoint. |
| `end_b` | Borrowed pointer to the other endpoint. |
| `bandwidth_mbps` | Stored bandwidth value. Not used by current transmit timing. |
| `delay_ms` | Stored one-way delay. Current transmit adds this directly to `now`. |
| `loss_rate` | Stored loss value. Not used by current transmit behavior. |
| `up` | Link administrative/operational flag; nonzero means usable. |

## Ownership And Lifetime

`link_create` allocates one `Link`.

`link_free` frees only the link object.

The link does not free:

- `end_a`
- `end_b`
- packets passed to `link_transmit`
- scheduler
- callback context

On successful `link_transmit`, the scheduled event carries a cloned packet. If
event creation or scheduling fails, `link_transmit` frees the clone before
returning `-1`.

## Public API

```c
Link      *link_create(Interface *end_a,
                       Interface *end_b,
                       uint32_t   bw,
                       uint32_t   delay,
                       float      loss_rate);

void       link_free(Link *link);

void       link_set_up(Link *link, int up);

int        link_is_up(const Link *link);

Interface *link_get_other_interface(const Link *link,
                                    const Interface *src);

int        link_transmit(Link             *link,
                         const Packet     *pkt,
                         const Interface  *src,
                         struct Scheduler *sched,
                         uint64_t          now,
                         EventCallback     rx_handler,
                         void             *rx_ctx);
```

## Function Behavior

### `link_create`

Required behavior:

- If `end_a == NULL`, return `NULL`.
- If `end_b == NULL`, return `NULL`.
- If `bw == 0`, return `NULL`.
- Allocate one `Link`.
- If allocation fails, return `NULL`.
- Store `end_a`.
- Store `end_b`.
- Store `bandwidth_mbps == bw`.
- Store `delay_ms == delay`.
- Store `loss_rate`.
- Set `up == 1`.

The function does not call `interface_set_link`. The caller must attach the link
to interfaces separately if that relationship should be visible from the
interface side.

### `link_free`

Required behavior:

- If `link == NULL`, return immediately.
- Free only the link object.

### `link_set_up`

Required behavior:

- If `link == NULL`, return without changing state.
- Otherwise set `link->up = up`.

### `link_is_up`

Required behavior:

- If `link == NULL`, return `0`.
- If `link->up != 0`, return `1`.
- Otherwise return `0`.

### `link_get_other_interface`

Required behavior:

- If `link == NULL`, return `NULL`.
- If `src == NULL`, return `NULL`.
- If `src == link->end_a`, return `link->end_b`.
- If `src == link->end_b`, return `link->end_a`.
- Otherwise return `NULL`.

This function compares interface pointers, not interface names or MAC
addresses.

### `link_transmit`

Required behavior:

- If `link == NULL`, return `-1`.
- If `pkt == NULL`, return `-1`.
- If `src == NULL`, return `-1`.
- If `sched == NULL`, return `-1`.
- If `link->up == 0`, return `-1`.
- Find destination with `link_get_other_interface(link, src)`.
- If no destination exists, return `-1`.
- If destination interface is down, return `-1`.
- Clone `pkt`.
- If clone fails, return `-1`.
- Create an `EVT_PACKET_RECEIVE` event with:
  - timestamp `now + link->delay_ms`
  - source pointer `src`
  - destination pointer `dst`
  - packet pointer `pkt_clone`
  - data pointer `NULL`
  - handler `rx_handler`
  - handler context `rx_ctx`
- If event creation fails, free `pkt_clone` and return `-1`.
- Schedule the event with `scheduler_schedule`.
- If scheduling fails:
  - free `pkt_clone`
  - free the event
  - return `-1`
- If scheduling succeeds, return `1`.

The current implementation does not check whether `src` itself is up. It checks
the link and the destination interface.

## Flow Charts

### Transmit Across Link

```text
link_transmit(link, pkt, src, sched, now, rx_handler, rx_ctx)
  |
  +-- reject NULL link/pkt/src/sched
  |
  +-- reject down link
  |
  +-- dst = link_get_other_interface(link, src)
  |
  +-- reject missing dst
  |
  +-- reject down dst
  |
  +-- pkt_clone = packet_clone(pkt)
  |
  +-- event_create_callback(
  |       EVT_PACKET_RECEIVE,
  |       now + link->delay_ms,
  |       src,
  |       dst,
  |       pkt_clone,
  |       NULL,
  |       rx_handler,
  |       rx_ctx)
  |
  +-- scheduler_schedule(sched, event)
  |
  +-- success: return 1
  |
  +-- failure after clone: free clone/event and return -1
```

### Scheduled Delivery

```text
scheduler reaches event timestamp
  |
  +-- scheduler_step pops event
  |
  +-- scheduler calls event->handler(event, event->handler_ctx)
  |
  +-- receive callback reads:
        event->src_device
        event->dst_device
        event->packet
```

## ACSL Contracts

The contracts belong in `link.h`. For `link_transmit`, full ownership of the
created event and clone is better checked with KLEVA tests than with a large
annotation.

### Shared Predicates

```c
/*@
    predicate link_well_formed(Link *link) =
        \valid(link) &&
        link->end_a != \null &&
        link->end_b != \null &&
        link->bandwidth_mbps > 0;
*/
```

### `link_create`

```c
/*@
    behavior null_or_bad:
        assumes end_a == \null || end_b == \null || bw == 0;
        assigns \nothing;
        ensures \result == \null;

    behavior ok:
        assumes end_a != \null && end_b != \null && bw > 0;
        allocates \result;
        ensures \result == \null || link_well_formed(\result);
        ensures \result != \null ==> \result->end_a == end_a;
        ensures \result != \null ==> \result->end_b == end_b;
        ensures \result != \null ==> \result->bandwidth_mbps == bw;
        ensures \result != \null ==> \result->delay_ms == delay;
        ensures \result != \null ==> \result->loss_rate == loss_rate;
        ensures \result != \null ==> \result->up == 1;

    complete behaviors;
    disjoint behaviors;
*/
Link *link_create(Interface *end_a,
                  Interface *end_b,
                  uint32_t bw,
                  uint32_t delay,
                  float loss_rate);
```

### `link_free`

```c
/*@
    assigns \nothing;
*/
void link_free(Link *link);
```

Implementation rule: accept `NULL`; otherwise free only the link object.

### `link_set_up`

```c
/*@
    behavior null:
        assumes link == \null;
        assigns \nothing;

    behavior valid:
        assumes \valid(link);
        assigns link->up;
        ensures link->up == up;

    complete behaviors;
    disjoint behaviors;
*/
void link_set_up(Link *link, int up);
```

### `link_is_up`

```c
/*@
    behavior null:
        assumes link == \null;
        assigns \nothing;
        ensures \result == 0;

    behavior valid:
        assumes \valid_read(link);
        assigns \nothing;
        ensures \result == 0 || \result == 1;
        ensures link->up != 0 ==> \result == 1;
        ensures link->up == 0 ==> \result == 0;

    complete behaviors;
    disjoint behaviors;
*/
int link_is_up(const Link *link);
```

### `link_get_other_interface`

```c
/*@
    behavior null:
        assumes link == \null || src == \null;
        assigns \nothing;
        ensures \result == \null;

    behavior mismatch:
        assumes \valid_read(link) && src != \null;
        assumes src != link->end_a && src != link->end_b;
        assigns \nothing;
        ensures \result == \null;

    behavior endpoint:
        assumes \valid_read(link) && src != \null;
        assumes src == link->end_a || src == link->end_b;
        assigns \nothing;
        ensures src == link->end_a ==> \result == link->end_b;
        ensures src == link->end_b ==> \result == link->end_a;

    complete behaviors;
    disjoint behaviors;
*/
Interface *link_get_other_interface(const Link *link, const Interface *src);
```

### `link_transmit`

```c
/*@
    behavior null:
        assumes link == \null || pkt == \null || src == \null || sched == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes link_well_formed(link);
        assumes packet_visible_bytes((Packet *)pkt);
        assumes \valid_read(src);
        assumes scheduler_well_formed(sched);
        assigns sched->eq->events,
                sched->eq->events[0 .. sched->eq->capacity - 1],
                sched->eq->count,
                sched->eq->capacity;
        ensures \result == 1 || \result == -1;
        ensures \result == -1 ==> sched->eq->count == \old(sched->eq->count);
        ensures \result == 1 ==> sched->eq->count == \old(sched->eq->count) + 1;
        ensures scheduler_well_formed(sched);

    complete behaviors;
    disjoint behaviors;
*/
int link_transmit(Link *link,
                  const Packet *pkt,
                  const Interface *src,
                  struct Scheduler *sched,
                  uint64_t now,
                  EventCallback rx_handler,
                  void *rx_ctx);
```

Additional required proof/test property:

- On success, the queued event has type `EVT_PACKET_RECEIVE`.
- On success, the queued event timestamp is `now + link->delay_ms`.
- On success, the queued event packet is not the same pointer as `pkt`.
- On success, the queued event stores `rx_handler` and `rx_ctx`.
- On every failure path, the caller's original packet is not freed.

## KLEVA Verification Plan

Minimum KLEVA tests:

1. `link_create(NULL, b, ...)` returns `NULL`.
2. `link_create(a, NULL, ...)` returns `NULL`.
3. `link_create(a, b, 0, ...)` returns `NULL`.
4. Successful create stores `end_a`.
5. Successful create stores `end_b`.
6. Successful create stores bandwidth, delay, and loss rate.
7. Successful create sets `up == 1`.
8. `link_free(NULL)` does not crash.
9. `link_set_up(NULL, value)` is a no-op.
10. `link_set_up` stores the exact `up` value.
11. `link_is_up(NULL)` returns `0`.
12. `link_is_up` returns `1` for nonzero `up`.
13. `link_is_up` returns `0` for `up == 0`.
14. `link_get_other_interface(NULL, src)` returns `NULL`.
15. `link_get_other_interface(link, NULL)` returns `NULL`.
16. `link_get_other_interface(link, end_a)` returns `end_b`.
17. `link_get_other_interface(link, end_b)` returns `end_a`.
18. `link_get_other_interface(link, outsider)` returns `NULL`.
19. `link_transmit` rejects NULL link, packet, source, or scheduler.
20. `link_transmit` rejects a down link.
21. `link_transmit` rejects a source that is not an endpoint.
22. `link_transmit` rejects a down destination interface.
23. Successful transmit increments scheduler queue count.
24. Successful transmit schedules `EVT_PACKET_RECEIVE`.
25. Successful transmit stores source and destination interface pointers.
26. Successful transmit stores timestamp `now + delay_ms`.
27. Successful transmit stores a cloned packet pointer, not the original.
28. Successful transmit preserves callback and callback context.
29. Schedule failure frees the clone and event and returns `-1`.

## Common Mistakes

- Do not say bandwidth affects timing until `link_transmit` implements it.
- Do not say loss rate drops packets until `link_transmit` implements it.
- Do not free the caller's original packet.
- Do not attach the link to interfaces inside `link_create`.
- Do not compare interfaces by name or MAC in `link_get_other_interface`.
- Do not assume `src` being down is currently checked by `link_transmit`.
