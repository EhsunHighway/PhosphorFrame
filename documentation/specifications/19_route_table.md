# Module 19 - Route Table

**Files:** `src/routing/route_table.c`, `src/routing/route_table.h`
**Status:** Ready for implementation; source files are currently empty
**Depends on:** `interface`

## Concepts First

This module is where the simulator starts to look like a real router instead
of just a packet sender. The route-table module answers this question:

```text
For this destination IP, which route should the router use?
```

That sounds simple, but real routers separate a few ideas that are easy to
mix together:

- **RIB**: Routing Information Base.
- **FIB**: Forwarding Information Base.
- **LPM**: Longest Prefix Match.

This simulator should teach those concepts instead of hiding them.

### What Is A Route?

A route is an instruction for reaching a group of IPv4 destinations.

```text
192.168.1.0/24 -> send through eth0
10.0.0.0/8     -> send through eth1 to next hop 10.0.0.2
0.0.0.0/0      -> default route
```

The left side is a **prefix**. The right side is forwarding information:

- egress interface
- optional next-hop IP
- metric/cost
- protocol that installed the route

### Prefix And Prefix Length

IPv4 addresses are 32-bit values. A prefix length says how many high-order bits
matter.

```text
/0   matches everything
/8   matches the first 8 bits
/24  matches the first 24 bits
/32  matches exactly one IPv4 address
```

For a route `192.168.1.0/24`, the first 24 bits identify the network. The last
8 bits are host bits.

A prefix match is checked by masking both values:

```text
match if (dst_ip & mask(prefix_len)) == (prefix & mask(prefix_len))
```

Important edge cases:

- `/0` uses mask `0x00000000`.
- `/32` uses mask `0xFFFFFFFF`.
- `prefix_len > 32` is invalid.

### Longest Prefix Match

More than one route can match the same destination. The most specific route
wins. This is **Longest Prefix Match**, or **LPM**.

```text
Routes:
  0.0.0.0/0
  10.0.0.0/8
  10.1.2.0/24
  10.1.2.99/32

Destination:
  10.1.2.99

Matches:
  /0, /8, /24, /32

Winner:
  /32
```

### RIB

The RIB is the control-plane view. It stores route candidates learned from
different sources:

- connected/direct routes
- static routes
- RIP routes
- OSPF routes
- BGP routes
- EIGRP routes
- IS-IS routes

The RIB may contain multiple candidates for the same prefix. For example:

```text
10.1.0.0/16 via eth1, proto OSPF, metric 20
10.1.0.0/16 via eth2, proto RIP,  metric 3
```

Both are information the router knows. They are not necessarily both active for
forwarding.

### FIB

The FIB is the data-plane view. It contains the selected active route for each
prefix that forwarding should consider.

The FIB is derived from the RIB. If the RIB has multiple candidates for the same
prefix, the FIB keeps the best candidate for that prefix.

Router forwarding uses the FIB because forwarding should not re-run every
control-plane decision for every packet.

### Combined Module In This Simulator

Real systems often separate RIB management from optimized FIB programming. This
module keeps both inside one `RouteTable` object:

```text
RouteTable
  |
  +-- RIB entries: route candidates from protocols/config
  |
  +-- FIB entries: selected active routes used by lookup
```

That gives us the important concepts without needing a separate route manager
process yet.

### Direct Route Versus Next-Hop Route

Direct route:

```text
192.168.1.0/24 -> next_hop 0, iface eth0
```

The destination is on the link attached to `eth0`. The forwarding code should
ARP for the final destination IP.

Next-hop route:

```text
172.16.0.0/16 -> next_hop 10.0.0.2, iface eth1
```

The destination is behind another router. The forwarding code should ARP for
`10.0.0.2`, not for the final destination.

Route table stores this information. It does not send ARP and does not transmit
packets.

### What Other Systems Do

This design is intentionally simplified, but it should not hide the real ideas:

- Linux uses an optimized FIB trie for IPv4 lookup.
- DPDK has LPM, RIB, and FIB libraries; its IPv4 LPM uses a fast table-based
  design with a special path for prefixes longer than 24 bits.
- FRRouting keeps routing-protocol routes in Zebra's RIB, computes best routes,
  and programs a FIB into the kernel or another forwarding plane.
- ns-3 models routing through modular IPv4 routing components rather than
  copying a kernel forwarding table implementation.

