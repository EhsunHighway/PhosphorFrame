# Module 23 - BGP

**Files:** `src/protocols/bgp.c`, `src/protocols/bgp.h`
**Status:** Ready for implementation; source files do not exist yet
**Depends on:** `router`, `route_table`, `tcp`, `ip`, `packet`, `scheduler`,
`event`, `simulator`, `byte_order`

## Concepts First

BGP means Border Gateway Protocol.

RIP and OSPF are usually interior gateway protocols: they run inside one
administrative network. BGP is an exterior gateway protocol: it exchanges
reachability between autonomous systems.

This simulator implements a simplified eBGP:

- BGP version 4
- TCP port `179`
- configured peers
- OPEN, KEEPALIVE, UPDATE, and NOTIFICATION messages
- simplified path attributes
- best-path selection
- route installation into `RouteTable` as `ROUTE_PROTO_BGP`

### Autonomous System

An autonomous system, or AS, is a network under one routing administration.

An AS has an AS number. In this simulator, use 2-byte ASNs first:

```text
AS 65001
AS 65002
```

eBGP runs between routers in different ASes.

### Path Vector

BGP is a path-vector protocol.

An UPDATE does not only say "I can reach prefix P." It also carries path
attributes, especially AS_PATH:

```text
prefix 203.0.113.0/24
AS_PATH: 65002 65010 65044
NEXT_HOP: 10.0.0.2
```

AS_PATH is used for:

- loop detection
- path selection
- policy

If the local AS appears in the received AS_PATH, the route is a loop and must be
ignored.

### BGP Runs Over TCP

BGP does not run directly over IP and does not use UDP.

BGP peers establish a TCP session on port `179`. BGP messages are byte-stream
records inside that TCP connection.

This matters for implementation:

- BGP does not receive raw IP packets
- BGP does not bind an IP protocol number
- BGP must use `tcp_listen`, `tcp_connect`, `tcp_send`, and TCP callbacks
- BGP receive must handle BGP messages delivered as TCP payload packets

Current TCP integration note: current TCP has separate receive and connect
callbacks:

```c
typedef void (*TcpRecvHandler)(Tcb *tcb, Packet *payload, void *ctx);
typedef void (*TcpConnectHandler)(Tcb *tcb, void *ctx);
```

BGP must use the connect callback to send OPEN after TCP reaches
`TCP_ESTABLISHED`. It must not rely on `recv_handler(tcb, NULL, ctx)` as an
establishment signal.

### BGP RIBs

BGP commonly separates:

| Name | Meaning |
| --- | --- |
| Adj-RIB-In | Routes received from peers before best-path selection. |
| Loc-RIB | Best BGP route selected locally for each prefix. |
| Adj-RIB-Out | Routes selected for advertisement to each peer. |

This simulator should implement:

- Adj-RIB-In as `BgpPrefix rib_in[]`
- best-path selection over that table
- route-table installation for selected best paths

The Router forwarding path still uses `RouteTable` FIB lookup, not BGP's
internal RIB.

### Best-Path Selection

Use this simplified BGP decision process:

1. highest LOCAL_PREF
2. shortest AS_PATH
3. lowest MED, when comparing routes from the same neighboring AS
4. eBGP over iBGP, if iBGP is later added
5. lowest peer router ID as deterministic tie-break

This is not full RFC-grade BGP policy. It is enough to teach the important path
selection idea and produce deterministic tests.

### Incremental Updates

BGP does not periodically flood the entire table.

It sends UPDATE messages when routes change:

- announced prefixes add or replace routes
- withdrawn prefixes remove routes

KEEPALIVE messages maintain session liveness.

## Purpose

The BGP module implements simplified eBGP route exchange for Router objects.

It provides:

- BGP message header and body structures
- peer/session table
- Adj-RIB-In storage
- TCP listener/connect integration
- OPEN/KEEPALIVE/UPDATE/NOTIFICATION handling
- AS loop detection
- simplified best-path selection
- route-table install/delete for selected BGP routes
- keepalive and hold timers
- connect retry timer boundary

It does not:

- forward data packets
- own Router interfaces
- replace the route table
- implement full RFC 4271 policy
- implement route reflection
- implement confederations
- implement 4-byte ASNs in the first milestone
- parse arbitrary variable-length path attributes in the first milestone

## Architecture Boundary

| Responsibility | Owner |
| --- | --- |
| TCP byte-stream transport | TCP |
| BGP peer/session state | BGP |
| Adj-RIB-In | BGP |
| Best BGP path selection | BGP |
| Route candidate storage | RouteTable RIB |
| Active forwarding route selection | RouteTable FIB |
| Packet forwarding | Router |
| Keepalive/hold/connect timers | Scheduler |

