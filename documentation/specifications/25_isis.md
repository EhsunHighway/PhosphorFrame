# Module 25 - IS-IS

**Files:** `src/protocols/isis.c`, `src/protocols/isis.h`
**Status:** Ready for implementation; source files do not exist yet
**Depends on:** `router`, `route_table`, `packet`, `interface`, `ethernet`,
`ip`, `scheduler`, `event`, `simulator`, `byte_order`

## Concepts First

IS-IS means Intermediate System to Intermediate System.

It is a link-state routing protocol like OSPF: routers flood link-state
information, build an LSDB, run Dijkstra SPF, and install routes into the route
table.

The most important difference is transport.

Real IS-IS does not run over IP. It runs directly over layer 2 using ISO/CLNS
encapsulation. That means IS-IS can still form adjacencies even when IP
addresses are not configured correctly.

This simulator may choose an IP-encapsulated first milestone for simplicity,
but the spec must be honest:

- real IS-IS is L2-native
- IP protocol `124` is a simulator simplification, not real IS-IS
- if the simulator uses IP protocol `124`, Router needs control-plane dispatch
  for that protocol just like OSPF/EIGRP
- if the simulator uses L2-native IS-IS later, Ethernet needs an IS-IS demux
  path

### Link-State Protocol

IS-IS routers advertise LSPs.

An LSP describes a router and its links:

```text
system-id R1
  neighbor R2 metric 10
  neighbor R3 metric 20
```

Each router stores LSPs in its LSDB. When the LSDB changes, the router schedules
SPF and installs the resulting best paths into the RouteTable.

Forwarding does not read the LSDB. Forwarding uses the route-table FIB.

### NET And System ID

IS-IS identifies routers with a NET, or Network Entity Title.

Real NETs are variable-length NSAP-style addresses. This simulator uses a fixed
simplified identity:

- area identifier
- system ID
- NSEL `0`

For implementation simplicity, store an 8-byte NET and a 6-byte system ID.

The system ID is the stable router identity used in neighbors, LSP IDs, and SPF.

### Level 1 And Level 2

IS-IS has two hierarchy levels:

- Level 1: routing inside an area
- Level 2: routing between areas, backbone-like behavior

This simulator implements Level 1 only in the first milestone.

No L1/L2 route leaking, attached-bit logic, or inter-area behavior is required
yet.

### IIH, LSP, CSNP, PSNP

Important IS-IS PDU types:

| PDU | Meaning |
| --- | --- |
| IIH | IS-IS Hello, used for neighbor discovery. |
| LSP | Link State PDU, carries topology information. |
| CSNP | Complete Sequence Number PDU, summarizes LSDB. |
| PSNP | Partial Sequence Number PDU, requests or acknowledges specific LSPs. |

The first milestone should implement IIH and LSP.

CSNP/PSNP can be recognized and ignored or left for a later database sync
milestone.

### SPF

SPF is Dijkstra's shortest-path algorithm over the LSDB.

IS-IS SPF is conceptually the same as OSPF SPF:

```text
start at local system ID
compute shortest metric to every reachable system
derive next hop and egress interface
install routes as ROUTE_PROTO_ISIS
```

Before installing new IS-IS routes, flush old IS-IS routes:

```c
route_table_flush_proto(&router->route_tbl, ROUTE_PROTO_ISIS);
```

## Purpose

The IS-IS module implements simplified Level-1 IS-IS route exchange for Router
objects.

It provides:

- IS-IS PDU structures
- fixed NET/system-ID state
- enabled-interface tracking
- neighbor discovery through IIH
- LSP generation
- LSDB storage and freshness checks
- LSP flooding
- debounced SPF scheduling
- SPF route installation into RouteTable
- hold timers and LSP regeneration timers

It does not:

- forward data packets
- own Router interfaces
- replace RouteTable
- implement Level 2 routing
- implement CSNP/PSNP synchronization in the first milestone
- implement wide metrics in the first milestone
- pretend IP protocol `124` is real IS-IS

## Architecture Boundary

| Responsibility | Owner |
| --- | --- |
| Neighbor table | IS-IS |
| LSDB | IS-IS |
| LSP flooding | IS-IS send path |
| SPF | IS-IS |
| Route candidate storage | RouteTable RIB |
| Active forwarding route | RouteTable FIB |
| Data packet forwarding | Router |
| L2-native demux, future | Ethernet/input dispatch |
| IP-encapsulated demux, milestone option | Router/IP control-plane dispatch |
| Timers | Scheduler |