Our simulator will use a smaller RIB/FIB design with an LPM lookup path that is
clear enough to implement and verify.

## Purpose

The route table module stores route candidates, chooses active forwarding
routes, and performs LPM lookup.

It provides:

- fixed-capacity RIB storage
- fixed-capacity FIB storage derived from the RIB
- route add/update/delete
- protocol flush
- FIB rebuild
- LPM lookup with a special long-prefix pass

It does not:

- own interfaces
- own packets
- send ARP
- send Ethernet
- decrement TTL
- generate ICMP errors
- parse RIP/OSPF/BGP messages

## Architecture Boundary

| Responsibility | Owner |
| --- | --- |
| Store route candidates | RouteTable RIB |
| Select one active route per prefix | RouteTable FIB rebuild |
| Perform LPM forwarding lookup | RouteTable FIB lookup |
| Own route table storage | Router or future route manager |
| Own interfaces | Device/Router/Host, not RouteTable |
| Resolve next-hop IP to MAC | ARP cache / forwarding path |
| Forward packets | Router/IP forwarding path |
| Run protocol algorithms | RIP/OSPF/BGP/etc. modules |

Route table MUST NOT include ARP, Ethernet, IP, router, simulator, or packet
headers. It should only need integer types and `Interface`.

## Data Model

### Constants

```c
#define ROUTE_RIB_SIZE              256
#define ROUTE_FIB_SIZE              256
#define ROUTE_LONG_PREFIX_THRESHOLD 24

#define ROUTE_PROTO_STATIC          1
#define ROUTE_PROTO_DIRECT          2
#define ROUTE_PROTO_RIP             3
#define ROUTE_PROTO_OSPF            4
#define ROUTE_PROTO_BGP             5
#define ROUTE_PROTO_EIGRP           6
#define ROUTE_PROTO_ISIS            7
```

`0` is not a valid public route protocol.

`ROUTE_LONG_PREFIX_THRESHOLD` splits lookup into two conceptual groups:

- long prefixes: prefix length greater than `24`
- normal prefixes: prefix length `0..24`

This is not DPDK's DIR-24-8 table. It is a simpler special-case path that keeps
long/host routes visible as an important forwarding concept.

### Byte Order Rules

All IPv4 values in this module are host order.

| Value | Order |
| --- | --- |
| RIB prefix | host order |
| RIB next hop | host order |
| FIB prefix | host order |
| FIB next hop | host order |
| lookup destination IP | host order |

Route table does not read wire headers. If another module reads an IPv4 header,
that module must convert network-order fields to host order before calling this
module.

### Administrative Distance

When the RIB has multiple route candidates for the same normalized prefix and
prefix length, the FIB chooses the best one using:

1. lower administrative distance
2. lower metric
3. earlier insertion sequence

Use these administrative distances:

| Protocol | Distance |
| --- | ---: |
| direct | `0` |
| static | `1` |
| BGP | `20` |
| EIGRP | `90` |
| OSPF | `110` |
| IS-IS | `115` |
| RIP | `120` |

This is a simplified but recognizable route-selection model. The route table
does not implement full BGP, OSPF, EIGRP, or IS-IS decision logic; those
protocol modules still own their protocol-specific algorithms.

### `RouteRibEntry`

```c
typedef struct RouteRibEntry {
    uint32_t   prefix;      /* host-order normalized prefix */
    uint8_t    prefix_len;  /* CIDR length, 0..32 */
    uint8_t    proto;       /* ROUTE_PROTO_* */
    uint8_t    valid;       /* 1 means this RIB slot is used */
    uint8_t    selected;    /* 1 if this candidate is installed in FIB */
    uint32_t   next_hop;    /* host order; 0 means directly connected */
    Interface *iface;       /* borrowed egress interface */
    uint32_t   metric;      /* protocol-specific cost */
    uint32_t   sequence;    /* insertion order, lower is older */
} RouteRibEntry;
```

### `RouteFibEntry`

```c
typedef struct RouteFibEntry {
    uint32_t   prefix;      /* host-order normalized prefix */
    uint8_t    prefix_len;  /* CIDR length, 0..32 */
    uint8_t    proto;       /* selected route protocol */
    uint8_t    valid;       /* 1 means this FIB slot participates in lookup */
    uint8_t    _pad;
    uint32_t   next_hop;    /* host order; 0 means directly connected */
    Interface *iface;       /* borrowed egress interface */
    uint32_t   metric;      /* copied from selected RIB entry */
    int        rib_index;   /* selected RIB slot index */
} RouteFibEntry;
```

