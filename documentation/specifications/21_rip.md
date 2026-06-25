# Module 21 - RIP

**Files:** `src/protocols/rip.c`, `src/protocols/rip.h`
**Status:** Ready for implementation; current source files are empty
**Depends on:** `router`, `route_table`, `udp`, `ip`, `packet`, `interface`,
`scheduler`, `event`, `simulator`, `byte_order`

## Concepts First

RIP means Routing Information Protocol.

RIP is a distance-vector routing protocol. Each router tells its neighbors what
networks it can reach and the metric, or cost, for each network.

RIP is intentionally simple:

- metric is hop count
- directly connected networks have low cost
- unreachable routes use metric `16`
- routers periodically advertise routes
- routers update their own routes from neighbor advertisements

RIP is not the forwarding table itself. RIP is a control-plane protocol that
feeds routes into the Router's route table.

### Distance Vector

A distance-vector protocol says:

```text
I can reach prefix P with cost M.
```

When a router receives that statement from a neighbor, it adds the cost of
reaching that neighbor.

```text
neighbor advertises: 172.16.0.0/16 metric 2
local link to neighbor costs 1
local candidate metric = 3
```

This is the Bellman-Ford idea in routing-protocol form.

RIP caps metrics at `16`. A metric of `16` means unreachable.

### RIB, FIB, And RIP Database

There are three related but different data sets:

| Data | Owner | Meaning |
| --- | --- | --- |
| RIP database | RIP | RIP-learned routes plus timers and learned interface. |
| RIB | RouteTable | Route candidates from direct, static, RIP, and later protocols. |
| FIB | RouteTable | Selected active routes used for forwarding lookup. |

RIP stores its own small database because RIP needs protocol-specific state:

- learned interface, for split horizon
- last update time, for timeout
- garbage collection state
- current advertised metric

Forwarding does not use the RIP database directly. Forwarding uses the
route-table FIB.

### Split Horizon

Split horizon prevents a common distance-vector loop.

If Router A learned a route from Router B on interface `eth1`, Router A should
not advertise that same route back out `eth1`.

```text
entry.learned_on == out_iface
  -> skip this route in the update sent on out_iface
```

This simulator should implement split horizon in the first RIP version.

Poison reverse is a related technique where the route is advertised back with
metric `16`. This spec uses simple split horizon, not poison reverse.

### Periodic And Triggered Updates

RIP sends periodic updates every 30 seconds.

RIP may also send triggered updates when a route changes. Triggered updates make
convergence faster.

This module should include the state and function boundary for triggered
updates, but the minimum implementation may first support periodic updates and
then add triggered updates through the same `rip_send_update` path.

### UDP Port 520

RIP runs over UDP port `520`.

RIPv2 normally sends updates to multicast address `224.0.0.9`.

Important current-stack constraint: the current `ip_output` path only sends to
destinations on the same subnet as the selected source interface. That means
RIPv2 multicast output needs either:

- IP output support for link-local multicast destinations, or
- a first milestone that sends directed unicast updates to configured neighbors.

The RIP spec must not pretend multicast already works if IP output still
rejects it.

### RIP Receive Context

`udp_receive` calls a bound UDP callback:

```c
void rip_receive(uint32_t src_ip,
                 uint16_t src_port,
                 Packet *payload,
                 void *ctx);
```

The `ctx` pointer must identify the RIP instance that owns:

- the simulator
- the router whose route table will be updated
- the UDP state used for binding/sending
- the enabled interfaces
- the RIP database

UDP should not know about route tables. RIP should update routes by calling
Router or RouteTable APIs.

## Purpose

The RIP module implements RIPv2-style route exchange for routers.

It provides:

- RIP wire header and entry layout
- per-router RIP state
- enabled-interface tracking
- UDP port 520 receive callback
- periodic update scheduling
- RIP update encoding
- RIP update parsing
- metric increment and capping at 16
- split horizon
- route install/delete through Router/RouteTable APIs
- route timeout and garbage-collection handlers

It does not:

- forward data packets
- own Router interfaces
- own the global simulator
- replace the route table
- implement OSPF/BGP/EIGRP behavior
- guarantee multicast delivery unless IP output supports it

## Architecture Boundary