IS-IS should call Router/RouteTable public APIs. It must not write directly into
route-table arrays.

Router forwarding must not read the IS-IS LSDB.

## Data Model

### Constants

```c
#define ISIS_DISCR              0x83
#define ISIS_VERSION            1
#define ISIS_NET_LEN            8
#define ISIS_SYSTEM_ID_LEN      6
#define ISIS_LSP_ID_LEN         8

#define ISIS_PROTO_NUM          124
#define ISIS_ETHERTYPE          0x8870

#define ISIS_HELLO_US           10000000ULL
#define ISIS_HOLD_MULTIPLIER    3
#define ISIS_HOLD_US            30000000ULL
#define ISIS_SPF_DELAY_US       500000ULL
#define ISIS_LSP_REGEN_US       900000000ULL

#define ISIS_LSDB_SIZE          256
#define ISIS_MAX_NEIGHBORS      32
#define ISIS_MAX_IFACES         16
#define ISIS_MAX_LSP_NEIGHBORS  8

#define ISIS_METRIC_DEFAULT     10
#define ISIS_METRIC_MAX         63
```

Use microseconds for timers because `Scheduler.now` and `Event.timestamp` use
microseconds.

### PDU Types

```c
#define ISIS_PDU_IIH   15
#define ISIS_PDU_LSP   18
#define ISIS_PDU_CSNP  24
#define ISIS_PDU_PSNP  26
```

### `IsisHeader`

```c
typedef struct __attribute__((packed)) IsisHeader {
    uint8_t discr;
    uint8_t hdr_len;
    uint8_t version;
    uint8_t id_len;
    uint8_t pdu_type;
    uint8_t version2;
    uint8_t reserved;
    uint8_t max_areas;
} IsisHeader;
```

Header length is `8` bytes in this simplified model.

`discr` must be `0x83`.

`id_len == 0` means 6-byte system ID in real IS-IS convention.

### `IsisIih`

```c
typedef struct __attribute__((packed)) IsisIih {
    uint8_t  circuit_type;
    uint8_t  src_id[ISIS_SYSTEM_ID_LEN];
    uint16_t hold_time;
    uint16_t pdu_len;
    uint8_t  priority;
    uint8_t  dis_id[7];
} IsisIih;
```

The first milestone uses Level-1 circuit type.

TLVs may follow the fixed IIH body.

### `IsisLspHeader`

```c
typedef struct __attribute__((packed)) IsisLspHeader {
    uint16_t pdu_len;
    uint16_t remaining_lifetime;
    uint8_t  lsp_id[ISIS_LSP_ID_LEN];
    uint32_t seq_num;
    uint16_t checksum;
    uint8_t  type_block;
} IsisLspHeader;
```

The LSP ID is:

```text
system-id[6] + pseudonode-id[1] + fragment-id[1]
```

First milestone can use pseudonode `0` and fragment `0`.

### `IsisIface`

```c
typedef struct IsisIface {
    Interface *iface;
    uint16_t   metric;
    int        valid;
} IsisIface;
```

IS-IS needs per-interface metric.

### `IsisNeighbor`

```c
typedef struct IsisNeighbor {
    uint8_t    sys_id[ISIS_SYSTEM_ID_LEN];
    uint64_t   hold_deadline;
    Interface *iface;
    int        valid;
} IsisNeighbor;
```

`iface` is borrowed.

### `IsisLspEntry`

```c
typedef struct IsisLspEntry {
    uint8_t  lsp_id[ISIS_LSP_ID_LEN];
    uint32_t seq_num;
    uint16_t checksum;
    uint16_t lifetime;
    uint8_t  neighbor_ids[ISIS_MAX_LSP_NEIGHBORS][ISIS_SYSTEM_ID_LEN];
    uint16_t metrics[ISIS_MAX_LSP_NEIGHBORS];
    int      neighbor_count;
    int      valid;
} IsisLspEntry;
```

This simplified LSDB stores neighbor system IDs and metrics directly.

### `IsisState`

