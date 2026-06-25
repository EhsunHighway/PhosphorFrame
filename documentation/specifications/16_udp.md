# Module 16 - UDP

**Files:** `src/protocols/udp.c`, `src/protocols/udp.h`
**Status:** Implemented core UDP sockets, send, receive
**Depends on:** `ip`, `packet`, `interface`, `simulator`, `icmp`, `byte_order`

## Concepts First

UDP is a small transport protocol above IPv4.

It provides ports, not connections.

```text
IPv4 address identifies a host/interface.
UDP port identifies an application/protocol endpoint on that host.
```

Example:

```text
10.0.0.1:520 -> RIP over UDP
10.0.0.1:53  -> DNS-like service
```

UDP does not provide:

- connection setup
- retransmission
- ordering
- flow control
- congestion control

It only wraps payload bytes with source port, destination port, length, and
checksum fields.

### Per-Host UDP State

UDP ports are not global to the whole simulator.

Two different hosts can both bind UDP port `520`. Therefore UDP state belongs
to the host/router/protocol owner, not to the simulator globally.

The module represents that with:

```c
typedef struct UdpState {
    UdpSocket sockets[UDP_MAX_SOCKETS];
    int       count;
} UdpState;
```

Host owns a `UdpState`. IP dispatch passes a `UdpContext` to `udp_receive` so
UDP can find both:

- simulator pointer for ICMP Port Unreachable
- this host's UDP socket table

### Two Levels Of Receive Callback

There are two different callback levels.

IP-level handler:

```c
int udp_receive(Interface *iface, Packet *pkt, void *ctx);
```

This is registered with IP for protocol `IPPROTO_UDP`.

Application/protocol-level handler:

```c
typedef void (*Udp_Recv_Handler)(uint32_t src_ip,
                                 uint16_t src_port,
                                 Packet *payload,
                                 void *ctx);
```

This is stored in a UDP socket for one destination port.

Receive flow:

```text
ip_receive
  |
  +-- udp_receive(iface, pkt, udp_ctx)
        |
        +-- dst_port lookup in udp_ctx->state
              |
              +-- found: strip UDP header and call socket callback
              |
              +-- missing: send ICMP Port Unreachable if simulator exists
```

### UDP Checksum In This Simulator

The UDP header has a checksum field, but the current implementation sends:

```c
checksum = 0
```

and receive does not validate UDP checksums.

In IPv4 UDP, checksum zero means checksum disabled. That is the simplified
behavior used here.

### Missing Port Means ICMP Port Unreachable

If a UDP datagram arrives for a destination port with no bound socket, UDP does
not silently drop it when a simulator context exists.

It calls:

```c
icmp_send_unreach_port(udp_ctx->sim, iface, pkt)
```

That means the packet is transferred to the ICMP error helper. UDP does not
strip the UDP header before doing this; ICMP needs the original IP header and
first bytes of the original UDP payload/header for quoting.

## Purpose

The UDP module implements basic datagram send/receive and port dispatch.

It provides:

- UDP header layout and constants
- UDP state initialization
- port bind
- port unbind
- datagram send
- datagram receive
- delivery to bound receive callback
- ICMP Port Unreachable on missing destination port

It does not:

- validate UDP checksums
- compute UDP pseudo-header checksums
- retransmit
- preserve ordering
- allocate `UdpState`
- register itself with IP
- own the simulator

## Architecture Boundary

| Responsibility | Owner |
| --- | --- |
| Store bound UDP ports | UdpState |
| Own UdpState storage | Host/Router/protocol owner |
| Register UDP with IP | IP stack owner |
| Parse UDP header | UDP |
| Dispatch to port callback | UDP |
| Send Port Unreachable | UDP through ICMP |
| Build IPv4 header and resolve ARP | IP output |
| Free payload after app receive | Application/protocol callback |

UDP should not know about RIP/DNS/DHCP internals. Those protocols bind ports
and provide callbacks.

## Data Model

### Constants

```c
#define UDP_HDR_LEN       8
#define UDP_MAX_SOCKETS   32
#define UDP_PORT_DNS      53
#define UDP_PORT_DHCP_SRV 67
#define UDP_PORT_DHCP_CLI 68
#define UDP_PORT_RIP      520
```

