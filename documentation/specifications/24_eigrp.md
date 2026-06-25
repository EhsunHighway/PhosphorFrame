# Module 24 - EIGRP

**Files:** `src/protocols/eigrp.c`, `src/protocols/eigrp.h`
**Status:** Ready for implementation; source files do not exist yet
**Depends on:** `router`, `route_table`, `ip`, `packet`, `interface`,
`scheduler`, `event`, `simulator`, `byte_order`

## Concepts First

EIGRP means Enhanced Interior Gateway Routing Protocol.

EIGRP is a Cisco-origin hybrid routing protocol. It behaves like a
distance-vector protocol in that routers exchange reachability with neighbors,
but it uses DUAL to keep route changes loop-free.

This simulator implements a simplified EIGRP:

- IPv4 protocol number `88`
- multicast `224.0.0.10`
- Hello neighbor discovery
- UPDATE, QUERY, REPLY packet types
- simplified reliable behavior
- composite metric using bandwidth and delay
- DUAL feasibility condition
- route installation into `RouteTable` as `ROUTE_PROTO_EIGRP`

### Distance Vector With Topology Memory

RIP stores one best route per prefix. EIGRP stores more topology information.

For each prefix, EIGRP tracks:

- successor: current best next hop
- feasible distance: best total local metric
- reported distance: neighbor's advertised distance
- feasible successors: backup paths that satisfy the feasibility condition

This gives EIGRP faster and safer convergence than plain RIP.

### DUAL

DUAL means Diffusing Update Algorithm.

The key loop-free rule is the feasibility condition:

```text
neighbor_reported_distance < current_feasible_distance
```

If a neighbor reports a distance smaller than our current feasible distance,
that neighbor cannot be routing through us for that destination. The route is
safe as a feasible successor.

If no feasible successor exists when a route fails, EIGRP sends QUERY messages
to neighbors and waits for REPLY messages before choosing a new path.

The first implementation may keep QUERY/REPLY simplified, but it must preserve
the feasibility-condition decision point.

### Composite Metric

EIGRP's real metric can combine bandwidth, delay, load, reliability, and MTU
with K-values.

This simulator uses:

```text
metric = (10000000 / bandwidth_kbps) + delay_us
```

with `K1 = 1`, `K3 = 1`, and the other K-values disabled.

Bandwidth must be nonzero. If bandwidth is zero, metric computation returns
`EIGRP_INFINITY`.

### EIGRP Transport

EIGRP does not use UDP or TCP.

It runs directly over IPv4 protocol number `88`.

It has its own reliability concept, RTP, for selected packet types. This
simulator does not need full RTP in the first milestone. It should still model:

- sequence number field
- acknowledgment field
- ACK packets for reliable UPDATE/QUERY/REPLY when needed
- per-neighbor last-seen sequence enough for KLEVA tests

### Router Control-Plane Dispatch

EIGRP protocol `88` packets are router control-plane packets.

Current Router design is forwarding-first. EIGRP requires the same integration
decision as OSPF:

- Router owns a local control-plane IP dispatch stack, or
- Router receive intercepts protocol `88` packets addressed to the router or
  EIGRP multicast, or
- a shared router control-plane dispatch mechanism is added.

Do not treat local EIGRP packets as ordinary transit packets.

### Multicast

EIGRP uses multicast `224.0.0.10`.

Current `ip_output` does not guarantee multicast output. The implementation
must either add multicast support to IP/link output or use an EIGRP-specific
link-local send helper that still follows packet ownership rules.

## Purpose

The EIGRP module implements simplified EIGRP route exchange for Router objects.

It provides:

- EIGRP header and route wire structures
- per-router EIGRP state
- enabled-interface tracking with bandwidth and delay
- neighbor table
- topology table
- metric computation
- Hello send/receive
- UPDATE/QUERY/REPLY handling
- DUAL feasibility decision
- route installation/removal through Router/RouteTable APIs
- Hello and hold timers

It does not:

- forward data packets
- own Router interfaces
- replace the route table
- implement full Cisco RTP in the first milestone
- implement all K-value combinations
- implement authentication
- guarantee multicast until IP/link output supports it

## Architecture Boundary

| Responsibility | Owner |
| --- | --- |
| Neighbor table | EIGRP |
| Topology table | EIGRP |
| DUAL decision | EIGRP |
| Composite metric | EIGRP |
| Route candidates | RouteTable RIB |
| Active forwarding route | RouteTable FIB |
| Data packet forwarding | Router |
| EIGRP protocol 88 send/receive | IP/control-plane path |
| Timers | Scheduler |