```c
typedef struct IsisState {
    uint8_t       net[ISIS_NET_LEN];
    uint8_t       system_id[ISIS_SYSTEM_ID_LEN];

    IsisNeighbor  neighbors[ISIS_MAX_NEIGHBORS];
    int           neighbor_count;

    IsisLspEntry  lsdb[ISIS_LSDB_SIZE];
    int           lsdb_count;

    IsisIface     ifaces[ISIS_MAX_IFACES];
    int           iface_count;

    uint32_t      local_lsp_seq;

    Simulator    *sim;
    Router       *router;
} IsisState;
```

`IsisState` is per router.

## Ownership And Lifetime

The owner allocates `IsisState`; IS-IS initializes it.

IS-IS borrows `Simulator *`, `Router *`, and `Interface *` pointers.

IS-IS does not free Router, Simulator, or interfaces.

`isis_receive` receives ownership of a packet from the selected control-plane
demux path. It must free the packet after parsing or on errors unless ownership
is transferred to a documented helper.

Scheduled IS-IS events borrow `IsisState *` as context. The owner must keep
state alive while events can fire.

## Public API

```c
void isis_init(IsisState *state,
               Simulator *sim,
               Router *router,
               const uint8_t net[ISIS_NET_LEN]);

int isis_enable_iface(IsisState *state,
                      Interface *iface,
                      uint16_t metric);

int isis_receive(Interface *iface,
                 Packet *pkt,
                 void *ctx);

int isis_send_iih(IsisState *state, Interface *iface);

int isis_flood_lsp(IsisState *state,
                   const IsisLspEntry *lsp,
                   Interface *except_iface);

int isis_generate_lsp(IsisState *state);

int isis_run_spf(IsisState *state);

void isis_hello_timer(const Event *e, void *ctx);

void isis_hold_timer(const Event *e, void *ctx);

void isis_spf_timer(const Event *e, void *ctx);

void isis_lsp_regen_timer(const Event *e, void *ctx);
```

If the simulator chooses IP-encapsulated IS-IS first, `isis_receive` has the IP
protocol handler shape:

```c
int handler(Interface *iface, Packet *pkt, void *ctx);
```

If the simulator chooses real L2-native IS-IS later, Ethernet/input dispatch
should call the same `isis_receive` after identifying IS-IS ethertype/SNAP.

## Function Behavior

### `isis_init`

Required behavior:

- If `state == NULL`, return immediately.
- Zero all IS-IS state.
- Store `sim` and `router`.
- If `net != NULL`, copy 8 NET bytes.
- Derive or copy the 6-byte system ID from NET bytes.
- Initialize counts to zero.
- Set `local_lsp_seq = 1`.
- If scheduler exists, schedule first Hello and LSP regeneration events.

Control-plane registration is owned by Router/IP/Ethernet integration, not
necessarily by `isis_init`.

### `isis_enable_iface`

Required behavior:

- If `state == NULL || iface == NULL`, return `-1`.
- If `metric == 0 || metric > ISIS_METRIC_MAX`, return `-1`.
- If interface is already enabled, update metric and return `0`.
- If interface table is full, return `-1`.
- Store interface and metric.
- Increment interface count.
- Return `0`.

IS-IS does not take ownership of the interface.

### `isis_receive`

Required behavior:

- If `iface == NULL`, return `-1`.
- If `pkt == NULL`, increment `iface->rx_errors` and return `-1`.
- If `ctx == NULL`, free packet, increment `rx_errors`, return `-1`.
- Cast context to `IsisState *`.
- If packet is shorter than `IsisHeader`, free packet, increment `rx_errors`,
  return `-1`.
- Validate discriminator, version fields, header length, and PDU type.
- Dispatch by PDU:
  - IIH: refresh or create neighbor
  - LSP: install newer LSP, flood to other interfaces, schedule SPF
  - CSNP/PSNP: recognize and ignore or return unsupported in first milestone
- Free packet before returning.

### `isis_send_iih`

Required behavior:

- If `state == NULL || iface == NULL`, return `-1`.
- If `state->sim == NULL`, return `-1`.
- Build IIH PDU with:
  - common IS-IS header
  - Level-1 circuit type
  - source system ID
  - hold time
  - PDU length
