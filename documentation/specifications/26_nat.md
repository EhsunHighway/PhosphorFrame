# Module 26 - NAT

**Files:** `src/protocols/nat.c`, `src/protocols/nat.h`
**Status:** Ready for implementation; source files do not exist yet
**Depends on:** `router`, `route_table`, `ip`, `tcp`, `udp`, `icmp`, `packet`,
`interface`, `scheduler`, `event`, `simulator`, `byte_order`

## Concepts First

NAT means Network Address Translation.

This module implements PAT, or Port Address Translation. PAT is the common
"many private hosts share one public IP" form of NAT.

Outbound example:

```text
private host: 192.168.1.10:5000
public side:  203.0.113.1:1024

outbound packet source becomes 203.0.113.1:1024
```

Inbound reply:

```text
reply destination 203.0.113.1:1024
table lookup finds 192.168.1.10:5000
destination becomes 192.168.1.10:5000
```

### NAT Is A Forwarding Hook

NAT is not a standalone sender.

It must run inside the Router/IP forwarding path:

- outbound: after a route chooses the public egress interface, before Ethernet
  transmission
- inbound: after a packet arrives on the public interface, before routing it to
  the private destination

Current integration warning: the current Router spec defines forwarding, but no
implemented `ip_forward` hook exists yet. NAT cannot be complete until Router
forwarding exposes explicit pre-forward translation points.

### Five-Tuple Versus Translation Key

Full NATs often track a five-tuple:

```text
src_ip, src_port, dst_ip, dst_port, protocol
```

This first simulator NAT uses a smaller outbound key:

```text
private_ip, private_port, protocol
```

The reverse inbound key is:

```text
public_port, protocol
```

This is enough for basic PAT, but it is less strict than production NAT because
it does not distinguish remote destinations for the same private socket.

### Ports And Protocols

TCP and UDP use source/destination ports in their transport headers.

ICMP does not have ports. ICMP echo can be NATed by treating the ICMP `id` field
as a port-like identifier, but that requires ICMP-specific parsing.

First milestone:

- TCP PAT
- UDP PAT

Optional later milestone:

- ICMP echo NAT using ICMP id

### Checksums

NAT rewrites packet headers. Header checksums must be updated after rewriting.

Outbound TCP/UDP NAT changes:

- IPv4 source address
- TCP/UDP source port

Inbound TCP/UDP NAT changes:

- IPv4 destination address
- TCP/UDP destination port

Therefore NAT must update:

- IPv4 header checksum
- TCP checksum when TCP checksum support exists
- UDP checksum when UDP checksum is nonzero and checksum support exists

Current-stack warning: current TCP and UDP modules send checksum `0` and do not
validate transport checksums. NAT should still recompute the IPv4 header
checksum after IP address rewrite. Transport checksum fixup should be specified
and tested when TCP/UDP checksum support is added.

### Port Allocation

NAT assigns public ports from a configured dynamic range:

```text
1024..65535
```

If `next_port` reaches the end, it wraps back to the start and searches for a
free mapping. If no free mapping exists, translation fails and the packet is
dropped by the forwarding path.

## Purpose

The NAT module maintains translation state and rewrites packet headers for PAT.

It provides:

- NAT table initialization
- outbound TCP/UDP translation
- inbound TCP/UDP reverse translation
- dynamic public port allocation
- mapping lookup helpers
- idle timeout garbage collection
- NAT garbage-collection event handler
- checksum update helpers

It does not:

- own Router interfaces
- choose routes
- send ARP
- send Ethernet frames
- implement application-layer gateways
- implement full firewall policy
- complete Router forwarding integration by itself

## Architecture Boundary

| Responsibility | Owner |
| --- | --- |
| NAT mappings | NAT |
| Port allocation | NAT |
| Header rewrite | NAT |
| Routing decision | Router/RouteTable |
| ARP resolution | Router/IP/ARP path |
| Ethernet transmission | Ethernet module |
| TCP/UDP protocol semantics | TCP/UDP modules |
| NAT hook placement | Router forwarding path |
| Idle timer events | Scheduler |

NAT should operate on an already-visible IPv4 packet. It must not create a new
IP packet from an L4 payload.