EIGRP should call Router/RouteTable public APIs. It must not write directly
into route-table arrays.

Router forwarding must not read EIGRP topology entries.

## Data Model

### Constants

```c
#define EIGRP_VERSION          2
#define EIGRP_PROTO_NUM        88
#define EIGRP_MULTICAST        0xE000000Au
#define EIGRP_HELLO_US         5000000ULL
#define EIGRP_HOLD_US          15000000ULL
#define EIGRP_MAX_NEIGHBORS    32
#define EIGRP_MAX_ROUTES       256
#define EIGRP_MAX_IFACES       16
#define EIGRP_INFINITY         0xFFFFFFFFu
#define EIGRP_K1               1
#define EIGRP_K3               1
```

Use microseconds for timers because `Scheduler.now` and `Event.timestamp` use
microseconds.

### Packet Types

```c
#define EIGRP_OPC_UPDATE 1
#define EIGRP_OPC_QUERY  3
#define EIGRP_OPC_REPLY  4
#define EIGRP_OPC_HELLO  5
```

### `EigrpHeader`

```c
typedef struct __attribute__((packed)) EigrpHeader {
    uint8_t  version;
    uint8_t  opcode;
    uint16_t checksum;
    uint32_t flags;
    uint32_t seq;
    uint32_t ack;
    uint32_t as_number;
} EigrpHeader;
```

Header length is `20` bytes.

Multi-byte fields are network byte order on the wire.

### `EigrpRoute`

```c
typedef struct __attribute__((packed)) EigrpRoute {
    uint8_t  prefix_len;
    uint8_t  _pad[3];
    uint32_t prefix;
    uint32_t bandwidth;
    uint32_t delay;
    uint32_t metric;
    uint32_t next_hop;
} EigrpRoute;
```

Route entries are used in UPDATE, QUERY, and REPLY messages.

### `EigrpIface`

```c
typedef struct EigrpIface {
    Interface *iface;
    uint32_t   bandwidth_kbps;
    uint32_t   delay_us;
    int        valid;
} EigrpIface;
```

EIGRP needs bandwidth and delay per enabled interface.

### `EigrpNeighbor`

```c
typedef struct EigrpNeighbor {
    uint32_t   ip_addr;
    uint32_t   router_id;
    uint64_t   hold_deadline;
    Interface *iface;
    uint32_t   last_seq;
    int        valid;
} EigrpNeighbor;
```

IP addresses are host order.

`iface` is borrowed.

### `EigrpTopoEntry`

```c
typedef struct EigrpTopoEntry {
    uint32_t   prefix;
    uint8_t    prefix_len;
    uint8_t    valid;
    uint8_t    active;
    uint8_t    _pad;
    uint32_t   fd;
    uint32_t   rd;
    uint32_t   successor_metric;
    uint32_t   next_hop;
    Interface *iface;
} EigrpTopoEntry;
```

`active == 1` means the route is in active/query state.

### `EigrpState`

```c
typedef struct EigrpState {
    EigrpNeighbor  neighbors[EIGRP_MAX_NEIGHBORS];
    int            neighbor_count;

    EigrpTopoEntry topo[EIGRP_MAX_ROUTES];
    int            topo_count;

    EigrpIface     ifaces[EIGRP_MAX_IFACES];
    int            iface_count;

    uint32_t       as_number;
    uint32_t       router_id;
    uint32_t       next_seq;

    Simulator     *sim;
    Router        *router;
} EigrpState;
```

`EigrpState` is per router.

## Ownership And Lifetime

The owner allocates `EigrpState`; EIGRP initializes it.

EIGRP borrows `Simulator *`, `Router *`, and `Interface *` pointers.

EIGRP does not free Router, Simulator, or interfaces.

`eigrp_receive` receives ownership of a packet from the protocol-88 receive
path. It must free the packet after parsing or on errors unless it transfers
ownership to another documented helper.

Scheduled EIGRP events borrow `EigrpState *` as context. The owner must keep
state alive while events can fire.

## Public API

