# Module 11 - ARP

**Files:** `src/protocols/arp.c`, `src/protocols/arp.h`
**Status:** Implemented core ARP exchange
**Depends on:** `byte_order`, `ethernet`, `packet`, `interface`, `device`,
`arp_cache`, `simulator`, `scheduler`

## Concepts First

IPv4 addresses identify layer-3 endpoints. Ethernet sends frames to layer-2 MAC
addresses.

ARP bridges that gap on a local link:

```text
I know the IPv4 address. What MAC address should Ethernet send to?
```

Example:

```text
Host A knows:  192.168.1.10
Host A needs:  BB:BB:BB:BB:BB:BB
```

ARP answers by sending a local broadcast request and receiving a unicast reply.

### Request And Reply

Request:

```text
Who has 192.168.1.10?
Tell 192.168.1.1 at AA:AA:AA:AA:AA:AA.
```

Reply:

```text
192.168.1.10 is at BB:BB:BB:BB:BB:BB.
```

The request is sent to Ethernet broadcast because the target MAC is unknown.

The reply is sent unicast back to the requester because the request packet
contains the requester's MAC address.

### Both Sides Learn

The responder learns the requester's mapping from the request:

```text
sender_protocol_addr -> sender_hardware_addr
```

The requester learns the responder's mapping from the reply.

In this implementation, both request and reply handlers add the sender mapping
to `iface->arp_cache` when the receiving interface has a cache pointer.

### ARP Protocol Versus ARP Cache

This module owns ARP wire behavior:

- ARP packet layout
- ARP request construction
- ARP reply construction
- ARP request handler registration
- ARP reply handler registration
- received request/reply handling

It does not own the ARP cache object.

The cache is owned by Host or Router. Interface borrows a pointer through:

```c
iface->arp_cache
```

ARP updates that borrowed cache pointer. The cache API and pending-packet rules
are specified in `10_arp_cache.md`.

### Event Dispatch In Current Code

`arp_init(sim)` registers two scheduler fallback handlers:

```text
EVT_ARP_REQUEST -> arp_request_handler, ctx = sim
EVT_ARP_REPLY   -> arp_reply_handler,   ctx = sim
```

The handlers themselves are `static` inside `arp.c`; they are not public API.

The event passed to a handler must contain:

```text
e->dst_device = receiving Interface *
e->packet     = Packet * whose data begins at ArpPacket
```

Ethernet receive strips the Ethernet header before ARP sees the packet. ARP
expects `pkt->data` to point at the ARP payload.

### Byte Order

ARP wire fields are network byte order where the protocol requires multi-byte
integers:

- `hardware_type`
- `protocol_type`
- `opcode`
- `sender_protocol_addr`
- `target_protocol_addr`

The current implementation:

- writes `hardware_type`, `protocol_type`, and `opcode` with `ns_htons`
- stores `iface->ip_addr` directly into sender protocol address
- stores `target_ip` directly into request target protocol address
- compares target IP by applying `ns_ntohl` to both ARP target and
  `iface->ip_addr`
- stores cache IP keys as `ns_ntohl(sender_protocol_addr)`

That means callers should pass `target_ip` to `arp_send_request` in the same
wire/network-order convention used by `Interface.ip_addr`.

## Purpose

The ARP module constructs and handles ARP request/reply packets.

It provides:

- ARP constants
- packed ARP packet layout
- scheduler registration for ARP request/reply events
- ARP request send helper
- ARP reply send helper
- static request/reply handlers used through scheduler registration

It does not:

- allocate or free ARP cache storage
- own pending packet queues
- parse Ethernet headers
- send IP packets directly except through `arp_pending_flush`
- decide route selection
- implement gratuitous ARP
- implement ARP probes
- implement ARP cache timeout cleanup

## Architecture Boundary

| Responsibility | Owner |
| --- | --- |
| Build ARP request/reply payloads | ARP |
| Register ARP event handlers | ARP |
| Convert ARP fixed fields to/from network byte order | ARP |
| Send ARP payload over Ethernet | ARP through Ethernet |
| Store learned mappings | ARP cache |
| Own ARP cache storage | Host/Router |
| Queue unresolved IP payloads | ARP cache pending queue |
| Retry pending payloads after mapping learned | ARP cache via `ip_send` |
| Strip Ethernet header and deliver ARP payload | Ethernet/interface receive path |