### `RouteTable`

```c
typedef struct RouteTable {
    RouteRibEntry rib[ROUTE_RIB_SIZE];
    int           rib_count;

    RouteFibEntry fib[ROUTE_FIB_SIZE];
    int           fib_count;

    uint32_t      next_sequence;
} RouteTable;
```

The RIB is the source of truth. The FIB can always be rebuilt from the RIB.

`rib_count` is the number of valid RIB entries.

`fib_count` is the number of valid FIB entries.

Deletion marks slots invalid and decrements the relevant count. It does not
compact either array. All scans must cover all 256 slots.

## Ownership And Lifetime

Route table borrows `Interface *iface`.

It never frees interfaces. If an interface is removed or freed, the owner must
delete or flush routes that point to that interface before freeing it.

Route table owns no packets.

Route table allocates no dynamic memory.

Route table storage is owned by the caller. A future Router module is expected
to embed or allocate a `RouteTable` and call `route_table_init` before use.

## Prefix Mask Helper

The implementation SHOULD use one internal helper for masks:

```c
static uint32_t route_prefix_mask(uint8_t prefix_len);
```

Required behavior:

- `prefix_len == 0` returns `0`.
- `prefix_len >= 32` returns `0xFFFFFFFFu`.
- otherwise returns `0xFFFFFFFFu << (32 - prefix_len)`.

Public APIs must reject `prefix_len > 32` before relying on the helper for a
valid route. The helper still handles `>= 32` defensively to avoid undefined
shifts.

## Prefix Normalization

`route_table_add` and `route_table_delete` MUST normalize prefixes:

```text
normalized_prefix = prefix & route_prefix_mask(prefix_len)
```

Example:

```text
route_table_add(table, 192.168.1.99, 24, ...)
stored prefix: 192.168.1.0
```

For `/0`, normalized prefix is `0`.

For `/32`, normalized prefix is the full input IP address.

## RIB Duplicate Key

An existing RIB candidate is the same route if all of these match:

- normalized prefix
- prefix length
- protocol

`next_hop`, `iface`, and `metric` are values to update. They are not part of
the duplicate key.

This means:

- a RIP update for an existing RIP prefix updates the RIP candidate
- a static route for that same prefix can coexist with the RIP candidate
- FIB rebuild decides which candidate becomes active for forwarding

## FIB Selection

FIB rebuild chooses one active route per normalized `(prefix, prefix_len)` pair.

For candidates with the same normalized prefix and prefix length, select:

1. lowest administrative distance
2. lowest metric
3. lowest sequence number

After rebuild:

- every valid FIB entry points back to one selected RIB entry by `rib_index`
- every selected RIB entry has `selected == 1`
- every non-selected valid RIB entry has `selected == 0`
- `fib_count` equals the number of unique normalized `(prefix, prefix_len)`
  pairs among valid RIB entries

## LPM Lookup Algorithm

`route_table_lookup` searches the FIB, not the RIB.

The lookup has two passes:

1. Long-prefix pass: scan valid FIB entries with `prefix_len > 24`.
2. Normal-prefix pass: scan valid FIB entries with `prefix_len <= 24`.

If the long-prefix pass finds any match, the best long-prefix match is the final
answer because every long prefix is more specific than every normal prefix.

If the long-prefix pass finds no match, the normal-prefix pass finds the best
normal match, including `/0`.

Within a pass, choose the matching FIB entry with the largest prefix length. If
two matching FIB entries have the same prefix length, keep the earlier slot.

This keeps the important idea from high-performance FIBs: longer or host-like
routes deserve a special path. It avoids a huge `2^24` table and keeps the
module small enough for ACSL/KLEVA.

## Public API

```c
void           route_table_init(RouteTable *table);

int            route_table_add(RouteTable *table,
                               uint32_t    prefix,
                               uint8_t     prefix_len,
                               uint32_t    next_hop,
                               Interface  *iface,
                               uint32_t    metric,
                               uint8_t     proto);

int            route_table_delete(RouteTable *table,
                                  uint32_t    prefix,
                                  uint8_t     prefix_len,
                                  uint8_t     proto);

RouteFibEntry *route_table_lookup(RouteTable *table,
                                  uint32_t    dst_ip);

int            route_table_flush_proto(RouteTable *table,
                                       uint8_t     proto);

int            route_table_rebuild_fib(RouteTable *table);
```