BGP should update forwarding by calling Router/RouteTable public APIs. It must
not write directly into route-table arrays.

BGP should not inspect TCP send queues or TCB internals beyond the public TCB
pointer it receives in callbacks.

## Data Model

### Constants

```c
#define BGP_PORT                  179
#define BGP_VERSION               4
#define BGP_HDR_LEN               19
#define BGP_OPEN_LEN              10
#define BGP_MARKER_LEN            16
#define BGP_MIN_MSG_LEN           19
#define BGP_MAX_MSG_LEN           4096

#define BGP_HOLD_TIME_SEC         90
#define BGP_KEEPALIVE_INTERVAL_US 30000000ULL
#define BGP_HOLD_TIME_US          90000000ULL

#define BGP_MAX_PEERS             16
#define BGP_MAX_PREFIXES          1024
#define BGP_MAX_AS_PATH           8
#define BGP_LOCAL_PREF_DEFAULT    100
```

Use microseconds for internal timers because `Scheduler.now` and
`Event.timestamp` use microseconds.

### Message Types

```c
#define BGP_MSG_OPEN         1
#define BGP_MSG_UPDATE       2
#define BGP_MSG_NOTIFICATION 3
#define BGP_MSG_KEEPALIVE    4
```

### `BgpHeader`

```c
typedef struct __attribute__((packed)) BgpHeader {
    uint8_t  marker[16];
    uint16_t length;
    uint8_t  type;
} BgpHeader;
```

The marker must be all `0xFF`.

`length` includes the 19-byte header and is network byte order.

### `BgpOpen`

```c
typedef struct __attribute__((packed)) BgpOpen {
    uint8_t  version;
    uint16_t my_as;
    uint16_t hold_time;
    uint32_t bgp_id;
    uint8_t  opt_param_len;
} BgpOpen;
```

`hold_time` is seconds on the wire.

No optional parameters are required in the first milestone.

### Session State

```c
typedef enum BgpSessionState {
    BGP_IDLE,
    BGP_CONNECT,
    BGP_ACTIVE,
    BGP_OPEN_SENT,
    BGP_OPEN_CONFIRM,
    BGP_ESTABLISHED
} BgpSessionState;
```

### `BgpPeer`

```c
typedef struct BgpPeer {
    uint32_t        remote_ip;
    uint32_t        local_ip;
    uint16_t        remote_as;
    uint16_t        local_as;
    BgpSessionState state;
    Tcb            *tcb;
    uint32_t        bgp_id;
    uint16_t        hold_time;
    uint64_t        hold_deadline;
    uint64_t        keepalive_ts;
    int             valid;
} BgpPeer;
```

IP addresses are host order.

`tcb` is borrowed from TCP. BGP does not free it directly.

### `BgpPrefix`

```c
typedef struct BgpPrefix {
    uint32_t prefix;
    uint8_t  prefix_len;
    uint8_t  valid;
    uint16_t peer_index;
    uint32_t next_hop;
    uint32_t local_pref;
    uint32_t med;
    uint16_t as_path[BGP_MAX_AS_PATH];
    int      as_path_len;
} BgpPrefix;
```

This is an Adj-RIB-In route learned from one peer.

`prefix` and `next_hop` are host order.

### `BgpStateBlock`

```c
typedef struct BgpStateBlock {
    BgpPeer   peers[BGP_MAX_PEERS];
    int       peer_count;

    BgpPrefix rib_in[BGP_MAX_PREFIXES];
    int       rib_count;

    uint16_t  local_as;
    uint32_t  router_id;

    Simulator *sim;
    Router    *router;
    TcpTable  *tcp_table;
} BgpStateBlock;
```

`BgpStateBlock` is per router or per BGP speaker.

## Ownership And Lifetime

The owner allocates `BgpStateBlock`; BGP initializes it.

BGP borrows `Simulator *`, `Router *`, `TcpTable *`, and TCP `Tcb *` pointers.

BGP does not free Router, Simulator, TCP table, or TCBs.

`bgp_receive` receives ownership of TCP payload packets. It must free each
payload after parsing.

Scheduled BGP events borrow `BgpStateBlock *` as context. The owner must keep
the state alive while BGP events can fire.

## Public API