- Send through the selected IS-IS transport:
  - L2-native future path: Ethernet IS-IS multicast
  - IP-encapsulated milestone path: protocol `ISIS_PROTO_NUM`
- Return `0` on success, `-1` on failure.

### `isis_generate_lsp`

Required behavior:

- If `state == NULL`, return `-1`.
- Build this router's local LSP from valid neighbors.
- Increment local LSP sequence.
- Store or update the local LSDB entry.
- Flood the local LSP.
- Schedule SPF.
- Return `0` on success.

### `isis_flood_lsp`

Required behavior:

- If `state == NULL || lsp == NULL`, return `-1`.
- For each enabled interface:
  - skip `except_iface`
  - send LSP
- Return `0` if all required sends succeed, else `-1`.

### `isis_run_spf`

Required behavior:

- If `state == NULL || state->router == NULL`, return `-1`.
- Build graph from valid LSDB entries.
- Run Dijkstra from local system ID.
- Flush old IS-IS routes:

```c
route_table_flush_proto(&state->router->route_tbl, ROUTE_PROTO_ISIS);
```

- Install reachable results with `ROUTE_PROTO_ISIS`.
- Return `0`.

The first milestone may install host routes to router loopbacks/system IDs.
Network TLV route installation can be added after TLV representation is
expanded.

### Timer Handlers

`isis_hello_timer`:

- Send IIH on each enabled interface.
- Schedule next Hello.

`isis_hold_timer`:

- Expire neighbors whose hold deadline passed.
- If any neighbor changed, generate new LSP and schedule SPF.

`isis_spf_timer`:

- Run SPF.

`isis_lsp_regen_timer`:

- Regenerate local LSP before lifetime expiry.
- Schedule next regeneration.

## Flow Charts

### Initialization

```text
isis_init(state, sim, router, net)
  |
  +-- null state: return
  +-- zero state
  +-- copy NET and derive system ID
  +-- local_lsp_seq = 1
  +-- schedule Hello and LSP regeneration if scheduler exists
```

### IIH Receive

```text
isis_receive(iface, pkt, state)
  |
  +-- validate IS-IS header
  +-- PDU IIH
  +-- find/create neighbor by system ID
  +-- set hold deadline
  +-- if new neighbor: generate LSP and schedule SPF
  +-- free packet
```

### LSP Receive

```text
LSP received
  |
  +-- older or duplicate: ignore
  +-- newer:
        store in LSDB
        flood except ingress interface
        schedule SPF
```

### SPF

```text
isis_run_spf(state)
  |
  +-- build graph from LSDB
  +-- run Dijkstra from local system ID
  +-- route_table_flush_proto(... ISIS)
  +-- route_table_add(... ISIS) for reachable results
```

## ACSL Contracts

The contracts belong in `isis.h`. Use literal bounds:

- NET bytes: `8`
- system ID bytes: `6`
- neighbors: `32`
- LSDB entries: `256`
- enabled interfaces: `16`
- LSP neighbors: `8`
- IS-IS header bytes: `8`

### Shared Predicates

```c
/*@
    predicate isis_neighbor_count_valid(IsisState *state) =
        0 <= state->neighbor_count && state->neighbor_count <= 32;

    predicate isis_lsdb_count_valid(IsisState *state) =
        0 <= state->lsdb_count && state->lsdb_count <= 256;

    predicate isis_iface_count_valid(IsisState *state) =
        0 <= state->iface_count && state->iface_count <= 16;

    predicate isis_lsp_slot_valid(IsisState *state, integer i) =
        0 <= i && i < 256 ==>
            (state->lsdb[i].valid == 0 ||
             (state->lsdb[i].valid == 1 &&
              0 <= state->lsdb[i].neighbor_count &&
              state->lsdb[i].neighbor_count <= 8));

    predicate isis_state_well_formed(IsisState *state) =
        \valid(state) &&
        isis_neighbor_count_valid(state) &&
        isis_lsdb_count_valid(state) &&
        isis_iface_count_valid(state) &&
        \forall integer i; 0 <= i && i < 256 ==>
            isis_lsp_slot_valid(state, i);
*/
```

### `isis_init`

