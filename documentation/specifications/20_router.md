# Module 20 - Router

**Files:** `src/network/router.c`, `src/network/router.h`
**Status:** Ready for implementation; current source files are stubs
**Depends on:** `device`, `interface`, `packet`, `ethernet`, `arp_cache`,
`arp`, `ip`, `icmp`, `route_table`, `simulator`

## Concepts First

A router is a layer-3 forwarding device.

A host usually sends packets for itself. A switch forwards Ethernet frames by
MAC address. A router forwards IPv4 packets by destination IP address.

Router receive flow:

```text
Ethernet frame arrives on an interface
  |
  +-- validate IPv4 packet
  +-- decrement TTL
  +-- route lookup by destination IP
  +-- choose next-hop IP
  +-- resolve next-hop MAC with ARP
  +-- send out the selected interface
```

### Router Versus Host

Host and Router both embed a `Device`, but they are not the same module.

Host:

- owns IP/UDP/TCP endpoint state
- receives packets for itself
- does not forward packets between interfaces
- stores only a default gateway value for future use

Router:

- owns a route table
- receives packets that may be destined somewhere else
- forwards packets between interfaces
- uses longest prefix match to choose an egress interface and next hop

Router should not own UDP or TCP state just because hosts do.

### Router Versus Switch

Switch:

- forwards Ethernet frames by destination MAC address
- learns source MAC addresses into a MAC table
- does not inspect IPv4 destination addresses for forwarding

Router:

- forwards IPv4 packets by destination IP address
- does not learn MAC addresses in a `MacTable`
- uses ARP only to resolve the next-hop IP to a destination MAC

Router must not include or use `mac_table`.

### Route Table And LPM

Router owns a `RouteTable`.

The route table performs longest prefix match, or LPM. LPM means that if
multiple routes match a destination, the route with the longest prefix length
wins.

Example:

```text
0.0.0.0/0        default route
10.0.0.0/8       broad route
10.1.2.0/24      more specific route
10.1.2.99/32     exact host route
```

For destination `10.1.2.99`, the `/32` route wins.

Router uses the FIB lookup result from `route_table_lookup`. Router does not
scan the RIB directly.

### Direct Route Versus Next-Hop Route

A route-table result has:

- egress interface
- next-hop IP
- destination prefix

If `next_hop == 0`, the destination is directly connected. Router should ARP
for the final destination IP.

If `next_hop != 0`, the destination is behind another router. Router should ARP
for `next_hop`, not for the final destination IP.

```text
192.168.1.0/24 via next_hop 0 on eth0
  -> ARP for final dst_ip

172.16.0.0/16 via next_hop 10.0.0.2 on eth1
  -> ARP for 10.0.0.2
```

### TTL

TTL is the IPv4 hop limit.

Every router hop must decrement TTL before forwarding. If the packet arrives
with `ttl <= 1`, forwarding would make TTL zero, so the router must drop the
packet and send ICMP Time Exceeded when that helper exists.

The current ICMP module does not expose a public TTL-exceeded helper. The
Router implementation must still structure the TTL path clearly:

- before a public helper exists, drop and count the packet
- after the helper exists, send ICMP Time Exceeded from this branch

Do not silently forward a packet with expired TTL.

### Forwarding And Checksums

When Router decrements TTL, the IPv4 header checksum becomes stale.

Router must update the IPv4 header checksum before sending the forwarded packet.
The simplest implementation may set `header_checksum = 0` and call
`ip_checksum` again.

Router forwards an already-formed IPv4 packet. It must not call `ip_send`,
because `ip_send` creates a new IPv4 header and sets a fresh default TTL.

Router should send the existing packet with `ethernet_send` after adjusting the
IPv4 header.

## Purpose

The Router module creates a forwarding node.

It provides:

- Router allocation and release
- embedded Device initialization
- embedded ARP cache initialization
- embedded RouteTable initialization
- interface addition with shared router ARP cache
- route add/delete wrappers
- IPv4 forwarding receive path
- ARP miss handling with pending packet queue

It does not:

- implement route-table algorithms
- implement routing protocols
- own interfaces before successful add
- own the simulator
- implement UDP/TCP endpoint behavior
- learn MAC addresses
- behave like a switch

## Architecture Boundary