ARP depends on Ethernet for frame transmission and on ARP cache for learned
state. ARP should not contain Host or Router-specific ownership logic.

## Data Model

### Constants

```c
#define HARDWARE_TYPE_ETHERNET 1
#define PROTOCOL_TYPE_IPV4     0x0800
#define HARDWARE_ADDR_LEN      6
#define PROTOCOL_ADDR_LEN      4
#define ARP_OPCODE_REQUEST     1
#define ARP_OPCODE_REPLY       2
#define ARP_CACHE_TIMEOUT_MS   300000
```

`ARP_CACHE_TIMEOUT_MS` is declared in `arp.h`, but cleanup logic currently lives
in `arp_cache.c`.

### `ArpPacket`

```c
typedef struct __attribute__((packed)) ArpPacket {
    uint16_t hardware_type;
    uint16_t protocol_type;
    uint8_t  hardware_addr_len;
    uint8_t  protocol_addr_len;
    uint16_t opcode;
    uint8_t  sender_hardware_addr[HARDWARE_ADDR_LEN];
    uint32_t sender_protocol_addr;
    uint8_t  target_hardware_addr[HARDWARE_ADDR_LEN];
    uint32_t target_protocol_addr;
} ArpPacket;
```

Wire layout:

```text
offset  size  field
0       2     hardware_type
2       2     protocol_type
4       1     hardware_addr_len
5       1     protocol_addr_len
6       2     opcode
8       6     sender_hardware_addr
14      4     sender_protocol_addr
18      6     target_hardware_addr
24      4     target_protocol_addr
28            end
```

The struct must be packed so its size and field offsets match the ARP wire
format.

## Ownership And Lifetime

`arp_send_request` creates:

- a `Packet`
- a temporary `ArpPacket`

It prepends the ARP packet bytes into the `Packet`, frees the temporary
`ArpPacket`, and calls `ethernet_send`.

`arp_send_reply` does the same for a reply packet.

Current implementation note: after `ethernet_send` returns, the ARP send helpers
do not free their original `Packet`. Since `ethernet_send`/`link_transmit`
clones for scheduled delivery and does not free the caller packet, this is a
current ownership issue to settle in code/tests.

Handlers do not free the received ARP packet.

ARP never frees `iface->arp_cache`.

## Public API

```c
void arp_init(Simulator *sim);

int  arp_send_request(Simulator *sim,
                      Interface *iface,
                      uint32_t   target_ip);

int  arp_send_reply(Simulator *sim,
                    Interface *iface,
                    Packet    *req_pkt);
```

## Function Behavior

### `arp_init`

Required behavior:

- If `sim == NULL`, return immediately.
- Register `arp_request_handler` for `EVT_ARP_REQUEST` using `sim` as context.
- Register `arp_reply_handler` for `EVT_ARP_REPLY` using `sim` as context.

This function does not allocate or initialize an ARP cache.

### `arp_send_request`

Required behavior:

- If `sim == NULL`, return `-1`.
- If `iface == NULL`, return `-1`.
- If `target_ip == 0`, return `-1`.
- Allocate a packet with capacity `sizeof(ArpPacket)`.
- Allocate a temporary `ArpPacket`.
- If either allocation fails, free any partial allocation and return `-1`.
- Fill the ARP request:
  - `hardware_type = ns_htons(HARDWARE_TYPE_ETHERNET)`
  - `protocol_type = ns_htons(PROTOCOL_TYPE_IPV4)`
  - `hardware_addr_len = HARDWARE_ADDR_LEN`
  - `protocol_addr_len = PROTOCOL_ADDR_LEN`
  - `opcode = ns_htons(ARP_OPCODE_REQUEST)`
  - `sender_hardware_addr = iface->mac`
  - `sender_protocol_addr = iface->ip_addr`
  - `target_hardware_addr = all zero bytes`
  - `target_protocol_addr = target_ip`