| Responsibility | Owner |
| --- | --- |
| Store RIP learned routes and timers | RIP |
| Store route candidates | RouteTable RIB |
| Select active forwarding route | RouteTable FIB rebuild |
| Forward packets | Router |
| Send/receive UDP datagrams | UDP/IP |
| Schedule periodic/timeout events | Scheduler |
| Decide split horizon omission | RIP |
| Resolve next-hop MAC | Router forwarding ARP path |

RIP should call `router_add_route` or `route_table_add` for learned routes. It
must not write directly into route-table arrays.

RIP should bind UDP port `520` through `udp_bind`. It must not inspect UDP
socket slots directly.

## Data Model

### Constants

```c
#define RIP_PORT               520
#define RIP_VERSION            2
#define RIP_CMD_REQUEST        1
#define RIP_CMD_RESPONSE       2
#define RIP_AFI_IPV4           2
#define RIP_MULTICAST          0xE0000009u
#define RIP_INFINITY           16
#define RIP_UPDATE_INTERVAL_US 30000000ULL
#define RIP_TIMEOUT_US         180000000ULL
#define RIP_GC_US              120000000ULL
#define RIP_MAX_IFACES         16
#define RIP_DB_SIZE            128
#define RIP_MAX_ROUTES         25
#define RIP_HDR_LEN            4
#define RIP_ENTRY_LEN          20
```

Use microseconds for timers because `Scheduler.now` and `Event.timestamp` use
microseconds.

### `RipHeader`

```c
typedef struct __attribute__((packed)) RipHeader {
    uint8_t  command;
    uint8_t  version;
    uint16_t zero;
} RipHeader;
```

Wire layout:

```text
offset  size  field
0       1     command: 1 request, 2 response
1       1     version: 2
2       2     zero: must be zero
```

### `RipEntry`

```c
typedef struct __attribute__((packed)) RipEntry {
    uint16_t afi;
    uint16_t route_tag;
    uint32_t ip_addr;
    uint32_t subnet_mask;
    uint32_t next_hop;
    uint32_t metric;
} RipEntry;
```

Wire layout:

```text
offset  size  field
0       2     AFI, 2 for IPv4
2       2     route tag, zero in this simulator
4       4     prefix/network address
8       4     subnet mask
12      4     next hop, zero means use sender
16      4     metric, 1..16
```

All multi-byte RIP fields are network byte order on the wire.

### `RipRouteInfo`

```c
typedef struct RipRouteInfo {
    uint32_t   prefix;
    uint8_t    prefix_len;
    uint8_t    state;
    uint8_t    valid;
    uint8_t    _pad;
    uint32_t   metric;
    uint32_t   next_hop;
    Interface *learned_on;
    uint64_t   last_update;
} RipRouteInfo;
```

Suggested states:

```c
#define RIP_ROUTE_ACTIVE  1
#define RIP_ROUTE_GC      2
```

`prefix` and `next_hop` are host-order IPv4 addresses.

`learned_on` is borrowed. RIP does not free interfaces.

### `RipState`

```c
typedef struct RipState {
    RipRouteInfo db[RIP_DB_SIZE];
    int          db_count;

    Interface   *ifaces[RIP_MAX_IFACES];
    int          iface_count;

    Simulator   *sim;
    Router      *router;
    UdpState    *udp_state;
} RipState;
```

`RipState` is per router.

The owner allocates `RipState`; RIP initializes it. The owner must also provide
the Router and UDP state that RIP uses for route installation and UDP binding.

## Ownership And Lifetime

RIP owns no interfaces, routers, simulators, UDP states, or packets before UDP
delivery.

`rip_init` initializes caller-owned `RipState`.

`rip_enable_iface` borrows the interface pointer.

`rip_receive` receives ownership of the UDP payload packet from UDP. It must
free that packet after parsing.

`rip_send_update` creates a UDP payload through `udp_send`; ownership follows
the UDP send contract.

Scheduled RIP events borrow `RipState *` as context. The owner must keep
`RipState` alive while scheduled RIP events can fire.

## Public API

```c
void rip_init(RipState *state,
              Simulator *sim,
              Router *router,
              UdpState *udp_state);

int rip_enable_iface(RipState *state, Interface *iface);

void rip_receive(uint32_t src_ip,
                 uint16_t src_port,
                 Packet *payload,
                 void *ctx);

int rip_send_update(RipState *state, Interface *out_iface);

void rip_update_handler(const Event *e, void *ctx);

void rip_timeout_handler(const Event *e, void *ctx);

void rip_gc_handler(const Event *e, void *ctx);
```