```c
/*@
    behavior null:
        assumes state == \null;
        assigns \nothing;

    behavior valid:
        assumes \valid(state);
        assigns state->net[0 .. 7],
                state->system_id[0 .. 5],
                state->neighbors[0 .. 31],
                state->neighbor_count,
                state->lsdb[0 .. 255],
                state->lsdb_count,
                state->ifaces[0 .. 15],
                state->iface_count,
                state->local_lsp_seq,
                state->sim,
                state->router;
        ensures state->neighbor_count == 0;
        ensures state->lsdb_count == 0;
        ensures state->iface_count == 0;
        ensures state->local_lsp_seq == 1;
        ensures state->sim == sim;
        ensures state->router == router;

    complete behaviors;
    disjoint behaviors;
*/
void isis_init(IsisState *state,
               Simulator *sim,
               Router *router,
               const uint8_t net[8]);
```

### `isis_enable_iface`

```c
/*@
    behavior bad_input:
        assumes state == \null || iface == \null ||
                metric == 0 || metric > 63;
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes isis_state_well_formed(state);
        assumes \valid(iface);
        assumes metric != 0 && metric <= 63;
        assigns state->ifaces[0 .. 15],
                state->iface_count;
        ensures \result == 0 || \result == -1;

    complete behaviors;
    disjoint behaviors;
*/
int isis_enable_iface(IsisState *state, Interface *iface, uint16_t metric);
```

### `isis_receive`

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
        assumes isis_state_well_formed((IsisState *)ctx);
        assigns iface->rx_errors,
                ((IsisState *)ctx)->neighbors[0 .. 31],
                ((IsisState *)ctx)->neighbor_count,
                ((IsisState *)ctx)->lsdb[0 .. 255],
                ((IsisState *)ctx)->lsdb_count;
        ensures \result == 0 || \result == -1;

    complete behaviors;
    disjoint behaviors;
*/
int isis_receive(Interface *iface, Packet *pkt, void *ctx);
```

### `isis_run_spf`

```c
/*@
    behavior bad_input:
        assumes state == \null || state->router == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes isis_state_well_formed(state);
        assumes state->router != \null;
        assigns state->router->route_tbl.rib[0 .. 255],
                state->router->route_tbl.rib_count,
                state->router->route_tbl.fib[0 .. 255],
                state->router->route_tbl.fib_count;
        ensures \result == 0 || \result == -1;

    complete behaviors;
    disjoint behaviors;
*/
int isis_run_spf(IsisState *state);
```

## KLEVA Verification Plan

Minimum KLEVA tests:

1. `isis_init(NULL, ...)` does not crash.
2. Valid init clears neighbor, LSDB, and interface counts.
3. Valid init copies NET and derives system ID.
4. Valid init sets local LSP sequence to `1`.
5. `isis_enable_iface` rejects NULL state, NULL interface, metric zero, and
   metric greater than 63.
6. `isis_enable_iface` adds first interface and metric.
7. `isis_enable_iface` updates duplicate interface metric.
8. `isis_enable_iface` rejects full interface table.
9. `isis_receive` rejects NULL interface.
10. `isis_receive` with NULL packet increments `rx_errors`.
11. `isis_receive` with NULL context frees packet and increments `rx_errors`.
12. Too-short packet is rejected.
13. Bad discriminator is rejected.
14. Bad version is rejected.
15. IIH creates or refreshes neighbor.
16. New neighbor triggers local LSP generation.
17. Newer LSP updates LSDB.
18. Duplicate or older LSP is ignored.
19. Newer LSP floods except ingress interface.
20. Newer LSP schedules SPF.
21. SPF flushes old IS-IS routes.
22. SPF installs reachable routes as `ROUTE_PROTO_ISIS`.
23. Hold timer expires stale neighbors.
24. Neighbor expiration regenerates local LSP.
25. LSP regeneration increments sequence number.
26. Transport choice is covered by either IP-proto-124 integration test or
    L2-native Ethernet demux test.

## Common Mistakes

- Do not say real IS-IS runs over IP.
- Do not hide that IP protocol `124` is a simulator simplification.
- Do not make Router forwarding read the IS-IS LSDB.
- Do not write directly into route-table arrays.
- Do not use milliseconds for scheduler timestamps.
- Do not implement Level 2 behavior in the Level 1 milestone.
- Do not accept metrics greater than 63 in the narrow-metric milestone.
- Do not free Router, Simulator, or interfaces from IS-IS state.