- Prepend the ARP bytes into the packet.
- Free the temporary `ArpPacket`.
- Send the packet through Ethernet broadcast:

```c
ethernet_send(sim, iface, ETH_BROADCAST, ETHERTYPE_ARP, pkt)
```

- Return `0` if `ethernet_send` returns a non-negative value.
- Return `-1` if `ethernet_send` returns a negative value.

Current implementation note: the return value from `packet_prepend` is not
checked. With a newly created packet sized to `sizeof(ArpPacket)`, this should
succeed, but the unchecked call is still part of current behavior.

### `arp_send_reply`

Required behavior:

- If `sim == NULL`, return `-1`.
- If `iface == NULL`, return `-1`.
- If `req_pkt == NULL`, return `-1`.
- If `req_pkt->data == NULL`, return `-1`.
- If `req_pkt->len < sizeof(ArpPacket)`, return `-1`.
- Interpret `req_pkt->data` as the request `ArpPacket`.
- Copy `req->sender_hardware_addr` into local `dst_mac`.
- Copy `req->sender_protocol_addr` into local `dst_ip`.
- Allocate a reply packet with capacity `sizeof(ArpPacket)`.
- Allocate a temporary `ArpPacket`.
- If either allocation fails, free any partial allocation and return `-1`.
- Fill the ARP reply:
  - `hardware_type = ns_htons(HARDWARE_TYPE_ETHERNET)`
  - `protocol_type = ns_htons(PROTOCOL_TYPE_IPV4)`
  - `hardware_addr_len = HARDWARE_ADDR_LEN`
  - `protocol_addr_len = PROTOCOL_ADDR_LEN`
  - `opcode = ns_htons(ARP_OPCODE_REPLY)`
  - `sender_hardware_addr = iface->mac`
  - `sender_protocol_addr = iface->ip_addr`
  - `target_hardware_addr = dst_mac`
  - `target_protocol_addr = dst_ip`
- Prepend the ARP bytes into the reply packet.
- Free the temporary `ArpPacket`.
- Send the reply through Ethernet unicast to `dst_mac`.
- Return `0` if `ethernet_send` returns a non-negative value.
- Return `-1` if `ethernet_send` returns a negative value.

Current implementation note: the return value from `packet_prepend` is not
checked here either.

### `arp_request_handler`

This function is static inside `arp.c`.

Required behavior when invoked by the scheduler:

- Interpret `ctx` as `Simulator *`.
- Read receiving interface from `e->dst_device`.
- Read packet from `e->packet`.
- If interface or packet is `NULL`, return immediately.
- Interpret `pkt->data` as `ArpPacket`.
- If `ns_ntohs(opcode) != ARP_OPCODE_REQUEST`, return.
- If `ns_ntohl(target_protocol_addr) != ns_ntohl(iface->ip_addr)`, return.
- Call `arp_send_reply(sim, iface, pkt)`.
- If `iface->arp_cache != NULL`:
  - compute `sender_ip = ns_ntohl(sender_protocol_addr)`
  - add sender mapping to the cache
  - flush pending packets for `sender_ip`
- If reply send returned `0`, set `iface->last_tx_time = e->timestamp`.
- Otherwise increment `iface->tx_errors` and set `last_error_time`.

Current implementation note: the handler does not check packet length before
casting `pkt->data` to `ArpPacket`.

### `arp_reply_handler`

This function is static inside `arp.c`.

Required behavior when invoked by the scheduler:

- Interpret `ctx` as `Simulator *`.
- Read receiving interface from `e->dst_device`.
- Read packet from `e->packet`.
- If interface or packet is `NULL`, return immediately.
- Interpret `pkt->data` as `ArpPacket`.
- If `ns_ntohs(opcode) != ARP_OPCODE_REPLY`, return.
- If `iface->arp_cache != NULL`:
  - compute `sender_ip = ns_ntohl(sender_protocol_addr)`
  - add sender mapping to the cache
  - flush pending packets for `sender_ip`
