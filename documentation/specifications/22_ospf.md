# Module 22 - OSPF

**Files:** `src/protocols/ospf.c`, `src/protocols/ospf.h`
**Status:** Ready for implementation; source files do not exist yet
**Depends on:** `router`, `route_table`, `ip`, `packet`, `interface`,
`scheduler`, `event`, `simulator`, `byte_order`

## Concepts First

OSPF means Open Shortest Path First.

RIP is distance-vector: routers tell neighbors "I can reach this prefix with
metric M." OSPF is link-state: each router describes its own links, floods that
description to the area, and every router builds the same topology map.

After each router has the topology map, each router runs Dijkstra's shortest
path algorithm from itself and installs the resulting best paths into the route
table.

This simulator implements a simplified OSPFv2 for IPv4:

- single area, area `0`
- router LSAs only in the first milestone
- no authentication
- no DR/BDR election in the first milestone
- fixed-size LSDB
- fixed maximum links per LSA
- SPF output installed into `RouteTable` as `ROUTE_PROTO_OSPF`

### Link-State Database

The LSDB is the link-state database.

It stores the most recent LSA learned from each advertising router.

An LSA says:

```text
Router R has links:
  to router A, cost 10
  to router B, cost 20
  to network N, cost 1
```

Every router in the same area should eventually have the same LSDB.

Forwarding does not use the LSDB directly. Forwarding uses the route-table FIB.

### LSA Flooding

Flooding means that when a router receives a newer LSA, it:

- stores it in the LSDB
- forwards it to other OSPF neighbors
- schedules SPF

The router should not flood the LSA back out the interface it came from.

Duplicate or older LSAs are ignored.

### Neighbor Discovery

OSPF neighbors are discovered with Hello packets.

Each OSPF-enabled interface periodically sends Hello packets. A neighbor moves
through a small state machine as bidirectional communication is confirmed.

For the first simulator milestone, the meaningful states are:

- `DOWN`: no valid neighbor
- `INIT`: heard a Hello from neighbor
- `TWOWAY`: neighbor's Hello lists our router ID
- `FULL`: LSDB exchange is considered complete

The full RFC state machine includes ExStart, Exchange, and Loading. This spec
defines them so the state model is recognizable, but the first implementation
may collapse database exchange from `TWOWAY` to `FULL` after accepting LSAs.

### SPF And Dijkstra

SPF means Shortest Path First.

OSPF runs Dijkstra's algorithm over the LSDB:

```text
dist[self] = 0
all other dist = infinity

repeat:
  pick unvisited router with smallest dist
  relax each link from that router
```

The result is the lowest-cost next hop and egress interface for each reachable
destination. OSPF then installs those routes into the Router's `RouteTable`.

Before installing new OSPF routes, OSPF should flush old OSPF routes:

```c
route_table_flush_proto(&router->route_tbl, ROUTE_PROTO_OSPF);
```

Then it adds the current SPF result with `ROUTE_PROTO_OSPF`.

### OSPF Transport

OSPF does not use UDP or TCP.

OSPFv2 uses IPv4 protocol number `89` directly.

That means the router needs a local IP protocol dispatch path for packets
addressed to OSPF multicast/local control-plane destinations.

Current-stack integration warning: the current Router spec is forwarding-first
and does not yet define a Host-like `IpStack` for Router local control-plane
protocols. OSPF receive requires one of these design choices before
implementation is complete:

- Router owns an `IpStack` and registers `IPPROTO_OSPF -> ospf_receive`, or
- Router receive explicitly intercepts protocol `89` before forwarding, or
- a shared control-plane dispatch mechanism is added for routers.

Do not hide this integration point. OSPF cannot be correct if protocol `89`
packets are only forwarded as transit traffic.

### Multicast Addresses

OSPF uses link-local multicast:

```text
224.0.0.5  AllSPFRouters
224.0.0.6  AllDRouters
```

The first milestone uses `224.0.0.5`.