`route_table_add`, `route_table_delete`, and `route_table_flush_proto` MUST
rebuild the FIB before returning success.

`route_table_rebuild_fib` is public so tests can verify RIB-to-FIB behavior
directly.

## Function Behavior

### `route_table_init`

Required behavior:

- If `table` is `NULL`, return.
- Clear the whole table.
- Set `rib_count == 0`.
- Set `fib_count == 0`.
- Set `next_sequence == 1`.
- Mark every RIB entry invalid.
- Mark every FIB entry invalid.
- Clear every borrowed interface pointer.

### `route_table_add`

Required behavior:

- If `table` is `NULL`, return `-1`.
- If `iface` is `NULL`, return `-1`.
- If `prefix_len > 32`, return `-1`.
- If `proto == 0`, return `-1`.
- Normalize `prefix`.
- Search all 256 RIB slots for a valid entry with the same duplicate key.
- If found:
  - update `next_hop`
  - update `iface`
  - update `metric`
  - keep `sequence` unchanged
  - rebuild FIB
  - return `0` if rebuild succeeds
- If not found:
  - find an invalid RIB slot
  - if no invalid slot exists, return `-1`
  - write normalized prefix, prefix length, protocol, next hop, iface, metric
  - set `sequence` to `next_sequence`
  - increment `next_sequence`
  - set `valid == 1`
  - increment `rib_count`
  - rebuild FIB
  - return `0` if rebuild succeeds

If FIB rebuild fails after a new insert, the implementation MUST roll back the
new RIB entry before returning `-1`. With equal RIB and FIB capacities, rebuild
should not fail when the RIB is valid, but the failure rule must still be clear.

### `route_table_delete`

Required behavior:

- If `table` is `NULL`, return `-1`.
- If `prefix_len > 32`, return `-1`.
- If `proto == 0`, return `-1`.
- Normalize `prefix`.
- Search all 256 RIB slots for a valid entry with the same duplicate key.
- If no matching entry exists, return `-1`.
- If a matching entry exists:
  - mark it invalid
  - clear `selected`
  - clear `iface`
  - clear the remaining fields when practical
  - decrement `rib_count` if greater than zero
  - rebuild FIB
  - return `0`

Deletion does not compact the RIB.

### `route_table_flush_proto`

Required behavior:

- If `table` is `NULL`, return `0`.
- If `proto == 0`, return `0`.
- Scan all 256 RIB slots.
- Invalidate every valid RIB entry with the matching protocol.
- Clear each invalidated entry's `iface` pointer.
- Decrement `rib_count` once per invalidated entry.
- Rebuild FIB once after the scan.
- Return the number of RIB entries invalidated.

### `route_table_rebuild_fib`

Required behavior:

- If `table` is `NULL`, return `-1`.
- Clear all FIB entries.
- Set `fib_count == 0`.
- Set every valid RIB entry's `selected` field to `0`.
- For each valid RIB entry:
  - find whether a FIB entry already exists with the same normalized prefix and
    prefix length
  - if no FIB entry exists, create one from this RIB entry
  - if one exists, compare this RIB entry against the currently selected RIB
    entry using administrative distance, metric, and sequence
  - replace the FIB entry if the new candidate is better
- Mark selected RIB entries with `selected == 1`.
- Return `0`.

Because FIB capacity equals RIB capacity, rebuild should be able to represent
all unique prefixes from a valid RIB.

### `route_table_lookup`

Required behavior:

- If `table` is `NULL`, return `NULL`.
- Treat `dst_ip` as host order.
- Search valid FIB entries only.
- Ignore FIB entries with `prefix_len > 32`.
- First pass: consider only entries with `prefix_len > 24`.
- If first pass finds a match, return the best match from that pass.
- Second pass: consider only entries with `prefix_len <= 24`.
- Return the best match from the second pass or `NULL`.
- Lookup assigns nothing.

The returned pointer points into `table->fib[0 .. 255]`.

## Flow Charts

### Add Or Update

