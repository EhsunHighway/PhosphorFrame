# Module 18 - Host

**Files:** `src/network/host.c`, `src/network/host.h`
**Status:** Implemented endpoint composition module
**Depends on:** `device`, `interface`, `packet`, `ethernet`, `arp_cache`,
`arp`, `ip`, `icmp`, `udp`, `tcp`, `simulator`

## Concepts First

Host is mostly glue code, so the concepts matter more than the number of lines
in the module.

A Host is one endpoint machine in the simulator. It owns the state needed by
one endpoint:

- a base `Device`
- an interface array through that base device
- one ARP cache
- one IP stack
- one UDP socket table
- one TCP connection table
- UDP and TCP context objects used by IP dispatch
- an optional default gateway address

Host is not a switch and not a router. It does not forward packets between
interfaces.

### Per-Host Protocol State

Per-host state means state that belongs to one Host object, not to the whole
simulator.

This matters because multiple hosts can use the same protocol values at the
same time:

```text
host A can bind UDP port 520
host B can also bind UDP port 520

host A can listen on TCP port 80
host B can also listen on TCP port 80
```

That works only if each Host owns a separate `UdpState` and `TcpTable`.

### Context Pointer

The IP stack stores handlers in a protocol dispatch table.

Each protocol table entry has:

- a protocol number, such as `IPPROTO_UDP`
- a handler function, such as `udp_receive`
- an opaque context pointer, stored as `void *`

Opaque means IP stores the pointer and passes it back later, but IP does not
inspect what it points to.

For UDP, the context pointer is this Host's `UdpContext`.

For TCP, the context pointer is this Host's `TcpContext`.

For ICMP, the context pointer is the simulator pointer.

### Registering A Protocol Handler

Registering a protocol handler means writing one entry in the Host-owned
`IpStack` dispatch table.

It does not call the handler immediately.

Later, when `ip_receive` accepts an IPv4 packet and sees the protocol byte, IP
uses that table entry to call the right receive function with the stored
context pointer.

```text
IPv4 protocol 1  -> icmp_receive(..., sim)
IPv4 protocol 17 -> udp_receive(..., host->udp_context)
IPv4 protocol 6  -> tcp_receive(..., host->tcp_context)
```

### Empty ARP Cache

An empty ARP cache is not a NULL pointer.

An empty ARP cache is a valid `ArpCache` object whose learned-entry count and
pending-packet count are zero, with all valid bits cleared.

Host must use `arp_cache_init` to create this state. Host owns the storage; the
ARP cache module owns the initialization rule.

### Gateway Boundary

Host stores `gateway_ip`, but current packet output still goes through
`ip_output`.

Current `ip_output` only sends to destinations reachable on the same subnet as
the selected source interface. Gateway routing is future routing/IP-output
work, not Host-level Ethernet bypass work.

## Purpose

The Host module creates and wires one endpoint.

It provides:

- Host allocation and release
- embedded Device initialization
- Host-owned ARP cache allocation and initialization
- Host-owned IP stack allocation and initialization
- UDP/TCP state allocation and initialization
- UDP/TCP dispatch context allocation
- ICMP/UDP/TCP registration with this Host's IP stack
- interface addition with ARP cache and IP receive binding
- Host-level receive wrapper for tests
- Host-level IP send wrapper

It does not:

- implement ARP packet logic
- parse IPv4, ICMP, UDP, or TCP headers
- choose Ethernet destination MAC addresses directly
- perform packet forwarding
- implement gateway routing
- allocate or own the simulator

## Architecture Boundary

| Responsibility | Owner |
| --- | --- |
| Interface list | embedded `Device` |
| Interface objects after successful add | Host through `Device` |
| ARP cache storage | Host |
| ARP cache initialization rules | ARP cache module |
| IPv4 receive dispatch table | Host-owned `IpStack` |
| ICMP parsing and replies | ICMP module |
| UDP socket table storage | Host-owned `UdpState` |
| UDP receive dispatch | UDP module using `UdpContext` |
| TCP connection table storage | Host-owned `TcpTable` |
| TCP receive dispatch | TCP module using `TcpContext` |
| Outgoing interface choice | IP output |
| ARP resolution and pending queue | ARP/IP path |
| Gateway route decision | future routing/IP output |

Host may call public initialization and registration APIs. Host must not reach
inside protocol internals when a public API exists.

## Data Model

### Constants

```c
#define HOST_MAX_PORTS 8
```

`HOST_MAX_PORTS` is the capacity of the embedded Device interface array created
by `host_create`.

### `Host`