```c
void eigrp_init(EigrpState *state,
                Simulator *sim,
                Router *router,
                uint32_t as_number,
                uint32_t router_id);

int eigrp_enable_iface(EigrpState *state,
                       Interface *iface,
                       uint32_t bandwidth_kbps,
                       uint32_t delay_us);

int eigrp_receive(Interface *iface,
                  Packet *pkt,
                  void *ctx);

int eigrp_send_hello(EigrpState *state, Interface *iface);

int eigrp_send_update(EigrpState *state,
                      Interface *iface,
                      const EigrpTopoEntry *routes,
                      size_t route_count);

int eigrp_dual_process(EigrpState *state,
                       uint32_t prefix,
                       uint8_t prefix_len,
                       uint32_t reported_metric,
                       EigrpNeighbor *nbr);

uint32_t eigrp_compute_metric(uint32_t bandwidth_kbps, uint32_t delay_us);

void eigrp_hello_timer(const Event *e, void *ctx);

void eigrp_hold_timer(const Event *e, void *ctx);
```

`eigrp_receive` has the IP protocol handler shape:

```c
int handler(Interface *iface, Packet *pkt, void *ctx);
```

The intended registration is protocol `88` with context `EigrpState *`, through
the router control-plane dispatch mechanism.

## Function Behavior

### `eigrp_init`

Required behavior:

- If `state == NULL`, return immediately.
- Zero all EIGRP state.
- Store simulator, router, AS number, and router ID.
- Set `next_seq = 1`.
- Initialize counts to zero.
- If scheduler exists, schedule first Hello event at
  `scheduler_now(sim->sched) + EIGRP_HELLO_US`.

Protocol `88` registration may be done by the Router/control-plane owner rather
than by `eigrp_init`, because `eigrp_init` may not own the IP stack.

### `eigrp_enable_iface`

Required behavior:

- If `state == NULL || iface == NULL`, return `-1`.
- If `bandwidth_kbps == 0`, return `-1`.
- If interface is already enabled, update bandwidth and delay and return `0`.
- If interface table is full, return `-1`.
- Store interface, bandwidth, and delay.
- Increment interface count.
- Return `0`.

EIGRP does not take ownership of the interface.

### `eigrp_compute_metric`

Required behavior:

- If `bandwidth_kbps == 0`, return `EIGRP_INFINITY`.
- Compute:

```text
(10000000 / bandwidth_kbps) + delay_us
```

- If arithmetic would overflow `uint32_t`, return `EIGRP_INFINITY`.
- Otherwise return the metric.

### `eigrp_receive`

Required behavior:

- If `iface == NULL`, return `-1`.
- If `pkt == NULL`, increment `iface->rx_errors` and return `-1`.
- If `ctx == NULL`, free packet, increment `rx_errors`, return `-1`.
- Cast context to `EigrpState *`.
- If packet is shorter than `EigrpHeader`, free packet, increment `rx_errors`,
  return `-1`.
- Validate version.
- Validate AS number matches state AS number.
- Validate checksum according to the chosen checksum rule, or document checksum
  as disabled in the first milestone and require zero checksum.
- Find or create neighbor for source IP and ingress interface when opcode is
  HELLO or a valid neighbor packet.
- Dispatch by opcode:
  - HELLO: refresh neighbor hold deadline
  - UPDATE: ACK if reliable, process route entries through DUAL
  - QUERY: mark route active and reply when possible
  - REPLY: clear pending query state when all replies are received
- Free packet before returning.

### `eigrp_send_hello`

Required behavior:

- If `state == NULL || iface == NULL`, return `-1`.
- If `state->sim == NULL`, return `-1`.
- Build EIGRP Hello packet.
- Send to `EIGRP_MULTICAST`.
- Return `0` on success, `-1` on failure.

Current-stack warning: this requires protocol-88 multicast output support or an
EIGRP-specific link-local send helper.

### `eigrp_send_update`

Required behavior:

- If `state == NULL || iface == NULL`, return `-1`.
- If `routes == NULL && route_count > 0`, return `-1`.
- Build UPDATE packet with one or more route entries.
- Assign a nonzero sequence number from `state->next_seq`.
- Increment `state->next_seq`.
- Send to multicast or a specific neighbor depending on the update type.
- Return `0` on success, `-1` on failure.

### `eigrp_dual_process`

Required behavior:

- If `state == NULL || nbr == NULL`, return `-1`.
- If `prefix_len > 32`, return `-1`.
- Compute link cost to neighbor from the enabled interface record.
- Candidate feasible distance is `reported_metric + link_cost`, capped at
  `EIGRP_INFINITY`.
- Find or create topology entry for prefix.
- If no route exists yet:
  - install neighbor as successor when metric is reachable
  - update topology entry
  - install route as `ROUTE_PROTO_EIGRP`