| Responsibility | Owner |
| --- | --- |
| Interface array | embedded `Device` |
| Interface objects after successful add | Router through `Device` |
| ARP cache storage | embedded `ArpCache` in Router |
| Route storage and LPM | embedded `RouteTable` in Router |
| Next-hop route selection | `route_table_lookup` |
| TTL decrement and checksum update | Router |
| ARP request and pending queue | ARP / ARP cache modules |
| Ethernet transmission | Ethernet module |
| Routing protocol decisions | RIP/OSPF/BGP/EIGRP/IS-IS modules |

Router should include `route_table.h`, not re-declare route structures.

Router should include `arp_cache.h` and `arp.h`, not manually inspect pending
queue layout except through public helpers.

## Data Model

### Constants

```c
#define ROUTER_MAX_PORTS 8
```

`ROUTER_MAX_PORTS` is the capacity of the embedded Device interface array.

### `Router`

```c
typedef struct Router {
    Device     base;
    ArpCache   arp_cache;
    RouteTable route_tbl;
    Simulator *sim;
} Router;
```

`base` must be the first field. That makes `&router->base` the embedded Device
used by shared Device helpers.

`arp_cache` and `route_tbl` are embedded. They are not pointers and must not be
freed separately.

`sim` is borrowed. Router does not free it.

### Header Includes

`router.h` should include only what the public struct and prototypes need:

```c
#include <stdint.h>
#include "device.h"
#include "interface.h"
#include "packet.h"
#include "../engine/simulator.h"
#include "../protocols/arp_cache.h"
#include "../routing/route_table.h"
```

`router.h` must not include `mac_table.h`.

Implementation-only includes such as `arp.h`, `ip.h`, `icmp.h`, and
`ethernet.h` belong in `router.c` when needed.

### Byte Order Rules

Router receives wire IPv4 headers in network byte order.

Before calling route-table or ARP-cache APIs that expect host-order IP values,
Router must convert:

| Value | Required order at call site |
| --- | --- |
| route add prefix | host order |
| route add next hop | host order |
| route lookup destination | host order |
| ARP cache lookup target | host order |
| ARP pending enqueue target | host order |
| forwarded IPv4 header fields | network order |

Interface `ip_addr` is network order in the current stack.

## Ownership And Lifetime

`router_create` owns the Router allocation and `base.interfaces` allocation.

`router_create` borrows `Simulator *sim`.

`router_add_interface` transfers ownership of an interface only on success.

On successful `router_add_interface`, `router_free` frees that interface.

On failure, the caller still owns the interface.

Router borrows route entry interfaces. It must not free an interface because a
route points at it.

Router receive consumes every non-NULL packet passed to it:

- malformed packet paths free the packet
- TTL expiration drops/frees the packet unless a future ICMP helper consumes it
- no route drops/frees the packet unless a future ICMP helper consumes it
- ARP miss queues the packet in the ARP pending queue on success
- ARP hit transfers the packet to Ethernet send path on success

If ARP request succeeds but pending enqueue fails, Router must free or otherwise
account for the packet to avoid a leak.

## Public API

```c
Router *router_create(const char *name, Simulator *sim);

void router_free(Router *router);

int router_add_interface(Router *router, Interface *iface);

int router_receive(Router    *router,
                   Interface *in_iface,
                   Packet    *pkt);

int router_add_route(Router    *router,
                     uint32_t   prefix,
                     uint8_t    prefix_len,
                     uint32_t   next_hop,
                     Interface *iface,
                     uint32_t   metric,
                     uint8_t    proto);

int router_del_route(Router  *router,
                     uint32_t prefix,
                     uint8_t  prefix_len,
                     uint8_t  proto);
```

`router_add_route` and `router_del_route` are thin wrappers around the route
table public API. Routing protocol modules should call these wrappers instead
of reaching into `router->route_tbl` directly.

## Function Behavior

### `router_create`

Required behavior:

- If `name == NULL || sim == NULL`, return `NULL`.
- Allocate and zero one Router.
- Copy `name` into `router->base.name` with termination.
- Allocate `router->base.interfaces` with capacity `ROUTER_MAX_PORTS`.
- Set `router->base.iface_count = 0`.
- Set `router->base.iface_max = ROUTER_MAX_PORTS`.
- Call `arp_cache_init(&router->arp_cache)`.
- Call `route_table_init(&router->route_tbl)`.
- Store borrowed simulator pointer in `router->sim`.
- Return the Router.