`IPPROTO_UDP` is defined in `ip.h` as protocol number `17`.

### `UdpHeader`

```c
typedef struct __attribute__((packed)) UdpHeader {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
} UdpHeader;
```

Wire layout:

```text
offset  size  field
0       2     source port
2       2     destination port
4       2     UDP length: header + payload
6       2     checksum, currently zero on send
8             payload begins
```

All UDP header fields are network byte order.

### `UdpSocket`

```c
struct UdpSocket {
    uint16_t         port;
    int              valid;
    Udp_Recv_Handler recv_handler;
    void            *ctx;
};
```

Field meanings:

| Field | Meaning |
| --- | --- |
| `port` | Bound UDP port in host order. |
| `valid` | `1` means socket slot is bound. |
| `recv_handler` | Callback invoked on receive hit. |
| `ctx` | Opaque callback context. |

### `UdpState`

```c
typedef struct UdpState {
    UdpSocket sockets[UDP_MAX_SOCKETS];
    int       count;
} UdpState;
```

`count` is the number of valid socket slots.

Slots are not compacted. All operations scan all 32 slots.

### `UdpContext`

```c
typedef struct UdpContext {
    Simulator *sim;
    UdpState  *state;
} UdpContext;
```

`UdpContext` is the opaque object passed to IP registration:

```c
ip_stack_register_protocol(stack, IPPROTO_UDP, udp_receive, &udp_ctx);
```

## Ownership And Lifetime

`udp_init` initializes existing `UdpState` storage. It does not allocate.

`udp_bind` and `udp_unbind` do not allocate or free packets.

`udp_send` allocates a new `Packet`. If `ip_output` returns `-1`, UDP frees the
packet. If `ip_output` returns `0` or positive, UDP does not free it.

`udp_receive` consumes every non-NULL packet passed to it:

- malformed receive paths free the packet
- receive hit strips the UDP header and transfers packet ownership to the bound
  socket callback
- missing port with simulator transfers packet to ICMP Port Unreachable helper
- missing port without simulator frees the packet

The `Udp_Recv_Handler` callback owns the stripped payload packet after UDP calls
it.

## Public API

```c
void udp_init(UdpState *state);

int  udp_bind(UdpState        *state,
              uint16_t         port,
              Udp_Recv_Handler recv_handler,
              void            *ctx);

int  udp_unbind(UdpState *state, uint16_t port);

int  udp_send(Simulator     *sim,
              uint32_t       src_ip,
              uint32_t       dst_ip,
              uint16_t       src_port,
              uint16_t       dst_port,
              const uint8_t *payload,
              size_t         payload_len);

int  udp_receive(Interface *iface,
                 Packet    *pkt,
                 void      *ctx);
```

## Function Behavior

### `udp_init`

Required behavior:

- If `state == NULL`, return immediately.
- For each socket slot, set `valid = 0`.
- Set `state->count = 0`.

Current implementation does not clear `port`, `recv_handler`, or `ctx` for each
socket slot. The `valid` bit is the authority.

### `udp_bind`

Required behavior:

- If `state == NULL`, return `-1`.
- If `recv_handler == NULL`, return `-1`.
- If `port == 0`, return `-1`.
- Scan all 32 sockets for a valid socket already bound to `port`.
- If duplicate exists, return `-1`.
- Scan all 32 sockets for first invalid slot.
- If no free slot exists, return `-1`.
- Fill the free slot:
  - `valid = 1`
  - `port = port`
  - `recv_handler = recv_handler`
  - `ctx = ctx`
- Increment `state->count`.
- Return `0`.

### `udp_unbind`

Required behavior:

- If `state == NULL`, return `-1`.
- If `port == 0`, return `-1`.
- Scan all 32 sockets for a valid socket with that port.
- If found:
  - set `valid = 0`
  - set `port = 0`
  - decrement `count`
  - return `0`
- If not found, return `-1`.

Current implementation does not clear `recv_handler` or `ctx`.

### `udp_send`

Required behavior:

- If `sim == NULL`, return `-1`.
- If `dst_port == 0`, return `-1`.
- If `payload_len > 0 && payload == NULL`, return `-1`.
- If `payload_len > UINT16_MAX - UDP_HDR_LEN`, return `-1`.
- Allocate a packet with capacity `UDP_HDR_LEN + payload_len`.
- If allocation fails, return `-1`.
- Write UDP header at `pkt->data`:
  - `src_port = ns_htons(src_port)`
  - `dst_port = ns_htons(dst_port)`
  - `length = ns_htons(UDP_HDR_LEN + payload_len)`
  - `checksum = 0`
- If payload length is nonzero, copy payload bytes after the UDP header.
- Set `pkt->len = UDP_HDR_LEN + payload_len`.
- Set `pkt->layer = 4`.
- Call `ip_output(sim, src_ip, dst_ip, IPPROTO_UDP, pkt)`.
- If `ip_output` returns `-1`, free `pkt` and return `-1`.
- Otherwise return `0`.

`src_ip` and `dst_ip` are host order because `ip_output` expects host order.

### `udp_receive`

Required behavior:

- If `iface == NULL`, return `-1`.
- If `pkt == NULL`:
  - increment `iface->rx_errors`
  - return `-1`
- If `ctx == NULL`:
  - free `pkt`
  - increment `iface->rx_errors`
  - return `-1`
- Cast `ctx` to `UdpContext *`.
- If `udp_ctx->state == NULL`:
  - free `pkt`
  - increment `iface->rx_errors`
  - return `-1`
- If `pkt->len < UDP_HDR_LEN`:
  - free `pkt`
  - increment `iface->rx_errors`
  - return `-1`
- If `pkt->head == NULL || pkt->data == NULL`:
  - free `pkt`
  - increment `iface->rx_errors`
  - return `-1`
- Verify current bytes are inside packet allocation:
  - `pkt->data >= pkt->head + IP_HDR_LEN`
  - `pkt->data < end`
  - `pkt->len <= end - pkt->data`
- If range check fails:
  - free `pkt`
  - increment `iface->rx_errors`
  - return `-1`
- Read stripped IP header at `pkt->data - IP_HDR_LEN`.
- If `ip_hdr->protocol != IPPROTO_UDP`:
  - free `pkt`
  - increment `iface->rx_errors`
  - return `-1`
- Read UDP header at `pkt->data`.
- Convert source port, destination port, UDP length, and source IP to host
  order.
- If `udp_len < UDP_HDR_LEN`:
  - free `pkt`
  - increment `iface->rx_errors`
  - return `-1`
- If `udp_len > pkt->len`:
  - free `pkt`
  - increment `iface->rx_errors`
  - return `-1`
- Search all 32 sockets for a valid socket bound to destination port.
- If found:
  - strip `UDP_HDR_LEN` bytes
  - if strip fails, free `pkt`, increment `rx_errors`, return `-1`
  - set `pkt->layer = 5`
  - call socket receive handler with source IP, source port, stripped payload,
    and socket context
  - return `0`
- If no socket is found and `udp_ctx->sim != NULL`, return
  `icmp_send_unreach_port(udp_ctx->sim, iface, pkt)`.
- If no socket is found and `udp_ctx->sim == NULL`:
  - free `pkt`
  - increment `iface->rx_dropped`
  - return `-1`

Current implementation note: if `udp_len < pkt->len`, the code does not trim
`pkt->len` to `udp_len`. After a receive hit, stripping the UDP header leaves
the trailing bytes visible to the callback. Tests should capture this current
behavior if it remains intentional.

## Flow Charts

### Send

```text
udp_send(sim, src_ip, dst_ip, src_port, dst_port, payload, payload_len)
  |
  +-- reject NULL/bad inputs
  |
  +-- reject payload too large for 16-bit UDP length
  |
  +-- packet_create(8 + payload_len)
  |
  +-- write UDP header
  +-- copy payload
  +-- pkt->len = 8 + payload_len
  +-- pkt->layer = 4
  |
  +-- ip_output(... IPPROTO_UDP ...)
  |
  +-- ip_output failure: free packet, return -1
  |
  +-- success or pending: return 0
```

### Receive