```text
route_table_add(...)
  |
  +-- reject invalid inputs
  |
  +-- normalize prefix
  |
  +-- existing RIB entry with same prefix, len, proto?
  |     |
  |     +-- yes:
  |          update next_hop, iface, metric
  |          rebuild FIB
  |          return 0
  |
  +-- find invalid RIB slot
        |
        +-- none: return -1
        |
        +-- found:
             insert RIB candidate
             rebuild FIB
             return 0
```

### Rebuild FIB

```text
route_table_rebuild_fib(table)
  |
  +-- clear FIB
  |
  +-- clear selected on all valid RIB entries
  |
  +-- for every valid RIB entry:
        |
        +-- no FIB entry for this prefix/len:
        |     create FIB entry from RIB entry
        |
        +-- FIB entry exists:
              compare current selected RIB vs this RIB
              lower distance wins
              then lower metric wins
              then older sequence wins
              update FIB if this RIB wins
  |
  +-- mark selected RIB entries
  |
  +-- return 0
```

### Lookup

```text
route_table_lookup(table, dst_ip)
  |
  +-- null table: return NULL
  |
  +-- scan FIB entries with prefix_len > 24
  |     remember best matching long prefix
  |
  +-- if long match exists: return it
  |
  +-- scan FIB entries with prefix_len <= 24
        remember best matching normal prefix
  |
  +-- return normal match or NULL
```

## ACSL Contracts

Use literal numeric bounds in ACSL comments. Do not rely on Frama-C/KLEVA
resolving route-table macro names inside annotations.

The contracts below are intended for `route_table.h`. They are deliberately
split into simple behaviors. The strongest LPM and best-route properties are
also listed after the blocks because they may need helper predicates or KLEVA
tests to keep EVA manageable.

### Shared Predicates

Put these before the public function declarations in `route_table.h`.

```c
/*@
    predicate route_rib_index(integer i) =
        0 <= i && i < 256;

    predicate route_fib_index(integer i) =
        0 <= i && i < 256;

    predicate route_valid_prefix_len(uint8_t prefix_len) =
        prefix_len <= 32;

    predicate route_valid_rib_count(RouteTable *table) =
        0 <= table->rib_count && table->rib_count <= 256;

    predicate route_valid_fib_count(RouteTable *table) =
        0 <= table->fib_count && table->fib_count <= 256;

    predicate route_valid_rib_slot(RouteTable *table, integer i) =
        route_rib_index(i) ==>
            ((table->rib[i].valid == 0 &&
              table->rib[i].selected == 0 &&
              table->rib[i].iface == \null) ||
             (table->rib[i].valid == 1 &&
              table->rib[i].prefix_len <= 32 &&
              table->rib[i].proto != 0 &&
              table->rib[i].iface != \null));

    predicate route_valid_fib_slot(RouteTable *table, integer i) =
        route_fib_index(i) ==>
            ((table->fib[i].valid == 0 &&
              table->fib[i].iface == \null) ||
             (table->fib[i].valid == 1 &&
              table->fib[i].prefix_len <= 32 &&
              table->fib[i].proto != 0 &&
              table->fib[i].iface != \null &&
              0 <= table->fib[i].rib_index &&
              table->fib[i].rib_index < 256));

    predicate route_table_well_formed(RouteTable *table) =
        \valid(table) &&
        route_valid_rib_count(table) &&
        route_valid_fib_count(table) &&
        (\forall integer i; 0 <= i < 256 ==> route_valid_rib_slot(table, i)) &&
        (\forall integer i; 0 <= i < 256 ==> route_valid_fib_slot(table, i));
*/
```

These predicates do not say the FIB contains the best routes. They only say the
memory shape and basic field ranges are valid. Best-route and LPM properties
belong to `route_table_rebuild_fib` and `route_table_lookup`.

### `route_table_init`

```c
/*@
    behavior null:
        assumes table == \null;
        assigns \nothing;

    behavior valid:
        assumes \valid(table);
        assigns table->rib[0 .. 255],
                table->rib_count,
                table->fib[0 .. 255],
                table->fib_count,
                table->next_sequence;
        ensures table->rib_count == 0;
        ensures table->fib_count == 0;
        ensures table->next_sequence == 1;
        ensures \forall integer i; 0 <= i < 256 ==>
                table->rib[i].valid == 0;
        ensures \forall integer i; 0 <= i < 256 ==>
                table->rib[i].selected == 0;
        ensures \forall integer i; 0 <= i < 256 ==>
                table->rib[i].iface == \null;
        ensures \forall integer i; 0 <= i < 256 ==>
                table->fib[i].valid == 0;
        ensures \forall integer i; 0 <= i < 256 ==>
                table->fib[i].iface == \null;
        ensures route_table_well_formed(table);

    complete behaviors;
    disjoint behaviors;
*/
void route_table_init(RouteTable *table);
```