`rip_init` should bind UDP port `520`:

```c
udp_bind(udp_state, RIP_PORT, rip_receive, state);
```

If the owner uses a different control-plane UDP state arrangement, the binding
must still produce the same result: UDP port 520 delivers payloads to
`rip_receive` with this `RipState *` as context.

## Function Behavior

### `rip_init`

Required behavior:

- If `state == NULL`, return immediately.
- Zero all RIP state.
- Store `sim`, `router`, and `udp_state`.
- If `udp_state != NULL`, bind UDP port `520` to `rip_receive` with `state` as
  context.
- If `sim != NULL && sim->sched != NULL`, schedule first RIP update at
  `scheduler_now(sim->sched) + RIP_UPDATE_INTERVAL_US`.

If UDP bind fails, state remains initialized but RIP receive will not work. The
implementation may record that as a status field later; this initial API returns
`void`, so KLEVA should verify observable state rather than an error code.

### `rip_enable_iface`

Required behavior:

- If `state == NULL || iface == NULL`, return `-1`.
- If the interface is already enabled, return `0`.
- If `iface_count >= RIP_MAX_IFACES`, return `-1`.
- Store the interface in the first free enabled-interface slot.
- Increment `iface_count`.
- Return `0`.

RIP does not take ownership of the interface.

### `rip_receive`

Required behavior:

- If `payload == NULL`, return.
- If `ctx == NULL`, free payload and return.
- Cast `ctx` to `RipState *`.
- If `state->router == NULL`, free payload and return.
- If `payload->len < RIP_HDR_LEN`, free payload and return.
- If `(payload->len - RIP_HDR_LEN) % RIP_ENTRY_LEN != 0`, free payload and
  return.
- Reject messages with more than `RIP_MAX_ROUTES` entries.
- Parse header.
- Accept only version `2`.
- Accept response/update messages in the first implementation.
- For each entry:
  - require AFI IPv4
  - convert prefix, subnet mask, next hop, and metric to host order
  - derive prefix length from the subnet mask
  - reject non-contiguous masks
  - cap metric at `RIP_INFINITY`
  - received candidate metric is `min(entry_metric + 1, RIP_INFINITY)`
  - if entry next hop is zero, use `src_ip` as route next hop
  - choose the incoming interface that should be recorded as `learned_on`
  - if metric is less than `RIP_INFINITY`, update RIP DB and install route as
    `ROUTE_PROTO_RIP`
  - if metric is `RIP_INFINITY`, mark DB route for garbage collection and
    remove or poison the route-table entry
- Free payload before returning.

Incoming interface discovery is a design point. Because UDP callbacks currently
receive source IP and source port, but not the ingress `Interface *`, the first
implementation needs one of these choices:

- extend UDP callback context/path to include ingress interface, or
- infer the enabled interface from source IP/subnet, or
- include ingress interface in a RIP-specific receive wrapper.

The spec requires `learned_on` for split horizon, so this must be solved before
RIP can be fully correct.

### `rip_send_update`

Required behavior:

- If `state == NULL || out_iface == NULL`, return `-1`.
- If `state->sim == NULL`, return `-1`.
- Build one or more RIP response payloads.
- Each payload starts with `RipHeader`:
  - command `RIP_CMD_RESPONSE`
  - version `RIP_VERSION`
  - zero field `0`
- Add at most `RIP_MAX_ROUTES` entries per payload.
- For each valid RIP DB entry:
  - if `entry.learned_on == out_iface`, skip it for split horizon
  - otherwise encode prefix, mask, next hop, and metric
- Send each payload with UDP source port and destination port `520`.
- Source IP is `ns_ntohl(out_iface->ip_addr)`.
- Destination IP is `RIP_MULTICAST` when multicast output is supported.
- Return `0` if all needed update packets are sent or no routes need sending.
- Return `-1` if payload construction or UDP send fails.

Current-stack warning: `udp_send` uses `ip_output`, and current `ip_output`
does not support `224.0.0.9` multicast. If multicast support is not added
before RIP, use configured unicast neighbors for the first milestone and state
that choice in the implementation notes.