```c
void bgp_init(BgpStateBlock *state,
              Simulator *sim,
              Router *router,
              TcpTable *tcp_table,
              uint16_t local_as,
              uint32_t router_id);

int bgp_add_peer(BgpStateBlock *state,
                 uint32_t local_ip,
                 uint32_t remote_ip,
                 uint16_t remote_as);

void bgp_tcp_connected(Tcb *tcb, void *ctx);

void bgp_receive(Tcb *tcb, Packet *payload, void *ctx);

int bgp_send_open(BgpStateBlock *state, BgpPeer *peer);

int bgp_send_keepalive(BgpStateBlock *state, BgpPeer *peer);

int bgp_send_update(BgpStateBlock *state,
                    BgpPeer *peer,
                    const BgpPrefix *announced,
                    size_t announced_count,
                    const BgpPrefix *withdrawn,
                    size_t withdrawn_count);

int bgp_select_best(BgpStateBlock *state,
                    uint32_t prefix,
                    uint8_t prefix_len);

void bgp_keepalive_timer(const Event *e, void *ctx);

void bgp_hold_timer(const Event *e, void *ctx);

void bgp_connect_retry_timer(const Event *e, void *ctx);
```

`bgp_init` should call `tcp_listen` on port `179` when `tcp_table` exists and a
local listen address is known. If listen address is configured later, the owner
may call a separate listen helper in the implementation.

## Function Behavior

### `bgp_init`

Required behavior:

- If `state == NULL`, return immediately.
- Zero all BGP state.
- Store simulator, router, TCP table, local AS, and router ID.
- Initialize peer count and RIB count to zero.
- If TCP table and listening address are available, create a TCP listener on
  port `179` with:
  - receive handler `bgp_receive`
  - connect handler `bgp_tcp_connected`
  - context `state`

### `bgp_add_peer`

Required behavior:

- If `state == NULL`, return `-1`.
- Reject zero local IP or remote IP.
- Reject remote AS `0`.
- Reject duplicate peer remote IP.
- Reject full peer table.
- Allocate a peer slot.
- Store local IP, remote IP, local AS, remote AS.
- Set state to `BGP_CONNECT`.
- If `state->sim` and `state->tcp_table` are available, call `tcp_connect`:
  - local IP is configured local IP
  - remote IP is peer IP
  - local port is implementation-chosen nonzero port
  - remote port is `BGP_PORT`
  - receive handler is `bgp_receive`
  - connect handler is `bgp_tcp_connected`
  - context is `state`
- Store returned TCB on success.
- Return `0` on configured peer success, `-1` on failure.

### `bgp_tcp_connected`

Required behavior:

- If `tcb == NULL || ctx == NULL`, return.
- Find peer whose `tcb` equals the callback TCB.
- Move peer to `BGP_OPEN_SENT`.
- Send OPEN.
- Schedule hold timer.

This is the TCP-established hook. Do not overload `bgp_receive` with
`payload == NULL` for connection establishment.

### `bgp_receive`

Required behavior:

- If `payload == NULL`, return.
- If `ctx == NULL`, free payload and return.
- Cast context to `BgpStateBlock *`.
- Find peer by TCB.
- If no peer matches, free payload and return.
- A TCP payload may contain one BGP message or partial stream data depending on
  future TCP stream behavior. Current TCP delivers one packet payload at a time,
  so the first milestone may require one complete BGP message per payload.
- Validate common header:
  - marker bytes are all `0xFF`
  - length is between `BGP_MIN_MSG_LEN` and payload length
  - type is 1..4
- Reset hold deadline on any valid received BGP message.
- Dispatch:
  - OPEN: validate version/AS/hold time, move to `OPEN_CONFIRM`, send KEEPALIVE
  - KEEPALIVE: if in `OPEN_CONFIRM`, move to `ESTABLISHED`; otherwise refresh
    hold timer
  - UPDATE: require `ESTABLISHED`, parse withdrawals and announcements, update
    Adj-RIB-In, run best-path selection for changed prefixes
  - NOTIFICATION: close TCP if possible, mark peer `IDLE`
- Free payload before returning.

### `bgp_send_open`

Required behavior:

- If state or peer is NULL, return `-1`.
- If peer has no TCB, return `-1`.
- Build BGP OPEN message:
  - marker all `0xFF`
  - length `BGP_HDR_LEN + BGP_OPEN_LEN`
  - type `BGP_MSG_OPEN`
  - version `4`
  - local AS
  - hold time in seconds
  - router ID
  - optional parameter length `0`
- Send with `tcp_send`.
- Return TCP send result.

### `bgp_send_keepalive`

Required behavior:

- If state or peer is NULL, return `-1`.
- If peer has no TCB, return `-1`.
- Build BGP header-only KEEPALIVE message.
- Send with `tcp_send`.
- Update `peer->keepalive_ts`.
- Return TCP send result.

### `bgp_send_update`

Required behavior:

- If state or peer is NULL, return `-1`.
- If peer is not established, return `-1`.
- Encode withdrawn prefixes and announced prefixes.
- Include simplified attributes for announcements:
  - AS_PATH
  - NEXT_HOP
  - LOCAL_PREF for local selection
  - MED
- Send one or more UPDATE messages with `tcp_send`.
- Return `0` if all sends succeed, otherwise `-1`.

### `bgp_select_best`

Required behavior:

- If `state == NULL`, return `-1`.
- Consider valid `rib_in` entries matching prefix and prefix length.
- Ignore entries whose AS_PATH contains local AS.
- Select best path:
  1. highest LOCAL_PREF
  2. shortest AS_PATH
  3. lowest MED when same neighboring AS
  4. lowest peer router ID as tie-break
- If no candidate remains, delete BGP route for that prefix from route table.
- If best candidate exists, install it as `ROUTE_PROTO_BGP` with:
  - prefix and prefix length from candidate
  - next hop from candidate
  - interface resolved from peer/local routing context
  - metric based on AS_PATH length or a documented BGP metric mapping
- Return `0` on successful selection/update.

Interface selection is a design point. BGP NEXT_HOP identifies an IP next hop,
but `route_table_add` also needs an egress `Interface *`. The implementation
must derive that interface from the Router's existing route table or peer
configuration before installing the route.

### Timer Handlers

`bgp_keepalive_timer`:

- For each established peer whose keepalive is due, send KEEPALIVE.
- Reschedule next keepalive event.

`bgp_hold_timer`:

- For each valid peer, if current time exceeds hold deadline:
  - send NOTIFICATION when possible
  - close TCP when possible
  - invalidate or withdraw routes learned from that peer
  - move peer to `BGP_IDLE`

`bgp_connect_retry_timer`:

- For peers in `BGP_CONNECT` or `BGP_ACTIVE`, retry TCP connection.

## Flow Charts

### Session Establishment

```text
bgp_add_peer
  |
  +-- create peer in CONNECT
  +-- tcp_connect(... remote port 179 ...)
  |
  +-- TCP established callback
        |
        +-- bgp_tcp_connected
        +-- send OPEN
        +-- state = OPEN_SENT
```

### OPEN/KEEPALIVE

```text
bgp_receive OPEN
  |
  +-- validate OPEN
  +-- state = OPEN_CONFIRM
  +-- send KEEPALIVE

bgp_receive KEEPALIVE
  |
  +-- reset hold deadline
  +-- if OPEN_CONFIRM: state = ESTABLISHED
```

### UPDATE

```text
bgp_receive UPDATE
  |
  +-- parse withdrawn prefixes
  +-- parse announced prefixes and attributes
  +-- update Adj-RIB-In
  +-- for each changed prefix:
        bgp_select_best
          |
          +-- choose best BGP path
          +-- route_table_add/delete ROUTE_PROTO_BGP
```

## ACSL Contracts

The contracts belong in `bgp.h`. Use literal bounds:

- peers: `16`
- Adj-RIB-In prefixes: `1024`
- AS path length: `8`
- BGP marker bytes: `16`
- BGP header bytes: `19`

### Shared Predicates

```c
/*@
    predicate bgp_peer_count_valid(BgpStateBlock *state) =
        0 <= state->peer_count && state->peer_count <= 16;

    predicate bgp_rib_count_valid(BgpStateBlock *state) =
        0 <= state->rib_count && state->rib_count <= 1024;

    predicate bgp_prefix_slot_valid(BgpStateBlock *state, integer i) =
        0 <= i && i < 1024 ==>
            (state->rib_in[i].valid == 0 ||
             (state->rib_in[i].valid == 1 &&
              state->rib_in[i].prefix_len <= 32 &&
              0 <= state->rib_in[i].as_path_len &&
              state->rib_in[i].as_path_len <= 8));

    predicate bgp_state_well_formed(BgpStateBlock *state) =
        \valid(state) &&
        bgp_peer_count_valid(state) &&
        bgp_rib_count_valid(state) &&
        \forall integer i; 0 <= i && i < 1024 ==>
            bgp_prefix_slot_valid(state, i);
*/
```

### `bgp_init`

```c
/*@
    behavior null:
        assumes state == \null;
        assigns \nothing;

    behavior valid:
        assumes \valid(state);
        assigns state->peers[0 .. 15],
                state->peer_count,
                state->rib_in[0 .. 1023],
                state->rib_count,
                state->local_as,
                state->router_id,
                state->sim,
                state->router,
                state->tcp_table;
        ensures state->peer_count == 0;
        ensures state->rib_count == 0;
        ensures state->local_as == local_as;
        ensures state->router_id == router_id;
        ensures state->sim == sim;
        ensures state->router == router;
        ensures state->tcp_table == tcp_table;

    complete behaviors;
    disjoint behaviors;
*/
void bgp_init(BgpStateBlock *state,
              Simulator *sim,
              Router *router,
              TcpTable *tcp_table,
              uint16_t local_as,
              uint32_t router_id);
```