### `route_table_add`

```c
/*@
    behavior bad_input:
        assumes table == \null || iface == \null ||
                prefix_len > 32 || proto == 0;
        assigns \nothing;
        ensures \result == -1;

    behavior valid_input:
        assumes route_table_well_formed(table) && \valid(iface);
        assumes prefix_len <= 32;
        assumes proto != 0;
        assigns table->rib[0 .. 255],
                table->rib_count,
                table->fib[0 .. 255],
                table->fib_count,
                table->next_sequence;
        ensures \result == 0 || \result == -1;
        ensures \result == -1 ==>
                table->rib_count == \old(table->rib_count);
        ensures \result == 0 ==>
                table->rib_count == \old(table->rib_count) ||
                table->rib_count == \old(table->rib_count) + 1;
        ensures \result == 0 ==>
                \exists integer i; 0 <= i < 256 &&
                    table->rib[i].valid == 1 &&
                    table->rib[i].prefix_len == prefix_len &&
                    table->rib[i].proto == proto &&
                    table->rib[i].next_hop == next_hop &&
                    table->rib[i].iface == iface &&
                    table->rib[i].metric == metric;
        ensures route_table_well_formed(table);

    complete behaviors;
    disjoint behaviors;
*/
int route_table_add(RouteTable *table,
                    uint32_t    prefix,
                    uint8_t     prefix_len,
                    uint32_t    next_hop,
                    Interface  *iface,
                    uint32_t    metric,
                    uint8_t     proto);
```

Additional required proof/test property:

- The stored RIB prefix for the inserted or updated entry is normalized:
  `stored_prefix == (prefix & route_prefix_mask(prefix_len))`.
- If an existing duplicate key is updated, its `sequence` is unchanged.
- If a new RIB entry is inserted, its `sequence` is the old `next_sequence`.
- On successful insert, `next_sequence` increments by one.

### `route_table_delete`

```c
/*@
    behavior bad_input:
        assumes table == \null || prefix_len > 32 || proto == 0;
        assigns \nothing;
        ensures \result == -1;

    behavior valid_input:
        assumes route_table_well_formed(table);
        assumes prefix_len <= 32;
        assumes proto != 0;
        assigns table->rib[0 .. 255],
                table->rib_count,
                table->fib[0 .. 255],
                table->fib_count;
        ensures \result == 0 || \result == -1;
        ensures \result == -1 ==>
                table->rib_count == \old(table->rib_count);
        ensures \result == 0 ==>
                table->rib_count == \old(table->rib_count) - 1;
        ensures route_table_well_formed(table);

    complete behaviors;
    disjoint behaviors;
*/
int route_table_delete(RouteTable *table,
                       uint32_t    prefix,
                       uint8_t     prefix_len,
                       uint8_t     proto);
```

Additional required proof/test property:

- On success, no valid RIB entry remains with the deleted normalized prefix,
  prefix length, and protocol.
- On success, FIB is rebuilt before the function returns.

### `route_table_flush_proto`

```c
/*@
    behavior null_or_zero:
        assumes table == \null || proto == 0;
        assigns \nothing;
        ensures \result == 0;

    behavior valid:
        assumes route_table_well_formed(table);
        assumes proto != 0;
        assigns table->rib[0 .. 255],
                table->rib_count,
                table->fib[0 .. 255],
                table->fib_count;
        ensures \result >= 0;
        ensures \result <= \old(table->rib_count);
        ensures table->rib_count == \old(table->rib_count) - \result;
        ensures \forall integer i; 0 <= i < 256 ==>
                table->rib[i].valid == 0 ||
                table->rib[i].proto != proto;
        ensures route_table_well_formed(table);

    complete behaviors;
    disjoint behaviors;
*/
int route_table_flush_proto(RouteTable *table, uint8_t proto);
```

Additional required proof/test property:

- Routes from protocols other than `proto` remain valid.
- FIB is rebuilt once after flushing.

### `route_table_rebuild_fib`