### `rip_update_handler`

Required behavior:

- If event/context is missing, return.
- Context is `RipState *`.
- For each enabled interface, call `rip_send_update`.
- Schedule the next update at current scheduler time plus
  `RIP_UPDATE_INTERVAL_US`.

### `rip_timeout_handler`

Required behavior:

- If event/context is missing, return.
- Find the RIP DB entry identified by event data or route key.
- If the route has not been refreshed before timeout:
  - set metric to `RIP_INFINITY`
  - move state to garbage collection
  - delete or poison the `ROUTE_PROTO_RIP` route in Router/RouteTable
  - schedule garbage collection after `RIP_GC_US`
  - send triggered update when triggered updates are implemented

### `rip_gc_handler`

Required behavior:

- If event/context is missing, return.
- Find the RIP DB entry identified by event data or route key.
- If it is still in garbage-collection state:
  - invalidate the DB entry
  - clear borrowed interface pointer
  - decrement `db_count`

## Flow Charts

### Initialization

```text
rip_init(state, sim, router, udp_state)
  |
  +-- null state: return
  +-- zero state
  +-- store sim/router/udp_state
  +-- udp_bind(udp_state, 520, rip_receive, state)
  +-- schedule first periodic update if scheduler exists
```

### Receive Update

```text
rip_receive(src_ip, src_port, payload, state)
  |
  +-- validate payload/header/entry count
  +-- for each entry:
        |
        +-- metric = min(received_metric + 1, 16)
        +-- prefix_len = mask_to_prefix_len(subnet_mask)
        |
        +-- metric < 16:
        |     update RIP DB
        |     router_add_route(... ROUTE_PROTO_RIP)
        |     schedule timeout
        |
        +-- metric == 16:
              mark RIP DB route unreachable
              router_del_route(... ROUTE_PROTO_RIP)
              schedule garbage collection
```

### Send Update

```text
rip_send_update(state, out_iface)
  |
  +-- build response header
  +-- scan RIP DB
        |
        +-- learned_on == out_iface: skip
        +-- otherwise append route entry
  |
  +-- send UDP src=520 dst=520
```

## ACSL Contracts

The contracts belong in `rip.h`. Use literal bounds:

- enabled interfaces: `16`
- RIP database entries: `128`
- RIP entries per packet: `25`
- RIP header bytes: `4`
- RIP entry bytes: `20`

### Shared Predicates

```c
/*@
    predicate rip_db_count_valid(RipState *state) =
        0 <= state->db_count && state->db_count <= 128;

    predicate rip_iface_count_valid(RipState *state) =
        0 <= state->iface_count && state->iface_count <= 16;

    predicate rip_route_slot_valid(RipState *state, integer i) =
        0 <= i && i < 128 ==>
            (state->db[i].valid == 0 ||
             (state->db[i].valid == 1 &&
              state->db[i].prefix_len <= 32 &&
              1 <= state->db[i].metric &&
              state->db[i].metric <= 16));

    predicate rip_state_well_formed(RipState *state) =
        \valid(state) &&
        rip_db_count_valid(state) &&
        rip_iface_count_valid(state) &&
        \forall integer i; 0 <= i && i < 128 ==>
            rip_route_slot_valid(state, i);
*/
```

### `rip_init`

```c
/*@
    behavior null:
        assumes state == \null;
        assigns \nothing;

    behavior valid:
        assumes \valid(state);
        assigns state->db[0 .. 127],
                state->db_count,
                state->ifaces[0 .. 15],
                state->iface_count,
                state->sim,
                state->router,
                state->udp_state;
        ensures state->db_count == 0;
        ensures state->iface_count == 0;
        ensures state->sim == sim;
        ensures state->router == router;
        ensures state->udp_state == udp_state;
        ensures \forall integer i; 0 <= i && i < 128 ==>
                state->db[i].valid == 0;

    complete behaviors;
    disjoint behaviors;
*/
void rip_init(RipState *state,
              Simulator *sim,
              Router *router,
              UdpState *udp_state);
```

### `rip_enable_iface`