On allocation failure, release any owned allocation already made and return
`NULL`.

### `router_free`

Required behavior:

- If `router == NULL`, return.
- Free each interface stored in `router->base.interfaces[0 .. iface_count - 1]`
  with `interface_free`.
- Free `router->base.interfaces`.
- Free the Router.

Do not free `router->arp_cache` or `router->route_tbl` separately because they
are embedded.

### `router_add_interface`

Required behavior:

- If `router == NULL || iface == NULL`, return `-1`.
- If the embedded Device interface array is full, return `-1`.
- Call `device_add_interface(&router->base, iface)`.
- If Device rejects the interface, return `-1`.
- Call `interface_set_arp_cache(iface, &router->arp_cache)`.
- Bind the interface receive path so incoming IPv4 frames can reach this
  Router's receive function with this Router as context.
- Return `0`.

The exact receive-binding helper may be an interface setter or direct assignment
depending on the current interface API. The important postcondition is that
the interface can deliver inbound packets to `router_receive`.

### `router_add_route`

Required behavior:

- If `router == NULL`, return `-1`.
- Delegate to:

```c
route_table_add(&router->route_tbl,
                prefix,
                prefix_len,
                next_hop,
                iface,
                metric,
                proto);
```

- Return that result.

All IP values passed to route table are host order.

### `router_del_route`

Required behavior:

- If `router == NULL`, return `-1`.
- Delegate to:

```c
route_table_delete(&router->route_tbl, prefix, prefix_len, proto);
```

- Return that result.

### `router_receive`

Required behavior:

- If `router == NULL || in_iface == NULL || pkt == NULL`, return `-1`.
- If `router->sim == NULL`, free `pkt` and return `-1`.
- If `pkt->data == NULL || pkt->len < IP_HDR_LEN`, free `pkt`, increment
  `in_iface->rx_errors`, and return `-1`.
- Read the IPv4 header at `pkt->data`.
- If the packet is not IPv4, free `pkt`, increment `rx_errors`, return `-1`.
- Validate the IPv4 header checksum before changing TTL.
- If checksum is invalid, free `pkt`, increment `rx_errors`, return `-1`.
- If `ttl <= 1`, drop the packet:
  - current implementation target: free `pkt`, increment `rx_dropped`, return
    `-1`
  - future improvement: send ICMP Time Exceeded from this branch
- Convert destination IP from network order to host order.
- Look up the destination in `router->route_tbl`.
- If lookup misses, drop the packet:
  - current implementation target: free `pkt`, increment `rx_dropped`, return
    `-1`
  - future improvement: send ICMP Net/Host Unreachable from this branch
- Determine ARP target:
  - if route `next_hop != 0`, target is `next_hop`
  - otherwise target is final destination IP
- Decrement TTL by one.
- Recompute IPv4 header checksum.
- Look up target IP in `router->arp_cache`.
- If ARP cache hit:
  - transmit the existing IPv4 packet through the route's egress interface with
    `ethernet_send`
  - return Ethernet send result
- If ARP cache miss:
  - send ARP request for the target IP on the route's egress interface
  - enqueue the packet in `router->arp_cache` pending queue using target,
    original source IP, original destination IP, protocol, egress interface,
    and packet
  - if enqueue succeeds, return `0`
  - if enqueue fails, free packet and return `-1`

Router forwards the same IPv4 packet. It does not strip the IP header and does
not prepend a new IP header.

Current ARP helper note: `arp_send_request` currently expects target IP in
network order in existing call sites, while `arp_pending_enqueue` stores target
IP in host order. Router must follow each API's contract exactly at the call
site.

## Flow Charts

### Creation

```text
router_create(name, sim)
  |
  +-- reject NULL name/sim
  +-- allocate and zero Router
  +-- allocate Device interface array
  +-- arp_cache_init(&router->arp_cache)
  +-- route_table_init(&router->route_tbl)
  +-- router->sim = sim
  +-- return Router
```

### Interface Add

```text
router_add_interface(router, iface)
  |
  +-- reject NULL/full inputs
  +-- device_add_interface(&router->base, iface)
  +-- interface_set_arp_cache(iface, &router->arp_cache)
  +-- bind receive path to router_receive/router
  +-- return 0
```