```c
typedef struct Host {
    Device      base;

    Simulator  *sim;

    ArpCache   *arp_cache;
    IpStack    *ip_stack;

    UdpState   *udp_state;
    UdpContext *udp_context;

    TcpTable   *tcp_table;
    TcpContext *tcp_context;

    uint32_t   gateway_ip;
} Host;
```

`base` is the embedded Device. It is the first field.

Current storage model:

| Field | Ownership |
| --- | --- |
| `base` | embedded in Host |
| `base.interfaces` | allocated and freed by Host |
| `base.interfaces[i]` | owned by Host after successful `host_add_interface` |
| `sim` | borrowed; Host does not free it |
| `arp_cache` | allocated and freed by Host |
| `ip_stack` | allocated and freed by Host |
| `udp_state` | allocated and freed by Host |
| `udp_context` | allocated and freed by Host |
| `tcp_table` | allocated and freed by Host |
| `tcp_context` | allocated and freed by Host |
| `gateway_ip` | host-order IPv4 address; `0` means none |

### Required Successful Creation State

After successful `host_create`:

| State | Required value |
| --- | --- |
| `host->sim` | equal to input `sim` |
| `host->base.iface_count` | `0` |
| `host->base.iface_max` | `8` |
| `host->base.interfaces` | non-NULL |
| `host->arp_cache` | non-NULL and initialized by `arp_cache_init` |
| `host->ip_stack` | non-NULL and initialized by `ip_stack_init` |
| `host->udp_state` | non-NULL and initialized by `udp_init` |
| `host->tcp_table` | non-NULL and initialized by `tcp_init` |
| `host->udp_context` | non-NULL, points to `sim` and `udp_state` |
| `host->tcp_context` | non-NULL, points to `sim` and `tcp_table` |
| `host->gateway_ip` | equal to input `gateway_ip` |

### ARP Cache Initial State

`arp_cache_init(host->arp_cache)` must produce:

- `count == 0`
- `pending_count == 0`
- every learned entry has `valid == 0`
- every pending entry has `valid == 0`
- pending packet/interface pointers are cleared by the ARP cache initializer

Host must not treat `NULL` as empty.

### UDP And TCP Initial State

Host allocates `udp_state` and then calls:

```c
udp_init(host->udp_state);
```

After that call:

- `host->udp_state->count == 0`
- every UDP socket slot has `valid == 0`

Host allocates `tcp_table` and then calls:

```c
tcp_init(host->tcp_table);
```

After that call:

- `host->tcp_table->count == 0`
- every TCB slot has `valid == 0`

## Ownership And Lifetime

`host_create` owns every pointer allocation it stores in `Host`, except the
borrowed `Simulator *`.

If any creation step fails, `host_create` calls `host_free` on the partially
created Host and returns `NULL`.

`host_add_interface` transfers interface ownership only on success.

On successful `host_add_interface`, `host_free` will free the interface.

On failed `host_add_interface`, the caller still owns the interface.

`host_receive` consumes a non-IPv4 packet by freeing it. For IPv4, ownership is
delegated to `ip_receive`.

`host_send_ip` does not free payload on Host-level validation failure. For valid
input, ownership follows `ip_output`:

- if `ip_output` returns success after sending or enqueueing, IP/ARP/lower
  layers own the payload
- if `ip_output` fails before transfer, the caller still owns the payload

Current `ip_output` can fail before transfer when no source interface exists,
the interface is down, the ARP cache is missing, or the destination is
off-subnet.

## Public API

```c
Host *host_create(const char *name,
                  Simulator  *sim,
                  uint32_t    gateway_ip);

void host_free(Host *host);

int host_add_interface(Host *host, Interface *iface);

int host_receive(Host      *host,
                 Interface *iface,
                 Packet    *pkt,
                 uint16_t   ethertype);

int host_send_ip(Host     *host,
                 uint32_t  src_ip,
                 uint32_t  dst_ip,
                 uint8_t   protocol,
                 Packet   *payload);
```

Host must not duplicate UDP or TCP packet construction. UDP and TCP have their
own send APIs.

## Function Behavior

### `host_create`

Required behavior:

- If `name == NULL || sim == NULL`, return `NULL`.
- Allocate and zero one Host.
- Copy the name into `host->base.name` with termination.
- Store borrowed simulator pointer in `host->sim`.
- Set `host->base.iface_max = HOST_MAX_PORTS`.
- Allocate `host->base.interfaces` with capacity `HOST_MAX_PORTS`.
- Set `host->base.iface_count = 0`.
- Allocate `host->arp_cache`.
- Call `arp_cache_init(host->arp_cache)`.
- Allocate `host->ip_stack`.
- Call `ip_stack_init(host->ip_stack, sim)`.
- Allocate `host->udp_state`.
- Call `udp_init(host->udp_state)`.
- Allocate `host->tcp_table`.
- Call `tcp_init(host->tcp_table)`.
- Allocate `host->udp_context`.
- Set `host->udp_context->sim = sim`.
- Set `host->udp_context->state = host->udp_state`.
- Allocate `host->tcp_context`.
- Set `host->tcp_context->sim = sim`.
- Set `host->tcp_context->table = host->tcp_table`.
- Register protocol handlers in this Host's IP stack:

| Protocol | Handler | Context |
| --- | --- | --- |
| `IPPROTO_ICMP` | `icmp_receive` | `sim` |
| `IPPROTO_UDP` | `udp_receive` | `host->udp_context` |
| `IPPROTO_TCP` | `tcp_receive` | `host->tcp_context` |

- Store `host->gateway_ip = gateway_ip`.
- Return the Host.

If any allocation or registration fails, release all Host-owned state already
created and return `NULL`.

### `host_free`

Required behavior:

- If `host == NULL`, return.
- For each interface pointer stored in `host->base.interfaces[0 ..
  iface_count - 1]`, call `interface_free`.
- Free `host->base.interfaces`.
- Free `host->arp_cache`.
- Free `host->ip_stack`.
- Free `host->udp_state`.
- Free `host->tcp_table`.
- Free `host->udp_context`.
- Free `host->tcp_context`.
- Free the Host.

Cleanup must match the pointer storage model in `host.h`.

### `host_add_interface`

Required behavior:

- If `host == NULL`, return `-1`.
- If `iface == NULL`, return `-1`.
- If `host->ip_stack == NULL`, return `-1`.
- If `host->arp_cache == NULL`, return `-1`.
- If the embedded Device interface array is full, return `-1`.
- Call `device_add_interface(&host->base, iface)`.
- If `device_add_interface` fails, return `-1`.
- Call `interface_set_arp_cache(iface, host->arp_cache)`.
- Call `ip_stack_bind_interface(host->ip_stack, iface)`.
- Return `0`.

Current implementation does not roll back `device_add_interface` if
`ip_stack_bind_interface` were to fail. The current `ip_stack_bind_interface`
fails only on NULL input, and Host checks those inputs first.

### `host_receive`

Required behavior:

- If `host == NULL`, return `-1`.
- If `iface == NULL`, return `-1`.
- If `pkt == NULL`, return `-1`.
- If `host->ip_stack == NULL`, return `-1`.
- If `ethertype != ETHERTYPE_IPV4`, free `pkt` and return `-1`.
- For IPv4, call:

```c
ip_receive(iface, pkt, ethertype, host->ip_stack);
```

- Return the result from `ip_receive`.

"Deliver the packet to this Host's IP receive path" means call `ip_receive`
with this Host's `ip_stack` as the opaque context. Host does not parse IPv4 or
upper-layer protocol headers in this function.

### `host_send_ip`

Required behavior:

- If `host == NULL`, return `-1`.
- If `host->sim == NULL`, return `-1`.
- If `payload == NULL`, return `-1`.
- If `src_ip == 0 || dst_ip == 0`, return `-1`.
- Call:

```c
ip_output(host->sim, src_ip, dst_ip, protocol, payload);
```

- Return the result from `ip_output`.

`src_ip` and `dst_ip` are host-order IPv4 addresses.

Host must not choose the outgoing interface, perform ARP lookup, or build
Ethernet frames itself.

## Flow Charts

### Host Creation

```text
host_create(name, sim, gateway_ip)
  |
  +-- reject NULL name or simulator
  +-- allocate and zero Host
  +-- initialize embedded Device capacity
  +-- allocate interface array
  +-- allocate ARP cache
  +-- arp_cache_init
  +-- allocate IP stack
  +-- ip_stack_init
  +-- allocate UDP state
  +-- udp_init
  +-- allocate TCP table
  +-- tcp_init
  +-- allocate UDP context -> sim + udp_state
  +-- allocate TCP context -> sim + tcp_table
  +-- register ICMP, UDP, TCP with IP stack
  +-- store gateway_ip
  +-- return Host
```

### Receive Path

```text
host_receive(host, iface, pkt, ethertype)
  |
  +-- reject NULL inputs
  |
  +-- ethertype != IPv4:
  |     free pkt
  |     return -1
  |
  +-- IPv4:
        ip_receive(iface, pkt, ethertype, host->ip_stack)
```

### Protocol Dispatch After IP Receive