- If route exists and feasibility condition holds:
  - if candidate is better than current FD, replace successor
  - install route as `ROUTE_PROTO_EIGRP`
- If feasibility condition fails and current successor is lost:
  - mark route active
  - send QUERY to neighbors
  - do not install a looping candidate
- Return `0` on successful processing.

### Timer Handlers

`eigrp_hello_timer`:

- If context is missing, return.
- Send Hello on each enabled interface.
- Schedule next Hello.

`eigrp_hold_timer`:

- If context is missing, return.
- For each neighbor whose hold deadline has expired:
  - invalidate neighbor
  - remove or mark routes using that neighbor unreachable
  - run DUAL for affected prefixes

## Flow Charts

### Initialization

```text
eigrp_init(state, sim, router, as_number, router_id)
  |
  +-- null state: return
  +-- zero state
  +-- store sim/router/as/router_id
  +-- next_seq = 1
  +-- schedule Hello if scheduler exists
```

### Receive Update

```text
eigrp_receive(iface, pkt, state)
  |
  +-- validate header/version/AS
  +-- opcode UPDATE
  +-- for each route:
        eigrp_dual_process(prefix, reported_metric, neighbor)
```

### DUAL Decision

```text
reported distance = neighbor metric
candidate FD = reported distance + link cost
  |
  +-- no current route:
  |     install as successor if reachable
  |
  +-- reported distance < current FD:
  |     feasible, loop-free candidate
  |     install if better
  |
  +-- not feasible and current successor failed:
        mark active
        send QUERY
        wait for REPLY
```

## ACSL Contracts

The contracts belong in `eigrp.h`. Use literal bounds:

- neighbors: `32`
- topology entries: `256`
- enabled interfaces: `16`
- EIGRP header bytes: `20`

### Shared Predicates

```c
/*@
    predicate eigrp_neighbor_count_valid(EigrpState *state) =
        0 <= state->neighbor_count && state->neighbor_count <= 32;

    predicate eigrp_topo_count_valid(EigrpState *state) =
        0 <= state->topo_count && state->topo_count <= 256;

    predicate eigrp_iface_count_valid(EigrpState *state) =
        0 <= state->iface_count && state->iface_count <= 16;

    predicate eigrp_topo_slot_valid(EigrpState *state, integer i) =
        0 <= i && i < 256 ==>
            (state->topo[i].valid == 0 ||
             (state->topo[i].valid == 1 &&
              state->topo[i].prefix_len <= 32));

    predicate eigrp_state_well_formed(EigrpState *state) =
        \valid(state) &&
        eigrp_neighbor_count_valid(state) &&
        eigrp_topo_count_valid(state) &&
        eigrp_iface_count_valid(state) &&
        \forall integer i; 0 <= i && i < 256 ==>
            eigrp_topo_slot_valid(state, i);
*/
```

### `eigrp_init`

```c
/*@
    behavior null:
        assumes state == \null;
        assigns \nothing;

    behavior valid:
        assumes \valid(state);
        assigns state->neighbors[0 .. 31],
                state->neighbor_count,
                state->topo[0 .. 255],
                state->topo_count,
                state->ifaces[0 .. 15],
                state->iface_count,
                state->as_number,
                state->router_id,
                state->next_seq,
                state->sim,
                state->router;
        ensures state->neighbor_count == 0;
        ensures state->topo_count == 0;
        ensures state->iface_count == 0;
        ensures state->as_number == as_number;
        ensures state->router_id == router_id;
        ensures state->next_seq == 1;
        ensures state->sim == sim;
        ensures state->router == router;

    complete behaviors;
    disjoint behaviors;
*/
void eigrp_init(EigrpState *state,
                Simulator *sim,
                Router *router,
                uint32_t as_number,
                uint32_t router_id);
```

### `eigrp_compute_metric`

```c
/*@
    behavior zero_bw:
        assumes bandwidth_kbps == 0;
        assigns \nothing;
        ensures \result == 0xFFFFFFFFu;

    behavior valid:
        assumes bandwidth_kbps != 0;
        assigns \nothing;
        ensures \result >= delay_us || \result == 0xFFFFFFFFu;

    complete behaviors;
    disjoint behaviors;
*/
uint32_t eigrp_compute_metric(uint32_t bandwidth_kbps, uint32_t delay_us);
```

### `eigrp_enable_iface`