Current-stack warning: current `ip_output` is subnet-oriented and does not
guarantee multicast output. OSPF send path must either add multicast support to
IP output or use an OSPF-specific link-local send helper that still preserves
IPv4 and Ethernet ownership rules.

## Purpose

The OSPF module implements simplified OSPFv2 control-plane behavior for Router
objects.

It provides:

- OSPF wire header structures
- Hello packet send and receive
- neighbor table and state updates
- LSDB storage
- LSA freshness checks
- LSA flooding
- debounced SPF scheduling
- Dijkstra shortest-path computation
- route installation into Router route table
- dead-neighbor timeout handling

It does not:

- forward data packets
- own Router interfaces
- replace the route table
- implement areas beyond area `0`
- implement DR/BDR election in the first milestone
- implement authentication
- implement all RFC database-exchange details in the first milestone

## Architecture Boundary

| Responsibility | Owner |
| --- | --- |
| Store neighbors | OSPF |
| Store LSDB | OSPF |
| Flood LSAs | OSPF send path |
| Run SPF | OSPF |
| Store route candidates | RouteTable |
| Select active forwarding route | RouteTable FIB |
| Forward data packets | Router |
| Schedule Hello/dead/SPF events | Scheduler |
| Send IPv4 protocol 89 packets | IP/link output path |

OSPF should call Router/RouteTable public APIs. It must not write directly into
route-table arrays.

Router forwarding should not read the LSDB.

## Data Model

### Constants

```c
#define OSPF_VERSION             2
#define OSPF_PROTO_NUM           89
#define OSPF_AREA_BACKBONE       0
#define OSPF_ALLSPFROUTERS       0xE0000005u
#define OSPF_ALLDROUTERS         0xE0000006u

#define OSPF_HELLO_INTERVAL_US   10000000ULL
#define OSPF_DEAD_INTERVAL_US    40000000ULL
#define OSPF_SPF_DELAY_US        500000ULL

#define OSPF_LSDB_SIZE           256
#define OSPF_MAX_NEIGHBORS       32
#define OSPF_MAX_IFACES          16
#define OSPF_MAX_LSA_LINKS       4
#define OSPF_INFINITY            0xFFFFFFFFu
```

Use microseconds for timers because `Scheduler.now` and `Event.timestamp` use
microseconds.

### Packet Types

```c
#define OSPF_TYPE_HELLO 1
#define OSPF_TYPE_DBD   2
#define OSPF_TYPE_LSR   3
#define OSPF_TYPE_LSU   4
#define OSPF_TYPE_LSACK 5
```

The first milestone must support Hello and LSU enough to build neighbors and
flood router LSAs. DBD/LSR/LSAck may be parsed as recognized but unsupported
until full database exchange is implemented.

### `OspfHeader`

```c
typedef struct __attribute__((packed)) OspfHeader {
    uint8_t  version;
    uint8_t  type;
    uint16_t pkt_len;
    uint32_t router_id;
    uint32_t area_id;
    uint16_t checksum;
    uint16_t au_type;
    uint64_t auth_data;
} OspfHeader;
```

Header length is `24` bytes.

Multi-byte fields are network byte order on the wire.

`au_type` and `auth_data` are zero in this simulator.

### `OspfHello`

```c
typedef struct __attribute__((packed)) OspfHello {
    uint32_t network_mask;
    uint16_t hello_interval;
    uint8_t  options;
    uint8_t  router_priority;
    uint32_t dead_interval;
    uint32_t dr;
    uint32_t bdr;
} OspfHello;
```

The neighbor list follows the fixed Hello body. Each neighbor ID is a 32-bit
router ID.

### `OspfLsaLink`

```c
typedef struct __attribute__((packed)) OspfLsaLink {
    uint32_t link_id;
    uint32_t link_data;
    uint8_t  type;
    uint8_t  num_tos;
    uint16_t metric;
} OspfLsaLink;
```

Suggested link types:

```c
#define OSPF_LINK_P2P      1
#define OSPF_LINK_TRANSIT  2
#define OSPF_LINK_STUB     3
```

### Neighbor State

```c
typedef enum OspfNbrState {
    OSPF_NBR_DOWN,
    OSPF_NBR_ATTEMPT,
    OSPF_NBR_INIT,
    OSPF_NBR_TWOWAY,
    OSPF_NBR_EXSTART,
    OSPF_NBR_EXCHANGE,
    OSPF_NBR_LOADING,
    OSPF_NBR_FULL
} OspfNbrState;
```

### `OspfNeighbor`

```c
typedef struct OspfNeighbor {
    uint32_t      router_id;
    uint32_t      ip_addr;
    OspfNbrState  state;
    uint64_t      last_hello_ts;
    Interface    *iface;
    int           valid;
} OspfNeighbor;
```

Router ID and IP address are host order.

`iface` is borrowed.

### `OspfLsaEntry`

```c
typedef struct OspfLsaEntry {
    uint32_t    lsa_id;
    uint32_t    adv_router;
    uint32_t    seq_num;
    uint16_t    checksum;
    uint8_t     lsa_type;
    uint8_t     valid;
    OspfLsaLink links[OSPF_MAX_LSA_LINKS];
    int         link_count;
} OspfLsaEntry;
```

The first milestone stores router LSAs with up to four links.

### `OspfIface`

```c
typedef struct OspfIface {
    Interface *iface;
    uint16_t   cost;
    int        valid;
} OspfIface;
```

OSPF needs per-interface cost. Do not store only `Interface *` if the algorithm
needs a link metric.

### `OspfState`

```c
typedef struct OspfState {
    uint32_t      router_id;
    uint32_t      area_id;

    OspfNeighbor  neighbors[OSPF_MAX_NEIGHBORS];
    int           neighbor_count;

    OspfLsaEntry  lsdb[OSPF_LSDB_SIZE];
    int           lsdb_count;

    OspfIface     ifaces[OSPF_MAX_IFACES];
    int           iface_count;

    Simulator    *sim;
    Router       *router;
} OspfState;
```

`OspfState` is per router.

## Ownership And Lifetime

The owner allocates `OspfState`; OSPF initializes it.

OSPF borrows `Simulator *`, `Router *`, and `Interface *` pointers.

OSPF does not free Router, Simulator, or interfaces.

`ospf_receive` receives ownership of a packet from the IP protocol-dispatch
path. It must free the packet on parse errors and after consuming control-plane
messages, unless it transfers the packet to another helper that documents
ownership transfer.

Scheduled OSPF events borrow `OspfState *` as context. The owner must keep the
state alive while OSPF events can fire.

## Public API

```c
void ospf_init(OspfState *state,
               Simulator *sim,
               Router *router,
               uint32_t router_id);

int ospf_enable_iface(OspfState *state,
                      Interface *iface,
                      uint16_t cost);

int ospf_receive(Interface *iface,
                 Packet *pkt,
                 void *ctx);

int ospf_send_hello(OspfState *state, Interface *iface);

int ospf_flood_lsa(OspfState *state,
                   const OspfLsaEntry *lsa,
                   Interface *except_iface);

int ospf_run_spf(OspfState *state);

void ospf_hello_timer(const Event *e, void *ctx);

void ospf_dead_timer(const Event *e, void *ctx);

void ospf_spf_timer(const Event *e, void *ctx);
```

`ospf_receive` has the IP protocol handler shape:

```c
int handler(Interface *iface, Packet *pkt, void *ctx);
```

The intended registration is:

```c
ip_stack_register_protocol(router_control_stack,
                           OSPF_PROTO_NUM,
                           ospf_receive,
                           ospf_state);
```

The exact owner of `router_control_stack` must be resolved in Router/IP
integration before OSPF receive is complete.

## Function Behavior

### `ospf_init`

Required behavior:

- If `state == NULL`, return immediately.
- Zero all OSPF state.
- Store `sim`, `router`, `router_id`, and area `0`.
- Initialize neighbor count, LSDB count, and interface count to zero.
- If `sim != NULL && sim->sched != NULL`, schedule first Hello event at
  `scheduler_now(sim->sched) + OSPF_HELLO_INTERVAL_US`.

Protocol registration for IP protocol `89` may be done by the Router/control
plane owner rather than by `ospf_init`, because `ospf_init` may not own the IP
stack.

### `ospf_enable_iface`

Required behavior:

- If `state == NULL || iface == NULL`, return `-1`.
- If `cost == 0`, return `-1`.
- If the interface is already enabled, update its cost and return `0`.
- If `iface_count >= OSPF_MAX_IFACES`, return `-1`.
- Store the interface and cost in the first free OSPF interface slot.
- Increment `iface_count`.
- Return `0`.

OSPF does not take ownership of the interface.

### `ospf_receive`

Required behavior:

- If `iface == NULL`, return `-1`.
- If `pkt == NULL`, increment `iface->rx_errors` and return `-1`.
- If `ctx == NULL`, free `pkt`, increment `iface->rx_errors`, return `-1`.
- Cast `ctx` to `OspfState *`.
- If packet is shorter than `OspfHeader`, free `pkt`, increment `rx_errors`,
  return `-1`.
- Validate OSPF version `2`.
- Validate area ID equals state area ID.
- Validate packet length field is at least header length and no larger than
  visible packet length.
- Validate authentication type/data are zero.
- Validate checksum according to the OSPF checksum rule, or document checksum
  as disabled for the first milestone and require checksum field zero.
- Dispatch by packet type:
  - Hello: process neighbor discovery
  - LSU: update LSDB, flood newer LSAs, schedule SPF
  - DBD/LSR/LSAck: return unsupported in first milestone or implement database
    exchange if the project reaches that phase
- Free `pkt` after processing.
- Return `0` for accepted control-plane packets, `-1` for malformed packets.

### `ospf_send_hello`

Required behavior:

- If `state == NULL || iface == NULL`, return `-1`.
- If `state->sim == NULL`, return `-1`.
- Build OSPF Hello packet:
  - common header type `OSPF_TYPE_HELLO`
  - router ID from state
  - area ID from state
  - network mask from interface prefix length
  - hello interval and dead interval in seconds or documented wire units
  - neighbor list from valid known neighbors on that interface
- Send to `OSPF_ALLSPFROUTERS`.
- Return `0` on success, `-1` on failure.

Current-stack warning: this requires a protocol-89 send path that can send
link-local multicast. Do not route Hello packets through ordinary unicast
forwarding.

### `ospf_flood_lsa`

Required behavior:

- If `state == NULL || lsa == NULL`, return `-1`.
- For each enabled OSPF interface:
  - skip `except_iface`
  - send LSU carrying the LSA
- Return `0` if all needed sends succeed.
- Return `-1` if any required send fails.

### `ospf_run_spf`

Required behavior:

- If `state == NULL || state->router == NULL`, return `-1`.
- Build a graph from valid LSDB entries.
- Run Dijkstra from `state->router_id`.
- Flush old OSPF routes:

```c
route_table_flush_proto(&state->router->route_tbl, ROUTE_PROTO_OSPF);
```

- For each reachable destination represented by the LSDB:
  - compute next-hop IP
  - compute egress interface
  - compute total cost
  - install route with `ROUTE_PROTO_OSPF`
- Return `0`.

For the first milestone, SPF may install host routes to reachable router IDs.
Network/stub route installation can be added after router-LSA link semantics
are fully represented.

### Timer Handlers

`ospf_hello_timer`:

- If context is missing, return.
- Send Hello on each enabled interface.
- Schedule next Hello.

`ospf_dead_timer`:

- If context is missing, return.
- Mark neighbors dead if `now - last_hello_ts >= OSPF_DEAD_INTERVAL_US`.
- If any neighbor dies, invalidate or age affected LSAs and schedule SPF.