```text
udp_receive(iface, pkt, udp_ctx)
  |
  +-- reject malformed inputs
  |
  +-- recover stripped IP header at pkt->data - IP_HDR_LEN
  |
  +-- reject if original IP protocol is not UDP
  |
  +-- parse UDP header
  |
  +-- reject invalid UDP length
  |
  +-- lookup destination port in UdpState
        |
        +-- hit:
        |     strip UDP header
        |     pkt->layer = 5
        |     call socket callback
        |     return 0
        |
        +-- miss and simulator exists:
        |     return icmp_send_unreach_port(...)
        |
        +-- miss and simulator missing:
              free packet
              rx_dropped++
              return -1
```

## ACSL Contracts

The contracts belong in `udp.h`. Use literal bounds:

- socket slots: `0 .. 31`
- UDP header bytes: `8`
- IPv4 stripped header bytes: `20`

### Shared Predicates

```c
/*@
    predicate udp_state_count_valid(UdpState *state) =
        0 <= state->count && state->count <= 32;

    predicate udp_socket_slots_valid(UdpState *state) =
        \forall integer i; 0 <= i && i < 32 ==>
            (state->sockets[i].valid == 0 ||
             (state->sockets[i].valid == 1 &&
              state->sockets[i].port != 0 &&
              state->sockets[i].recv_handler != \null));

    predicate udp_state_well_formed(UdpState *state) =
        \valid(state) &&
        udp_state_count_valid(state) &&
        udp_socket_slots_valid(state);

    predicate udp_packet_readable(Packet *pkt) =
        packet_visible_bytes(pkt) &&
        pkt->len >= 8 &&
        pkt->data >= pkt->head + 20;
*/
```

### `udp_init`

```c
/*@
    behavior null:
        assumes state == \null;
        assigns \nothing;

    behavior valid:
        assumes \valid(state);
        assigns state->sockets[0 .. 31],
                state->count;
        ensures state->count == 0;
        ensures \forall integer i; 0 <= i && i < 32 ==>
                state->sockets[i].valid == 0;

    complete behaviors;
    disjoint behaviors;
*/
void udp_init(UdpState *state);
```

### `udp_bind`

```c
/*@
    behavior bad_input:
        assumes state == \null || recv_handler == \null || port == 0;
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes udp_state_well_formed(state);
        assumes recv_handler != \null;
        assumes port != 0;
        assigns state->sockets[0 .. 31],
                state->count;
        ensures \result == 0 || \result == -1;
        ensures \result == 0 ==> state->count == \old(state->count) + 1;
        ensures \result == -1 ==> state->count == \old(state->count);
        ensures udp_state_well_formed(state);

    complete behaviors;
    disjoint behaviors;
*/
int udp_bind(UdpState *state,
             uint16_t port,
             Udp_Recv_Handler recv_handler,
             void *ctx);
```

Additional required proof/test property:

- Duplicate port returns `-1`.
- Full table returns `-1`.
- Successful bind stores port, handler, and context.

### `udp_unbind`

```c
/*@
    behavior bad_input:
        assumes state == \null || port == 0;
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes udp_state_well_formed(state);
        assumes port != 0;
        assigns state->sockets[0 .. 31],
                state->count;
        ensures \result == 0 || \result == -1;
        ensures \result == 0 ==> state->count == \old(state->count) - 1;
        ensures \result == -1 ==> state->count == \old(state->count);

    complete behaviors;
    disjoint behaviors;
*/
int udp_unbind(UdpState *state, uint16_t port);
```

Additional required proof/test property:

- Missing port returns `-1`.
- Successful unbind clears `valid` and `port`.
- Current implementation does not clear handler or context.

### `udp_send`