- Set `iface->last_rx_time = e->timestamp`.

Current implementation note: the handler does not verify that the reply target
IP/MAC is actually this interface before learning the sender.

## Flow Charts

### Send Request

```text
arp_send_request(sim, iface, target_ip)
  |
  +-- reject NULL sim/iface or zero target_ip
  |
  +-- create Packet
  +-- allocate ArpPacket
  |
  +-- fill request fields
  |
  +-- prepend ARP payload into Packet
  |
  +-- ethernet_send(... ETH_BROADCAST, ETHERTYPE_ARP, pkt)
  |
  +-- ethernet result >= 0: return 0
  |
  +-- ethernet result < 0: return -1
```

### Handle Request

```text
arp_request_handler(e, sim)
  |
  +-- iface = e->dst_device
  +-- pkt = e->packet
  |
  +-- missing iface/pkt: return
  |
  +-- opcode not REQUEST: return
  |
  +-- target IP not iface IP: return
  |
  +-- arp_send_reply(sim, iface, pkt)
  |
  +-- if iface->arp_cache:
        |
        +-- learn sender IP -> sender MAC
        +-- flush pending packets for sender IP
```

### Handle Reply

```text
arp_reply_handler(e, sim)
  |
  +-- iface = e->dst_device
  +-- pkt = e->packet
  |
  +-- missing iface/pkt: return
  |
  +-- opcode not REPLY: return
  |
  +-- if iface->arp_cache:
        |
        +-- learn sender IP -> sender MAC
        +-- flush pending packets for sender IP
  |
  +-- last_rx_time = e->timestamp
```

## ACSL Contracts

The contracts belong in `arp.h`. The static handlers should be covered by KLEVA
tests through `arp_init` and scheduled events, or by test-only wrappers if
needed.

### Shared Predicates

```c
/*@
    predicate arp_packet_readable(Packet *pkt) =
        packet_visible_bytes(pkt) &&
        pkt->len >= 28;

    predicate arp_request_wire(ArpPacket *arp) =
        \valid_read(arp) &&
        arp->hardware_addr_len == 6 &&
        arp->protocol_addr_len == 4 &&
        arp->opcode == ns_htons(ARP_OPCODE_REQUEST);

    predicate arp_reply_wire(ArpPacket *arp) =
        \valid_read(arp) &&
        arp->hardware_addr_len == 6 &&
        arp->protocol_addr_len == 4 &&
        arp->opcode == ns_htons(ARP_OPCODE_REPLY);
*/
```

### `arp_init`

```c
/*@
    behavior null:
        assumes sim == \null;
        assigns \nothing;

    behavior valid:
        assumes simulator_well_formed(sim);
        assigns sim->sched->handlers[EVT_ARP_REQUEST],
                sim->sched->handlers[EVT_ARP_REPLY];
        ensures sim->sched->handlers[EVT_ARP_REQUEST].ctx == sim;
        ensures sim->sched->handlers[EVT_ARP_REPLY].ctx == sim;

    complete behaviors;
    disjoint behaviors;
*/
void arp_init(Simulator *sim);
```

Additional required proof/test property:

- `EVT_ARP_REQUEST` handler function becomes the ARP request handler.
- `EVT_ARP_REPLY` handler function becomes the ARP reply handler.

### `arp_send_request`

```c
/*@
    behavior bad_input:
        assumes sim == \null || iface == \null || target_ip == 0;
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes simulator_well_formed(sim);
        assumes interface_basic_valid(iface);
        assumes target_ip != 0;
        assigns sim->sched->eq->events,
                sim->sched->eq->events[0 .. sim->sched->eq->capacity - 1],
                sim->sched->eq->count,
                sim->sched->eq->capacity,
                iface->tx_bytes,
                iface->tx_errors,
                iface->last_tx_time,
                iface->last_error_time;
        ensures \result == 0 || \result == -1;

    complete behaviors;
    disjoint behaviors;
*/
int arp_send_request(Simulator *sim,
                     Interface *iface,
                     uint32_t target_ip);
```

Additional required proof/test property:

- Constructed request uses broadcast Ethernet destination.
- Constructed request opcode is network-order `ARP_OPCODE_REQUEST`.
- Sender MAC equals `iface->mac`.
- Sender protocol address equals `iface->ip_addr`.
- Target hardware address is all zero bytes.
- Target protocol address equals `target_ip`.

### `arp_send_reply`

```c
/*@
    behavior bad_input:
        assumes sim == \null || iface == \null || req_pkt == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior bad_packet:
        assumes simulator_well_formed(sim);
        assumes interface_basic_valid(iface);
        assumes req_pkt != \null;
        assumes req_pkt->data == \null || req_pkt->len < 28;
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes simulator_well_formed(sim);
        assumes interface_basic_valid(iface);
        assumes arp_packet_readable(req_pkt);
        assigns sim->sched->eq->events,
                sim->sched->eq->events[0 .. sim->sched->eq->capacity - 1],
                sim->sched->eq->count,
                sim->sched->eq->capacity,
                iface->tx_bytes,
                iface->tx_errors,
                iface->last_tx_time,
                iface->last_error_time;
        ensures \result == 0 || \result == -1;

    complete behaviors;
    disjoint behaviors;
*/
int arp_send_reply(Simulator *sim,
                   Interface *iface,
                   Packet *req_pkt);
```

Additional required proof/test property:

- Constructed reply opcode is network-order `ARP_OPCODE_REPLY`.
- Reply destination MAC equals request sender hardware address.
- Reply sender MAC equals `iface->mac`.
- Reply sender protocol address equals `iface->ip_addr`.
- Reply target protocol address equals request sender protocol address.

## KLEVA Verification Plan

Minimum KLEVA tests:

1. `arp_init(NULL)` does not crash.
2. `arp_init(valid)` registers request handler context as `sim`.
3. `arp_init(valid)` registers reply handler context as `sim`.
4. `arp_send_request` rejects NULL simulator, NULL interface, and zero target.
5. Request allocation failure returns `-1` and frees partial allocation.
6. Request construction writes hardware type in network order.
7. Request construction writes protocol type in network order.
8. Request construction writes opcode request in network order.
9. Request construction copies interface MAC into sender hardware address.
10. Request construction stores interface IP as sender protocol address.
11. Request construction zeroes target hardware address.
12. Request construction stores target IP exactly as provided.
13. Request send uses Ethernet broadcast destination.
14. Request send normalizes non-negative Ethernet result to return `0`.
15. `arp_send_reply` rejects NULL simulator, interface, or request packet.
16. `arp_send_reply` rejects NULL request data.
17. `arp_send_reply` rejects request shorter than 28 bytes.
18. Reply construction writes opcode reply in network order.
19. Reply construction copies request sender MAC into target hardware address.
20. Reply construction sends Ethernet unicast to request sender MAC.
21. Reply construction stores interface MAC/IP as sender fields.
22. Reply send normalizes non-negative Ethernet result to return `0`.
23. Request handler ignores missing interface or packet.
24. Request handler ignores non-request opcode.
25. Request handler ignores target IP not matching receiving interface.
26. Request handler sends reply for matching request.
27. Request handler learns sender mapping when `iface->arp_cache` exists.
28. Request handler flushes pending packets for learned sender IP.
29. Reply handler ignores missing interface or packet.
30. Reply handler ignores non-reply opcode.
31. Reply handler learns sender mapping when `iface->arp_cache` exists.
32. Reply handler flushes pending packets for learned sender IP.
33. Reply handler updates `last_rx_time`.

## Common Mistakes

- Do not say ARP owns the ARP cache object.
- Do not make Interface own or free the ARP cache.
- Do not call ARP handlers directly from outside `arp.c`; they are static.
- Do not forget that `arp_send_request` expects `target_ip` in the same wire
  order convention as `iface->ip_addr`.
- Do not treat Ethernet success `1` as ARP failure; ARP helpers normalize
  non-negative Ethernet results to `0`.
- Do not hide the current packet ownership issue after ARP send helpers call
  `ethernet_send`.
- Do not claim handlers validate packet length; current static handlers do not.