```c
/*@
    behavior bad_input:
        assumes state == \null || iface == \null || bandwidth_kbps == 0;
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes eigrp_state_well_formed(state);
        assumes \valid(iface);
        assumes bandwidth_kbps != 0;
        assigns state->ifaces[0 .. 15],
                state->iface_count;
        ensures \result == 0 || \result == -1;

    complete behaviors;
    disjoint behaviors;
*/
int eigrp_enable_iface(EigrpState *state,
                       Interface *iface,
                       uint32_t bandwidth_kbps,
                       uint32_t delay_us);
```

### `eigrp_receive`

```c
/*@
    behavior null_iface:
        assumes iface == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior null_pkt:
        assumes iface != \null && pkt == \null;
        assigns iface->rx_errors;
        ensures \result == -1;

    behavior bad_ctx:
        assumes iface != \null && pkt != \null && ctx == \null;
        assigns iface->rx_errors;
        ensures \result == -1;

    behavior valid:
        assumes \valid(iface);
        assumes pkt != \null;
        assumes ctx != \null;
        assumes eigrp_state_well_formed((EigrpState *)ctx);
        assigns iface->rx_errors,
                ((EigrpState *)ctx)->neighbors[0 .. 31],
                ((EigrpState *)ctx)->neighbor_count,
                ((EigrpState *)ctx)->topo[0 .. 255],
                ((EigrpState *)ctx)->topo_count;
        ensures \result == 0 || \result == -1;

    complete behaviors;
    disjoint behaviors;
*/
int eigrp_receive(Interface *iface, Packet *pkt, void *ctx);
```

### `eigrp_dual_process`

```c
/*@
    behavior bad_input:
        assumes state == \null || nbr == \null || prefix_len > 32;
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes eigrp_state_well_formed(state);
        assumes \valid(nbr);
        assumes prefix_len <= 32;
        assigns state->topo[0 .. 255],
                state->topo_count,
                state->router->route_tbl.rib[0 .. 255],
                state->router->route_tbl.rib_count,
                state->router->route_tbl.fib[0 .. 255],
                state->router->route_tbl.fib_count;
        ensures \result == 0 || \result == -1;

    complete behaviors;
    disjoint behaviors;
*/
int eigrp_dual_process(EigrpState *state,
                       uint32_t prefix,
                       uint8_t prefix_len,
                       uint32_t reported_metric,
                       EigrpNeighbor *nbr);
```

## KLEVA Verification Plan

Minimum KLEVA tests:

1. `eigrp_init(NULL, ...)` does not crash.
2. Valid init clears neighbor, topology, and interface counts.
3. Valid init stores AS number, router ID, simulator, and router.
4. Valid init sets `next_seq == 1`.
5. `eigrp_compute_metric(0, delay)` returns infinity.
6. Metric formula matches `(10000000 / bandwidth) + delay`.
7. `eigrp_enable_iface` rejects NULL state, NULL interface, and zero bandwidth.
8. `eigrp_enable_iface` adds first interface.
9. `eigrp_enable_iface` updates duplicate interface metrics.
10. `eigrp_enable_iface` rejects full interface table.
11. `eigrp_receive` rejects NULL interface.
12. `eigrp_receive` with NULL packet increments `rx_errors`.
13. `eigrp_receive` with NULL context frees packet and increments `rx_errors`.
14. Too-short packet is rejected.
15. Wrong version is rejected.
16. Wrong AS number is rejected.
17. HELLO creates or refreshes neighbor.
18. UPDATE calls DUAL for each route.
19. Feasible route installs successor.
20. Infeasible route does not replace current successor.
21. Lost successor with no feasible successor marks route active.
22. Active route sends QUERY.
23. REPLY can clear active state when all replies are received.
24. Hold timer invalidates stale neighbor.
25. Neighbor loss reprocesses affected routes.
26. EIGRP protocol 88 receive path is covered by Router/IP integration test.
27. EIGRP multicast send path is covered by IP multicast or send-helper test.

## Common Mistakes

- Do not implement EIGRP over UDP or TCP.
- Do not treat protocol `88` control packets as ordinary transit traffic.
- Do not write directly into route-table arrays.
- Do not let Router forwarding read EIGRP topology state.
- Do not install an infeasible route that fails `reported_distance < fd`.
- Do not divide by zero in metric computation.
- Do not use milliseconds for scheduler timestamps.
- Do not assume multicast `224.0.0.10` works until the send path supports it.
- Do not free Router, Simulator, or interfaces from EIGRP state.