```text
ip_receive(..., host->ip_stack)
  |
  +-- IPPROTO_ICMP -> icmp_receive(ctx = sim)
  +-- IPPROTO_UDP  -> udp_receive(ctx = host->udp_context)
  +-- IPPROTO_TCP  -> tcp_receive(ctx = host->tcp_context)
```

### Send Path

```text
host_send_ip(host, src_ip, dst_ip, protocol, payload)
  |
  +-- reject NULL host, NULL simulator, NULL payload
  +-- reject zero source or destination IP
  |
  +-- ip_output(host->sim, src_ip, dst_ip, protocol, payload)
        |
        +-- IP chooses source interface
        +-- IP checks subnet reachability
        +-- ARP cache hit: send IP packet
        +-- ARP cache miss: send ARP request and enqueue pending payload
```

## ACSL Contracts

The contracts belong in `host.h`. Use literal bounds:

- Host interface capacity: `8`
- ARP cache entries: `256`
- ARP pending entries: `32`
- UDP sockets: `32`
- TCP TCBs: `64`

### `host_create`

```c
/*@
    behavior bad_input:
        assumes name == \null || sim == \null;
        assigns \nothing;
        ensures \result == \null;

    behavior valid_input:
        assumes name != \null && sim != \null;
        allocates \result;
        ensures \result == \null || \valid(\result);
        ensures \result != \null ==> \result->sim == sim;
        ensures \result != \null ==> \result->base.iface_count == 0;
        ensures \result != \null ==> \result->base.iface_max == 8;
        ensures \result != \null ==> \result->base.interfaces != \null;
        ensures \result != \null ==> \result->arp_cache != \null;
        ensures \result != \null ==> \result->ip_stack != \null;
        ensures \result != \null ==> \result->udp_state != \null;
        ensures \result != \null ==> \result->udp_context != \null;
        ensures \result != \null ==> \result->tcp_table != \null;
        ensures \result != \null ==> \result->tcp_context != \null;
        ensures \result != \null ==> \result->gateway_ip == gateway_ip;

    complete behaviors;
    disjoint behaviors;
*/
Host *host_create(const char *name, Simulator *sim, uint32_t gateway_ip);
```

Additional required proof/test property:

- Successful creation initializes ARP cache counts and valid bits.
- Successful creation initializes UDP count and valid bits.
- Successful creation initializes TCP count and valid bits.
- Successful creation registers ICMP/UDP/TCP handlers with the expected
  contexts.

### `host_free`

```c
/*@
    behavior null:
        assumes host == \null;
        assigns \nothing;

    behavior valid:
        assumes \valid(host);
        frees host->base.interfaces[0 .. host->base.iface_count - 1],
              host->tcp_context,
              host->udp_context,
              host->tcp_table,
              host->udp_state,
              host->ip_stack,
              host->arp_cache,
              host->base.interfaces,
              host;

    complete behaviors;
    disjoint behaviors;
*/
void host_free(Host *host);
```

### `host_add_interface`

```c
/*@
    behavior bad_input:
        assumes host == \null || iface == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior missing_state:
        assumes \valid(host) && iface != \null;
        assumes host->ip_stack == \null || host->arp_cache == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior full:
        assumes \valid(host) && \valid(iface);
        assumes host->ip_stack != \null && host->arp_cache != \null;
        assumes host->base.iface_count >= host->base.iface_max;
        assigns \nothing;
        ensures \result == -1;

    behavior added:
        assumes \valid(host) && \valid(iface);
        assumes host->ip_stack != \null && host->arp_cache != \null;
        assumes host->base.iface_count < host->base.iface_max;
        assigns host->base.interfaces[0 .. host->base.iface_count],
                host->base.iface_count,
                iface->device,
                iface->arp_cache,
                iface->rx_handler,
                iface->handler_ctx;
        ensures \result == 0 || \result == -1;
        ensures \result == 0 ==>
                host->base.iface_count == \old(host->base.iface_count) + 1;
        ensures \result == 0 ==>
                host->base.interfaces[\old(host->base.iface_count)] == iface;
        ensures \result == 0 ==> iface->device == &host->base;
        ensures \result == 0 ==> iface->arp_cache == host->arp_cache;

    complete behaviors;
    disjoint behaviors;
*/
int host_add_interface(Host *host, Interface *iface);
```

### `host_receive`