`ospf_spf_timer`:

- If context is missing, return.
- Call `ospf_run_spf`.

## Flow Charts

### Initialization

```text
ospf_init(state, sim, router, router_id)
  |
  +-- null state: return
  +-- zero state
  +-- store sim/router/router_id/area 0
  +-- schedule first Hello if scheduler exists
```

### Hello Receive

```text
ospf_receive(iface, pkt, state)
  |
  +-- validate OSPF header
  +-- type == Hello
  +-- find or create neighbor by router_id
  +-- update last_hello_ts
  +-- if our router_id is in neighbor list: TWOWAY/FULL
  +-- else INIT
  +-- free packet
```

### LSA Flood And SPF

```text
LSU received
  |
  +-- for each LSA:
        |
        +-- older or duplicate: ignore
        +-- newer:
              store in LSDB
              flood to other OSPF interfaces
              schedule SPF after OSPF_SPF_DELAY_US
```

### SPF

```text
ospf_run_spf(state)
  |
  +-- build graph from LSDB
  +-- run Dijkstra from local router_id
  +-- route_table_flush_proto(... OSPF)
  +-- route_table_add(... OSPF) for reachable results
```

## ACSL Contracts

The contracts belong in `ospf.h`. Use literal bounds:

- neighbors: `32`
- LSDB entries: `256`
- OSPF interfaces: `16`
- links per LSA: `4`
- OSPF header bytes: `24`

### Shared Predicates

```c
/*@
    predicate ospf_neighbor_count_valid(OspfState *state) =
        0 <= state->neighbor_count && state->neighbor_count <= 32;

    predicate ospf_lsdb_count_valid(OspfState *state) =
        0 <= state->lsdb_count && state->lsdb_count <= 256;

    predicate ospf_iface_count_valid(OspfState *state) =
        0 <= state->iface_count && state->iface_count <= 16;

    predicate ospf_lsa_slot_valid(OspfState *state, integer i) =
        0 <= i && i < 256 ==>
            (state->lsdb[i].valid == 0 ||
             (state->lsdb[i].valid == 1 &&
              0 <= state->lsdb[i].link_count &&
              state->lsdb[i].link_count <= 4));

    predicate ospf_state_well_formed(OspfState *state) =
        \valid(state) &&
        ospf_neighbor_count_valid(state) &&
        ospf_lsdb_count_valid(state) &&
        ospf_iface_count_valid(state) &&
        \forall integer i; 0 <= i && i < 256 ==>
            ospf_lsa_slot_valid(state, i);
*/
```

### `ospf_init`

```c
/*@
    behavior null:
        assumes state == \null;
        assigns \nothing;

    behavior valid:
        assumes \valid(state);
        assigns state->router_id,
                state->area_id,
                state->neighbors[0 .. 31],
                state->neighbor_count,
                state->lsdb[0 .. 255],
                state->lsdb_count,
                state->ifaces[0 .. 15],
                state->iface_count,
                state->sim,
                state->router;
        ensures state->router_id == router_id;
        ensures state->area_id == 0;
        ensures state->neighbor_count == 0;
        ensures state->lsdb_count == 0;
        ensures state->iface_count == 0;
        ensures state->sim == sim;
        ensures state->router == router;

    complete behaviors;
    disjoint behaviors;
*/
void ospf_init(OspfState *state,
               Simulator *sim,
               Router *router,
               uint32_t router_id);
```

### `ospf_enable_iface`

```c
/*@
    behavior bad_input:
        assumes state == \null || iface == \null || cost == 0;
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes ospf_state_well_formed(state);
        assumes \valid(iface);
        assumes cost != 0;
        assigns state->ifaces[0 .. 15],
                state->iface_count;
        ensures \result == 0 || \result == -1;

    complete behaviors;
    disjoint behaviors;
*/
int ospf_enable_iface(OspfState *state, Interface *iface, uint16_t cost);
```