## Data Model

### Constants

```c
#define NAT_TABLE_SIZE       512
#define NAT_PORT_START       1024
#define NAT_PORT_END         65535
#define NAT_TCP_TIMEOUT_US   86400000000ULL
#define NAT_UDP_TIMEOUT_US   300000000ULL
#define NAT_ICMP_TIMEOUT_US  60000000ULL
#define NAT_GC_INTERVAL_US   60000000ULL
```

Use microseconds because `Scheduler.now` and `Event.timestamp` use
microseconds.

### `NatEntry`

```c
typedef struct NatEntry {
    uint32_t private_ip;
    uint32_t public_ip;
    uint16_t private_port;
    uint16_t public_port;
    uint8_t  proto;
    uint8_t  valid;
    uint16_t _pad;
    uint64_t last_use;
    uint64_t timeout;
} NatEntry;
```

All IP addresses in the table are host order.

Ports are host order.

`proto` is the IPv4 protocol number: `IPPROTO_TCP` or `IPPROTO_UDP` in the
first milestone.

### `NatState`

```c
typedef struct NatState {
    NatEntry  table[NAT_TABLE_SIZE];
    int       count;

    uint32_t  public_ip;
    uint16_t  next_port;

    Interface *public_iface;
    Simulator *sim;
    Router    *router;
} NatState;
```

`public_iface` is borrowed.

`public_ip` is host order.

## Ownership And Lifetime

The owner allocates `NatState`; NAT initializes it.

NAT borrows `Simulator *`, `Router *`, and `Interface *`.

NAT does not free Router, Simulator, or interfaces.

`nat_outbound` and `nat_inbound` operate on a packet owned by the forwarding
path. They do not free packets on validation failure unless the forwarding
contract explicitly says NAT owns the packet at that hook.

Recommended forwarding contract:

- NAT returns `0` when packet was translated and forwarding should continue.
- NAT returns `-1` when packet should be dropped.
- The caller that owns the forwarding path frees the packet on `-1`.

This avoids hidden packet ownership transfer inside a header-rewrite helper.

## Public API

```c
void nat_init(NatState *state,
              Simulator *sim,
              Router *router,
              uint32_t public_ip,
              Interface *public_iface);

int nat_outbound(NatState *state, Packet *pkt);

int nat_inbound(NatState *state, Packet *pkt);

int nat_gc(NatState *state, uint64_t now);

NatEntry *nat_find_entry_outbound(NatState *state,
                                  uint32_t private_ip,
                                  uint16_t private_port,
                                  uint8_t proto);

NatEntry *nat_find_entry_inbound(NatState *state,
                                 uint16_t public_port,
                                 uint8_t proto);

uint16_t nat_checksum_update16(uint16_t old_checksum,
                               uint16_t old_value,
                               uint16_t new_value);

void nat_gc_handler(const Event *e, void *ctx);
```

## Function Behavior

### `nat_init`

Required behavior:

- If `state == NULL`, return immediately.
- Zero the NAT table.
- Set `count = 0`.
- Store `sim`, `router`, `public_ip`, and `public_iface`.
- Set `next_port = NAT_PORT_START`.
- If scheduler exists, schedule first NAT GC event at current time plus
  `NAT_GC_INTERVAL_US`.

### `nat_find_entry_outbound`

Required behavior:

- If `state == NULL`, return `NULL`.
- If `private_ip == 0 || private_port == 0`, return `NULL`.
- Scan all 512 entries.
- Return the valid entry whose private IP, private port, and protocol match.
- Return `NULL` if none exists.

### `nat_find_entry_inbound`

Required behavior:

- If `state == NULL`, return `NULL`.
- If `public_port == 0`, return `NULL`.
- Scan all 512 entries.
- Return the valid entry whose public port and protocol match.
- Return `NULL` if none exists.

### `nat_outbound`

Required behavior:

- If `state == NULL || pkt == NULL`, return `-1`.
- If `pkt->data == NULL || pkt->len < IP_HDR_LEN`, return `-1`.
- Parse IPv4 header at `pkt->data`.
- Support only `IPPROTO_TCP` and `IPPROTO_UDP` in the first milestone.
- Verify the visible packet contains the transport header.
- Read source IP and source port in host order.
- Look up existing outbound entry.
- If no entry exists:
  - allocate a free NAT entry
  - allocate a public port
  - fill private/public/protocol fields
  - set timeout based on protocol
  - increment count
- Rewrite IPv4 source address to `state->public_ip`.
- Rewrite TCP/UDP source port to entry public port.
- Refresh `last_use`.
- Recompute IPv4 header checksum.
- Update TCP/UDP checksum when transport checksum support is enabled.
- Return `0`.

If the NAT table is full or no public port is available, return `-1`.

### `nat_inbound`

Required behavior:

- If `state == NULL || pkt == NULL`, return `-1`.
- If `pkt->data == NULL || pkt->len < IP_HDR_LEN`, return `-1`.
- Parse IPv4 header at `pkt->data`.
- Support only `IPPROTO_TCP` and `IPPROTO_UDP` in the first milestone.
- Verify destination IP matches `state->public_ip`.
- Verify the visible packet contains the transport header.
- Read destination port in host order.
- Find inbound entry by public port and protocol.
- If no entry exists, return `-1`.
- Rewrite IPv4 destination address to entry private IP.
- Rewrite TCP/UDP destination port to entry private port.
- Refresh `last_use`.
- Recompute IPv4 header checksum.
- Update TCP/UDP checksum when transport checksum support is enabled.
- Return `0`.

### `nat_gc`

Required behavior:

- If `state == NULL`, return `0`.
- Scan all 512 entries.
- For each valid entry where `now - last_use >= timeout`:
  - invalidate the entry
  - clear fields when practical
  - decrement count if greater than zero
- Return the number of entries invalidated.

### `nat_checksum_update16`

Required behavior:

- Implement one's-complement incremental update for a 16-bit field:

```text
new_checksum = ~(~old_checksum + ~old_value + new_value)
```

- Fold carries until the result fits in 16 bits.
- Return the updated checksum.

This helper is useful for port updates. IPv4 address updates are two 16-bit
updates or a full recompute.

### `nat_gc_handler`

Required behavior:

- If event or context is NULL, return.
- Cast context to `NatState *`.
- Get current simulator/scheduler time when available.
- Call `nat_gc`.
- Schedule the next GC event.

## Flow Charts

### Outbound

```text
nat_outbound(state, pkt)
  |
  +-- validate IPv4 + TCP/UDP headers
  +-- key = private src_ip, src_port, proto
  |
  +-- existing mapping?
  |     yes: use public_port
  |     no: allocate entry and public_port
  |
  +-- rewrite src_ip to public_ip
  +-- rewrite src_port to public_port
  +-- refresh last_use
  +-- recompute checksums
  +-- return 0
```

### Inbound

```text
nat_inbound(state, pkt)
  |
  +-- validate IPv4 + TCP/UDP headers
  +-- require dst_ip == public_ip
  +-- key = dst_port, proto
  |
  +-- mapping found?
        |
        +-- no: return -1
        |
        +-- yes:
              rewrite dst_ip to private_ip
              rewrite dst_port to private_port
              refresh last_use
              recompute checksums
              return 0
```

## ACSL Contracts

The contracts belong in `nat.h`. Use literal bounds:

- NAT table entries: `512`
- IPv4 header bytes: `20`
- TCP minimum header bytes: `20`
- UDP header bytes: `8`

### Shared Predicates

```c
/*@
    predicate nat_count_valid(NatState *state) =
        0 <= state->count && state->count <= 512;

    predicate nat_port_valid(uint16_t port) =
        1024 <= port && port <= 65535;

    predicate nat_entry_valid(NatState *state, integer i) =
        0 <= i && i < 512 ==>
            (state->table[i].valid == 0 ||
             (state->table[i].valid == 1 &&
              state->table[i].private_ip != 0 &&
              state->table[i].public_ip != 0 &&
              state->table[i].private_port != 0 &&
              nat_port_valid(state->table[i].public_port)));

    predicate nat_state_well_formed(NatState *state) =
        \valid(state) &&
        nat_count_valid(state) &&
        \forall integer i; 0 <= i && i < 512 ==>
            nat_entry_valid(state, i);
*/
```