### Forwarding

```text
router_receive(router, in_iface, pkt)
  |
  +-- validate IPv4 packet
  +-- ttl <= 1?
  |     drop, rx_dropped++, return -1
  |
  +-- route_table_lookup(dst_ip)
  |     miss: drop, rx_dropped++, return -1
  |
  +-- target_ip = route.next_hop != 0 ? route.next_hop : dst_ip
  +-- ttl--
  +-- recompute checksum
  |
  +-- arp_cache_lookup(target_ip)
        |
        +-- hit:
        |     ethernet_send(egress_iface, dst_mac, IPv4 packet)
        |
        +-- miss:
              arp_send_request
              arp_pending_enqueue
              return 0
```

## ACSL Contracts

The contracts belong in `router.h`. Use literal bounds:

- Router interface capacity: `8`
- ARP cache entries: `256`
- ARP pending entries: `32`
- route table RIB/FIB entries: `256`
- IPv4 header length: `20`

### `router_create`

```c
/*@
    behavior bad_input:
        assumes name == \null || sim == \null;
        assigns \nothing;
        ensures \result == \null;

    behavior valid:
        assumes name != \null && sim != \null;
        allocates \result;
        ensures \result == \null || \valid(\result);
        ensures \result != \null ==> \result->sim == sim;
        ensures \result != \null ==> \result->base.iface_count == 0;
        ensures \result != \null ==> \result->base.iface_max == 8;
        ensures \result != \null ==> \result->base.interfaces != \null;
        ensures \result != \null ==> \result->arp_cache.count == 0;
        ensures \result != \null ==> \result->arp_cache.pending_count == 0;
        ensures \result != \null ==> \result->route_tbl.rib_count == 0;
        ensures \result != \null ==> \result->route_tbl.fib_count == 0;

    complete behaviors;
    disjoint behaviors;
*/
Router *router_create(const char *name, Simulator *sim);
```

Additional required proof/test property:

- Every ARP cache entry valid bit is cleared.
- Every ARP pending entry valid bit is cleared.
- Every route table RIB and FIB entry valid bit is cleared.

### `router_free`

```c
/*@
    behavior null:
        assumes router == \null;
        assigns \nothing;

    behavior valid:
        assumes \valid(router);
        frees router->base.interfaces[0 .. router->base.iface_count - 1],
              router->base.interfaces,
              router;

    complete behaviors;
    disjoint behaviors;
*/
void router_free(Router *router);
```

### `router_add_interface`

```c
/*@
    behavior bad_input:
        assumes router == \null || iface == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior full:
        assumes \valid(router) && \valid(iface);
        assumes router->base.iface_count >= router->base.iface_max;
        assigns \nothing;
        ensures \result == -1;

    behavior added:
        assumes \valid(router) && \valid(iface);
        assumes router->base.iface_count < router->base.iface_max;
        assigns router->base.interfaces[0 .. router->base.iface_count],
                router->base.iface_count,
                iface->device,
                iface->arp_cache,
                iface->rx_handler,
                iface->handler_ctx;
        ensures \result == 0 || \result == -1;
        ensures \result == 0 ==>
                router->base.iface_count == \old(router->base.iface_count) + 1;
        ensures \result == 0 ==>
                router->base.interfaces[\old(router->base.iface_count)] == iface;
        ensures \result == 0 ==> iface->device == &router->base;
        ensures \result == 0 ==> iface->arp_cache == &router->arp_cache;

    complete behaviors;
    disjoint behaviors;
*/
int router_add_interface(Router *router, Interface *iface);
```

### `router_add_route`

```c
/*@
    behavior bad_input:
        assumes router == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes \valid(router);
        assigns router->route_tbl.rib[0 .. 255],
                router->route_tbl.rib_count,
                router->route_tbl.fib[0 .. 255],
                router->route_tbl.fib_count,
                router->route_tbl.next_sequence;
        ensures \result == 0 || \result == -1;

    complete behaviors;
    disjoint behaviors;
*/
int router_add_route(Router *router,
                     uint32_t prefix,
                     uint8_t prefix_len,
                     uint32_t next_hop,
                     Interface *iface,
                     uint32_t metric,
                     uint8_t proto);
```

### `router_del_route`