```c
/*@
    behavior bad_input:
        assumes sim == \null ||
                dst_port == 0 ||
                (payload_len > 0 && payload == \null);
        assigns \nothing;
        ensures \result == -1;

    behavior too_large:
        assumes simulator_well_formed(sim);
        assumes dst_port != 0;
        assumes payload_len == 0 || payload != \null;
        assumes payload_len > 65535 - 8;
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes simulator_well_formed(sim);
        assumes dst_port != 0;
        assumes payload_len <= 65535 - 8;
        assumes payload_len == 0 ||
                \valid_read(payload + (0 .. payload_len - 1));
        assigns sim->sched->eq->events,
                sim->sched->eq->events[0 .. sim->sched->eq->capacity - 1],
                sim->sched->eq->count,
                sim->sched->eq->capacity;
        ensures \result == 0 || \result == -1;

    complete behaviors;
    disjoint behaviors;
*/
int udp_send(Simulator *sim,
             uint32_t src_ip,
             uint32_t dst_ip,
             uint16_t src_port,
             uint16_t dst_port,
             const uint8_t *payload,
             size_t payload_len);
```

Additional required proof/test property:

- Constructed UDP header stores ports and length in network byte order.
- Checksum field is zero.
- Payload bytes are copied exactly.
- On `ip_output` failure, allocated packet is freed.

### `udp_receive`

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

    behavior valid:
        assumes interface_basic_valid(iface);
        assumes pkt != \null;
        assigns iface->rx_errors,
                iface->rx_dropped,
                pkt->data,
                pkt->len,
                pkt->layer;
        ensures \result == 0 || \result == -1;

    complete behaviors;
    disjoint behaviors;
*/
int udp_receive(Interface *iface, Packet *pkt, void *ctx);
```

Additional required proof/test property:

- Malformed non-null packets increment `rx_errors` and are freed.
- Non-UDP original IP protocol increments `rx_errors`.
- UDP length smaller than header increments `rx_errors`.
- UDP length larger than visible packet length increments `rx_errors`.
- Bound destination port strips UDP header and calls socket callback.
- Missing destination port with simulator calls ICMP Port Unreachable.
- Missing destination port without simulator increments `rx_dropped`.

## KLEVA Verification Plan

Minimum KLEVA tests:

1. `udp_init(NULL)` does not crash.
2. `udp_init(valid)` sets `count == 0`.
3. `udp_init(valid)` clears all socket valid bits.
4. `udp_bind` rejects NULL state, NULL handler, and port zero.
5. `udp_bind` rejects duplicate port.
6. `udp_bind` rejects full socket table.
7. Successful bind increments count.
8. Successful bind stores port, handler, and context.
9. `udp_unbind` rejects NULL state and port zero.
10. `udp_unbind` rejects missing port.
11. Successful unbind decrements count.
12. Successful unbind clears valid and port.
13. `udp_send` rejects NULL simulator.
14. `udp_send` rejects destination port zero.
15. `udp_send` rejects NULL payload when payload length is nonzero.
16. `udp_send` accepts NULL payload when payload length is zero.
17. `udp_send` rejects payload too large for UDP length.
18. UDP send header fields are network order.
19. UDP send checksum field is zero.
20. UDP send copies payload exactly.
21. UDP send frees packet on `ip_output` failure.
22. `udp_receive` rejects NULL interface.
23. `udp_receive` with NULL packet increments `rx_errors`.
24. NULL context increments `rx_errors` and frees packet.
25. NULL context state increments `rx_errors` and frees packet.
26. Too-short UDP packet increments `rx_errors`.
27. Null packet head/data increments `rx_errors`.
28. Out-of-bounds UDP bytes increment `rx_errors`.
29. Non-UDP original protocol increments `rx_errors`.
30. UDP length less than 8 increments `rx_errors`.
31. UDP length greater than packet length increments `rx_errors`.
32. Bound port strips UDP header.
33. Bound port sets packet layer to `5`.
34. Bound port calls callback with source IP and source port.
35. Missing port with simulator calls ICMP Port Unreachable.
36. Missing port without simulator increments `rx_dropped`.
37. Current behavior: `udp_len < pkt->len` does not trim trailing bytes.

## Common Mistakes

- Do not store UDP sockets globally in `Simulator`.
- Do not register UDP from inside `ip.c`; stack owner registers it.
- Do not free payload after calling `Udp_Recv_Handler`; callback owns it.
- Do not strip UDP header before missing-port ICMP handling.
- Do not claim UDP checksum validation exists.
- Do not claim receive trims trailing bytes; current code does not.
- Do not call `ip_send` directly from `udp_send`; use `ip_output`.