### `nat_init`

```c
/*@
    behavior null:
        assumes state == \null;
        assigns \nothing;

    behavior valid:
        assumes \valid(state);
        assigns state->table[0 .. 511],
                state->count,
                state->public_ip,
                state->next_port,
                state->public_iface,
                state->sim,
                state->router;
        ensures state->count == 0;
        ensures state->public_ip == public_ip;
        ensures state->next_port == 1024;
        ensures state->public_iface == public_iface;
        ensures state->sim == sim;
        ensures state->router == router;
        ensures \forall integer i; 0 <= i && i < 512 ==>
                state->table[i].valid == 0;

    complete behaviors;
    disjoint behaviors;
*/
void nat_init(NatState *state,
              Simulator *sim,
              Router *router,
              uint32_t public_ip,
              Interface *public_iface);
```

### `nat_outbound`

```c
/*@
    behavior bad_input:
        assumes state == \null || pkt == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes nat_state_well_formed(state);
        assumes pkt != \null;
        assigns state->table[0 .. 511],
                state->count,
                state->next_port,
                pkt->data[0 .. pkt->len - 1];
        ensures \result == 0 || \result == -1;

    complete behaviors;
    disjoint behaviors;
*/
int nat_outbound(NatState *state, Packet *pkt);
```

### `nat_inbound`

```c
/*@
    behavior bad_input:
        assumes state == \null || pkt == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes nat_state_well_formed(state);
        assumes pkt != \null;
        assigns state->table[0 .. 511],
                pkt->data[0 .. pkt->len - 1];
        ensures \result == 0 || \result == -1;

    complete behaviors;
    disjoint behaviors;
*/
int nat_inbound(NatState *state, Packet *pkt);
```

### `nat_gc`

```c
/*@
    behavior null:
        assumes state == \null;
        assigns \nothing;
        ensures \result == 0;

    behavior valid:
        assumes nat_state_well_formed(state);
        assigns state->table[0 .. 511],
                state->count;
        ensures \result >= 0;
        ensures \result <= \old(state->count);
        ensures state->count == \old(state->count) - \result;

    complete behaviors;
    disjoint behaviors;
*/
int nat_gc(NatState *state, uint64_t now);
```

## KLEVA Verification Plan

Minimum KLEVA tests:

1. `nat_init(NULL, ...)` does not crash.
2. Valid init clears count and every valid bit.
3. Valid init stores public IP and public interface.
4. Valid init sets next port to `1024`.
5. Outbound rejects NULL state and NULL packet.
6. Outbound rejects too-short IP packet.
7. Outbound rejects unsupported protocol.
8. Outbound creates new TCP mapping.
9. Outbound creates new UDP mapping.
10. Outbound reuses existing mapping.
11. Outbound rewrites source IP and source port.
12. Outbound refreshes `last_use`.
13. Outbound recomputes IPv4 header checksum.
14. Port allocation wraps and skips used ports.
15. Port exhaustion returns `-1`.
16. Inbound rejects NULL state and NULL packet.
17. Inbound rejects packet not addressed to NAT public IP.
18. Inbound miss returns `-1`.
19. Inbound hit rewrites destination IP and destination port.
20. Inbound refreshes `last_use`.
21. Inbound recomputes IPv4 header checksum.
22. GC removes timed-out TCP entry.
23. GC removes timed-out UDP entry.
24. GC keeps active entry.
25. Checksum update helper matches full recomputation for one 16-bit change.
26. NAT hook integration test proves Router frees packet on NAT failure.

## Common Mistakes

- Do not make NAT choose routes.
- Do not make NAT send Ethernet frames.
- Do not create a new IP packet from an L4 payload; NAT rewrites an existing IP
  packet.
- Do not forget host-order versus network-order conversions.
- Do not use milliseconds for scheduler timestamps.
- Do not claim TCP/UDP checksum fixup is fully verified while current TCP/UDP
  checksums are disabled.
- Do not leak packets on NAT failure in the forwarding path.
- Do not implement ICMP NAT by pretending ICMP has ports.