```c
/*@
    behavior bad_input:
        assumes router == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes \valid(router);
        assigns router->route_tbl.rib[0 .. 255],
                router->route_tbl.rib_count,
                router->route_tbl.fib[0 .. 255],
                router->route_tbl.fib_count;
        ensures \result == 0 || \result == -1;

    complete behaviors;
    disjoint behaviors;
*/
int router_del_route(Router *router,
                     uint32_t prefix,
                     uint8_t prefix_len,
                     uint8_t proto);
```

### `router_receive`

```c
/*@
    behavior bad_input:
        assumes router == \null || in_iface == \null || pkt == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior malformed:
        assumes \valid(router) && \valid(in_iface) && \valid(pkt);
        assumes router->sim == \null ||
                pkt->data == \null ||
                pkt->len < 20;
        assigns in_iface->rx_errors;
        ensures \result == -1;

    behavior readable_ipv4:
        assumes \valid(router) && \valid(in_iface) && \valid(pkt);
        assumes router->sim != \null;
        assumes pkt->data != \null;
        assumes pkt->len >= 20;
        assumes \valid_read(pkt->data + (0 .. pkt->len - 1));
        assigns in_iface->rx_errors,
                in_iface->rx_dropped,
                in_iface->tx_bytes,
                in_iface->last_tx_time,
                pkt->data[0 .. pkt->len - 1],
                router->arp_cache.pending[0 .. 31],
                router->arp_cache.pending_count;
        ensures \result == 0 || \result == -1;

    complete behaviors;
    disjoint behaviors;
*/
int router_receive(Router *router, Interface *in_iface, Packet *pkt);
```

Additional required proof/test property:

- TTL-expired packets are not forwarded.
- No-route packets are not forwarded.
- ARP-hit forwarding decrements TTL and recomputes checksum.
- ARP-miss forwarding enqueues the packet with the correct target IP.

## KLEVA Verification Plan

Minimum KLEVA tests:

1. `router_create(NULL, sim)` returns NULL.
2. `router_create("r1", NULL)` returns NULL.
3. Valid create initializes Device count and capacity.
4. Valid create initializes embedded ARP cache counts and valid bits.
5. Valid create initializes embedded route table counts and valid bits.
6. `router_free(NULL)` does not crash.
7. `router_add_interface(NULL, iface)` returns `-1`.
8. `router_add_interface(router, NULL)` returns `-1`.
9. Full router returns `-1` and does not increment interface count.
10. Successful add stores the interface pointer.
11. Successful add sets `iface->device`.
12. Successful add sets `iface->arp_cache` to `&router->arp_cache`.
13. Successful add binds receive handler/context for router receive.
14. `router_add_route(NULL, ...)` returns `-1`.
15. Valid add-route delegates to route table and rebuilds FIB.
16. `router_del_route(NULL, ...)` returns `-1`.
17. Valid delete-route delegates to route table and rebuilds FIB.
18. `router_receive` rejects NULL inputs.
19. `router_receive` rejects too-short packet and increments `rx_errors`.
20. Non-IPv4 packet is dropped as malformed.
21. Bad IP checksum is dropped as malformed.
22. TTL `0` packet is dropped and not forwarded.
23. TTL `1` packet is dropped and not forwarded.
24. No route drops packet and increments `rx_dropped`.
25. Direct route chooses final destination IP as ARP target.
26. Next-hop route chooses route next-hop as ARP target.
27. ARP hit calls Ethernet send with route egress interface.
28. ARP hit forwards the same packet after TTL/checksum update.
29. ARP miss sends request for the target IP.
30. ARP miss enqueues packet in pending queue.
31. ARP miss enqueue failure frees or otherwise consumes packet.

## Common Mistakes

- Do not include or use `mac_table` in Router.
- Do not call `ip_send` to forward; it creates a new IP header and resets TTL.
- Do not strip the IP header before forwarding.
- Do not ARP for the final destination when the route has a nonzero next hop.
- Do not store network-order IP values in the route table.
- Do not free embedded `arp_cache` or `route_tbl` separately.
- Do not let lookup scan the route RIB directly; use the FIB lookup API.
- Do not forward packets with TTL `0` or `1`.
- Do not forget to recompute the IPv4 checksum after decrementing TTL.
- Do not transfer interface ownership on failed `router_add_interface`.