```c
/*@
    behavior bad_input:
        assumes host == \null || iface == \null || pkt == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior missing_stack:
        assumes host != \null && iface != \null && pkt != \null;
        assumes host->ip_stack == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior non_ipv4:
        assumes \valid(host) && \valid(iface) && \valid(pkt);
        assumes host->ip_stack != \null;
        assumes ethertype != 0x0800;
        frees pkt;
        ensures \result == -1;

    behavior ipv4:
        assumes \valid(host) && \valid(iface) && \valid(pkt);
        assumes host->ip_stack != \null;
        assumes ethertype == 0x0800;
        assigns iface->rx_bytes,
                iface->last_rx_time,
                iface->rx_errors,
                iface->rx_dropped,
                pkt->data,
                pkt->len,
                pkt->layer;
        ensures \result == 0 || \result == -1;

    complete behaviors;
    disjoint behaviors;
*/
int host_receive(Host *host, Interface *iface, Packet *pkt, uint16_t ethertype);
```

### `host_send_ip`

```c
/*@
    behavior bad_input:
        assumes host == \null || payload == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior missing_sim:
        assumes host != \null && payload != \null;
        assumes host->sim == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior bad_address:
        assumes \valid(host) && \valid(payload);
        assumes host->sim != \null;
        assumes src_ip == 0 || dst_ip == 0;
        assigns \nothing;
        ensures \result == -1;

    behavior send:
        assumes \valid(host) && \valid(payload);
        assumes host->sim != \null;
        assumes src_ip != 0 && dst_ip != 0;
        assigns payload->data,
                payload->len,
                payload->capacity,
                payload->layer;
        ensures \result == 0 || \result == -1;

    complete behaviors;
    disjoint behaviors;
*/
int host_send_ip(Host *host,
                 uint32_t src_ip,
                 uint32_t dst_ip,
                 uint8_t protocol,
                 Packet *payload);
```

## KLEVA Verification Plan

Minimum KLEVA tests:

1. `host_create(NULL, sim, 0)` returns NULL.
2. `host_create("h1", NULL, 0)` returns NULL.
3. Valid `host_create` returns non-NULL Host-owned state pointers.
4. Valid `host_create` sets Device interface count to `0` and capacity to `8`.
5. Valid `host_create` initializes ARP cache count and pending count to `0`.
6. Valid `host_create` clears every ARP learned-entry valid bit.
7. Valid `host_create` clears every ARP pending-entry valid bit.
8. Valid `host_create` initializes IP stack with simulator pointer.
9. Valid `host_create` registers ICMP handler with simulator context.
10. Valid `host_create` registers UDP handler with Host UDP context.
11. Valid `host_create` registers TCP handler with Host TCP context.
12. Valid `host_create` initializes UDP state count to `0`.
13. Valid `host_create` clears every UDP socket valid bit.
14. Valid `host_create` initializes TCP table count to `0`.
15. Valid `host_create` clears every TCB valid bit.
16. `host_add_interface(NULL, iface)` returns `-1`.
17. `host_add_interface(host, NULL)` returns `-1`.
18. Missing Host IP stack returns `-1`.
19. Missing Host ARP cache returns `-1`.
20. Full Host returns `-1` without incrementing count.
21. Successful add increments interface count.
22. Successful add stores the interface pointer.
23. Successful add sets `iface->device`.
24. Successful add sets `iface->arp_cache`.
25. Successful add binds interface receive handler/context to IP stack.
26. `host_receive` rejects NULL Host, interface, or packet.
27. `host_receive` rejects missing Host IP stack.
28. `host_receive` rejects non-IPv4 ethertype and frees packet.
29. `host_receive` with IPv4 calls IP receive using Host IP stack.
30. `host_send_ip` rejects NULL Host.
31. `host_send_ip` rejects missing Host simulator.
32. `host_send_ip` rejects NULL payload.
33. `host_send_ip` rejects zero source or destination IP.
34. Valid `host_send_ip` reaches `ip_output`.
35. Host send path does not perform ARP lookup directly.

Use small wrappers. Separate Host creation, interface binding, receive
rejection, and send rejection instead of making one giant lifecycle proof.

## Common Mistakes

- Do not treat an empty ARP cache as NULL.
- Do not manually initialize ARP cache fields from Host.
- Do not use one global UDP state or TCP table for every Host.
- Do not call ICMP, UDP, or TCP receive handlers during Host creation.
- Do not pass `&host->tcp_ctx`; the current field is `host->tcp_context`.
- Do not make Host parse IPv4, UDP, TCP, or ICMP.
- Do not make Host choose Ethernet destination MAC addresses.
- Do not make Host forward packets like a router.
- Do not free payload inside `host_send_ip` on validation failure.
- Do not let `host.h`, `host.c`, ACSL contracts, and this spec disagree about
  pointer versus embedded storage.