```c
/*@
    behavior bad_input:
        assumes state == \null || iface == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes rip_state_well_formed(state);
        assumes \valid(iface);
        assigns state->ifaces[0 .. 15],
                state->iface_count;
        ensures \result == 0 || \result == -1;
        ensures \result == 0 ==>
                state->iface_count == \old(state->iface_count) ||
                state->iface_count == \old(state->iface_count) + 1;

    complete behaviors;
    disjoint behaviors;
*/
int rip_enable_iface(RipState *state, Interface *iface);
```

### `rip_receive`

```c
/*@
    behavior null_payload:
        assumes payload == \null;
        assigns \nothing;

    behavior bad_ctx:
        assumes payload != \null && ctx == \null;
        assigns \nothing;

    behavior valid:
        assumes payload != \null;
        assumes ctx != \null;
        assumes rip_state_well_formed((RipState *)ctx);
        assigns ((RipState *)ctx)->db[0 .. 127],
                ((RipState *)ctx)->db_count;
*/
void rip_receive(uint32_t src_ip,
                 uint16_t src_port,
                 Packet *payload,
                 void *ctx);
```

Additional required proof/test property:

- `rip_receive` frees non-NULL payload on every path.
- Valid metric is incremented by one and capped at `16`.
- Metric `16` does not install a forwarding route.

### `rip_send_update`

```c
/*@
    behavior bad_input:
        assumes state == \null || out_iface == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes rip_state_well_formed(state);
        assumes \valid(out_iface);
        assigns \nothing;
        ensures \result == 0 || \result == -1;

    complete behaviors;
    disjoint behaviors;
*/
int rip_send_update(RipState *state, Interface *out_iface);
```

### Event Handlers

```c
/*@
    behavior null_input:
        assumes e == \null || ctx == \null;
        assigns \nothing;

    behavior valid:
        assumes e != \null && ctx != \null;
        assumes rip_state_well_formed((RipState *)ctx);
        assigns ((RipState *)ctx)->db[0 .. 127],
                ((RipState *)ctx)->db_count;
*/
void rip_update_handler(const Event *e, void *ctx);
void rip_timeout_handler(const Event *e, void *ctx);
void rip_gc_handler(const Event *e, void *ctx);
```

## KLEVA Verification Plan

Minimum KLEVA tests:

1. `rip_init(NULL, ...)` does not crash.
2. Valid init clears DB count and interface count.
3. Valid init stores simulator, router, and UDP state pointers.
4. Valid init clears every DB valid bit.
5. Valid init binds UDP port 520 when UDP state exists.
6. Valid init schedules first update when scheduler exists.
7. `rip_enable_iface` rejects NULL state and NULL interface.
8. `rip_enable_iface` accepts first interface.
9. `rip_enable_iface` does not duplicate an already enabled interface.
10. `rip_enable_iface` rejects a full interface list.
11. `rip_receive(NULL payload)` does not crash.
12. `rip_receive` with NULL context frees payload.
13. `rip_receive` rejects too-short payload.
14. `rip_receive` rejects invalid entry alignment.
15. `rip_receive` rejects more than 25 entries.
16. `rip_receive` rejects wrong RIP version.
17. `rip_receive` rejects non-IPv4 AFI entries.
18. `rip_receive` rejects non-contiguous subnet mask.
19. Received metric `1` installs route with metric `2`.
20. Received metric `15` installs route with metric `16` only as unreachable.
21. Received metric `16` removes or poisons route.
22. Entry next hop `0` uses sender IP as next hop.
23. Nonzero entry next hop is preserved.
24. Split horizon omits routes learned on outgoing interface.
25. Update packet contains at most 25 entries.
26. Periodic update sends one update per enabled interface.
27. Timeout marks stale route unreachable.
28. Garbage collection invalidates stale unreachable route.
29. Valid receive frees payload after parsing.
30. Multicast output limitation is covered by either IP multicast support test
    or directed-neighbor milestone test.

## Common Mistakes

- Do not make RIP forwarding data packets; Router forwards packets.
- Do not write directly into `RouteTable` arrays.
- Do not let forwarding read the RIP database.
- Do not forget that RIP metric `16` means unreachable.
- Do not advertise a route back out the interface it was learned on.
- Do not use milliseconds for scheduler timestamps; scheduler uses
  microseconds.
- Do not assume `224.0.0.9` works until IP output supports multicast.
- Do not ignore the ingress-interface problem; split horizon needs it.
- Do not let UDP inspect RIP routes.
- Do not leak the UDP payload passed to `rip_receive`.