```c
/*@
    behavior null:
        assumes table == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes route_table_well_formed(table);
        assigns table->rib[0 .. 255],
                table->fib[0 .. 255],
                table->fib_count;
        ensures \result == 0;
        ensures table->fib_count >= 0;
        ensures table->fib_count <= table->rib_count;
        ensures \forall integer i; 0 <= i < 256 ==>
                table->fib[i].valid == 0 ||
                (table->fib[i].prefix_len <= 32 &&
                 0 <= table->fib[i].rib_index &&
                 table->fib[i].rib_index < 256 &&
                 table->rib[table->fib[i].rib_index].valid == 1 &&
                 table->rib[table->fib[i].rib_index].selected == 1);
        ensures route_table_well_formed(table);

    complete behaviors;
    disjoint behaviors;
*/
int route_table_rebuild_fib(RouteTable *table);
```

Additional required proof/test property:

- Every valid FIB entry corresponds to the best RIB entry for its normalized
  `(prefix, prefix_len)` pair.
- Every valid RIB entry is either selected or loses to a better RIB entry with
  the same normalized prefix and prefix length.
- `fib_count` equals the number of unique normalized `(prefix, prefix_len)`
  pairs among valid RIB entries.

### `route_table_lookup`

```c
/*@
    behavior null:
        assumes table == \null;
        assigns \nothing;
        ensures \result == \null;

    behavior valid:
        assumes route_table_well_formed(table);
        assigns \nothing;
        ensures \result == \null ||
                \exists integer i; 0 <= i < 256 &&
                    \result == &table->fib[i] &&
                    table->fib[i].valid == 1 &&
                    table->fib[i].prefix_len <= 32;

    complete behaviors;
    disjoint behaviors;
*/
RouteFibEntry *route_table_lookup(RouteTable *table, uint32_t dst_ip);
```

Additional required proof/test property:

- A non-null result matches `dst_ip`.
- No valid FIB entry with a longer prefix length also matches `dst_ip`.
- If any matching long-prefix FIB entry exists with `prefix_len > 24`, lookup
  returns a long-prefix result.

## KLEVA Verification Plan

Minimum KLEVA tests:

1. `route_table_init(NULL)` does not crash.
2. `route_table_init(table)` clears RIB and FIB counts.
3. `route_table_init(table)` invalidates all 256 RIB and FIB entries.
4. `route_table_add(NULL, ...)` returns `-1`.
5. `route_table_add(table, ..., NULL, ...)` returns `-1`.
6. `route_table_add` rejects `prefix_len == 33`.
7. `route_table_add` rejects `proto == 0`.
8. `route_table_add` inserts a valid RIB entry.
9. `route_table_add` normalizes host bits.
10. `route_table_add` updates an existing RIB key without incrementing
    `rib_count`.
11. Same prefix/length with different protocols creates separate RIB entries.
12. FIB selects direct over static for the same prefix.
13. FIB selects static over RIP for the same prefix.
14. FIB selects lower metric when administrative distance is equal.
15. FIB keeps earlier sequence when distance and metric are equal.
16. `route_table_delete` invalidates one matching RIB entry and rebuilds FIB.
17. After delete, lookup no longer returns a FIB entry derived from that route.
18. `route_table_flush_proto` removes RIP routes but keeps static/direct routes.
19. `route_table_lookup(NULL, dst_ip)` returns `NULL`.
20. Lookup returns `NULL` when FIB has no match.
21. Lookup matches `/0` default route.
22. Lookup matches `/32` exact host route.
23. Lookup chooses `/24` over `/8` and `/0`.
24. Lookup chooses `/32` from the long-prefix pass over `/24`.
25. Lookup ignores invalid FIB entries.
26. Rebuild produces one FIB entry for two RIB candidates with the same
    prefix/length.
27. Rebuild produces separate FIB entries for different prefix lengths.
28. Full RIB rejects a new non-duplicate route.

## Common Mistakes

- Do not add route-table storage to generic `Device`; Router owns it later.
- Do not free `Interface *iface` from RIB or FIB entries.
- Do not store network-order IPv4 values in RIB or FIB.
- Do not let lookup read the RIB directly.
- Do not let lookup stop at `fib_count`; deletion leaves holes.
- Do not use `0xFFFFFFFF << 32`; handle `/0` separately.
- Do not let metric beat administrative distance.
- Do not forget to rebuild FIB after add, delete, and flush.
- Do not let duplicate route spellings exist because host bits were not
  normalized.