### `ospf_receive`

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
        assumes ospf_state_well_formed((OspfState *)ctx);
        assigns iface->rx_errors,
                iface->rx_dropped,
                ((OspfState *)ctx)->neighbors[0 .. 31],
                ((OspfState *)ctx)->neighbor_count,
                ((OspfState *)ctx)->lsdb[0 .. 255],
                ((OspfState *)ctx)->lsdb_count;
        ensures \result == 0 || \result == -1;

    complete behaviors;
    disjoint behaviors;
*/
int ospf_receive(Interface *iface, Packet *pkt, void *ctx);
```

### `ospf_run_spf`

```c
/*@
    behavior bad_input:
        assumes state == \null || state->router == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes ospf_state_well_formed(state);
        assumes state->router != \null;
        assigns state->router->route_tbl.rib[0 .. 255],
                state->router->route_tbl.rib_count,
                state->router->route_tbl.fib[0 .. 255],
                state->router->route_tbl.fib_count;
        ensures \result == 0 || \result == -1;

    complete behaviors;
    disjoint behaviors;
*/
int ospf_run_spf(OspfState *state);
```

### Timer Handlers

```c
/*@
    behavior null_input:
        assumes e == \null || ctx == \null;
        assigns \nothing;

    behavior valid:
        assumes e != \null && ctx != \null;
        assumes ospf_state_well_formed((OspfState *)ctx);
        assigns ((OspfState *)ctx)->neighbors[0 .. 31],
                ((OspfState *)ctx)->lsdb[0 .. 255],
                ((OspfState *)ctx)->lsdb_count;
*/
void ospf_hello_timer(const Event *e, void *ctx);
void ospf_dead_timer(const Event *e, void *ctx);
void ospf_spf_timer(const Event *e, void *ctx);
```

## KLEVA Verification Plan

Minimum KLEVA tests:

1. `ospf_init(NULL, ...)` does not crash.
2. Valid init clears neighbor, LSDB, and interface counts.
3. Valid init stores router ID, area ID, simulator, and router.
4. Valid init schedules first Hello when scheduler exists.
5. `ospf_enable_iface` rejects NULL state, NULL interface, and zero cost.
6. `ospf_enable_iface` adds first interface and cost.
7. `ospf_enable_iface` updates cost for duplicate interface.
8. `ospf_enable_iface` rejects full interface list.
9. `ospf_receive` rejects NULL interface.
10. `ospf_receive` with NULL packet increments `rx_errors`.
11. `ospf_receive` with NULL context frees packet and increments `rx_errors`.
12. Too-short OSPF packet is rejected.
13. Wrong OSPF version is rejected.
14. Wrong area ID is rejected.
15. Bad packet length is rejected.
16. Hello creates or updates neighbor entry.
17. Hello without our router ID leaves neighbor in INIT.
18. Hello listing our router ID moves neighbor to TWOWAY or FULL.
19. Newer LSA updates LSDB.
20. Duplicate or older LSA is ignored.
21. Newer LSA is flooded except on ingress interface.
22. Newer LSA schedules SPF.
23. SPF flushes old OSPF routes.
24. SPF installs reachable routes as `ROUTE_PROTO_OSPF`.
25. Dead timer marks stale neighbor down.
26. Dead neighbor schedules SPF.
27. OSPF protocol 89 receive path is covered by Router/IP integration test.
28. OSPF multicast send path is covered by IP multicast or OSPF send-helper
    test.

## Common Mistakes

- Do not implement OSPF over UDP.
- Do not let Router forwarding treat protocol 89 control packets as ordinary
  transit traffic when they are addressed to the router/control plane.
- Do not write directly into route-table arrays.
- Do not make forwarding read the LSDB.
- Do not use milliseconds for scheduler timestamps.
- Do not assume multicast `224.0.0.5` works until the IP/link send path
  supports it.
- Do not run SPF after every LSA immediately if a debounce timer is available.
- Do not forget to flush old OSPF routes before installing new SPF results.
- Do not free Router, Simulator, or interfaces from OSPF state.