### `bgp_add_peer`

```c
/*@
    behavior bad_input:
        assumes state == \null ||
                local_ip == 0 ||
                remote_ip == 0 ||
                remote_as == 0;
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes bgp_state_well_formed(state);
        assumes local_ip != 0 && remote_ip != 0 && remote_as != 0;
        assigns state->peers[0 .. 15],
                state->peer_count;
        ensures \result == 0 || \result == -1;

    complete behaviors;
    disjoint behaviors;
*/
int bgp_add_peer(BgpStateBlock *state,
                 uint32_t local_ip,
                 uint32_t remote_ip,
                 uint16_t remote_as);
```

### `bgp_receive`

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
        assumes bgp_state_well_formed((BgpStateBlock *)ctx);
        assigns ((BgpStateBlock *)ctx)->peers[0 .. 15],
                ((BgpStateBlock *)ctx)->rib_in[0 .. 1023],
                ((BgpStateBlock *)ctx)->rib_count;
*/
void bgp_receive(Tcb *tcb, Packet *payload, void *ctx);
```

Additional required proof/test property:

- Every non-NULL payload is freed.
- Invalid marker rejects the message.
- Invalid length rejects the message.
- UPDATE is ignored unless peer is established.

### `bgp_select_best`

```c
/*@
    behavior bad_input:
        assumes state == \null || prefix_len > 32;
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes bgp_state_well_formed(state);
        assumes prefix_len <= 32;
        assigns state->router->route_tbl.rib[0 .. 255],
                state->router->route_tbl.rib_count,
                state->router->route_tbl.fib[0 .. 255],
                state->router->route_tbl.fib_count;
        ensures \result == 0 || \result == -1;

    complete behaviors;
    disjoint behaviors;
*/
int bgp_select_best(BgpStateBlock *state,
                    uint32_t prefix,
                    uint8_t prefix_len);
```

## KLEVA Verification Plan

Minimum KLEVA tests:

1. `bgp_init(NULL, ...)` does not crash.
2. Valid init clears peer and RIB counts.
3. Valid init stores local AS, router ID, simulator, router, and TCP table.
4. `bgp_add_peer` rejects NULL state and zero IP/AS values.
5. `bgp_add_peer` rejects duplicate remote IP.
6. `bgp_add_peer` rejects full peer table.
7. Valid peer add creates CONNECT peer.
8. TCP connected callback sends OPEN and moves to OPEN_SENT.
9. OPEN message has all-ones marker and correct length.
10. KEEPALIVE message is header-only.
11. Receive NULL payload does not crash.
12. Receive bad context frees payload.
13. Invalid marker is rejected.
14. Invalid length is rejected.
15. OPEN with wrong version sends notification or rejects.
16. Valid OPEN moves to OPEN_CONFIRM and sends KEEPALIVE.
17. KEEPALIVE in OPEN_CONFIRM moves to ESTABLISHED.
18. Any valid message resets hold deadline.
19. UPDATE before ESTABLISHED is ignored or rejected.
20. UPDATE with local AS in AS_PATH is dropped.
21. UPDATE announcement creates Adj-RIB-In entry.
22. UPDATE withdrawal invalidates Adj-RIB-In entry.
23. Best path prefers higher LOCAL_PREF.
24. Best path then prefers shorter AS_PATH.
25. Best path then prefers lower MED for same neighboring AS.
26. Best path tie-break is deterministic by peer router ID.
27. Best path install calls route-table path with `ROUTE_PROTO_BGP`.
28. No remaining candidate deletes BGP route.
29. Hold timer expiry sends NOTIFICATION when possible.
30. Hold timer expiry withdraws peer-learned routes.
31. Connect retry tries peers in CONNECT/ACTIVE.

## Common Mistakes

- Do not implement BGP over UDP.
- Do not bind BGP as an IP protocol handler.
- Do not treat `bgp_receive(tcb, NULL, ctx)` as TCP established; use the TCP
  connect callback.
- Do not let Router forwarding read BGP Adj-RIB-In.
- Do not write directly into route-table arrays.
- Do not install a route whose AS_PATH contains the local AS.
- Do not use milliseconds for scheduler timestamps.
- Do not forget to free TCP payload packets passed to BGP receive.
- Do not assume `route_table_add` can install BGP without a real egress
  interface.
