# Module 15 - ICMP

**Files:** `src/protocols/icmp.c`, `src/protocols/icmp.h`
**Status:** Implemented core echo and error-message helpers
**Depends on:** `ip`, `packet`, `interface`, `simulator`, `byte_order`

## Concepts First

ICMP is IPv4's control-message protocol.

It is not a transport protocol like TCP or UDP. It is used by IP hosts and
routers to report control information:

- echo request/reply for reachability tests
- destination unreachable
- time exceeded
- fragmentation needed

In this simulator, ICMP is an IPv4 payload protocol. IP validates and strips the
IPv4 header first, then dispatches protocol number `IPPROTO_ICMP` to
`icmp_receive`.

### ICMP Is Registered With IP

IP must stay generic. It must not include `icmp.h` or call ICMP directly.

The stack owner registers ICMP like this:

```c
ip_stack_register_protocol(&ip_stack, IPPROTO_ICMP, icmp_receive, sim);
```

The registered context is `Simulator *sim`, because ICMP send helpers use
`ip_output`.

### Echo Request And Echo Reply

Echo is the familiar ping mechanism:

```text
Host A sends Echo Request to Host B
Host B sends Echo Reply back to Host A
```

The reply keeps the request identifier, sequence number, and payload. It changes
only the ICMP type and checksum, then swaps the original IPv4 source and
destination through `ip_output`.

### ICMP Error Messages

Destination Unreachable and Time Exceeded messages quote part of the original
IPv4 packet:

```text
ICMP error header
  +
original IPv4 header
  +
first 8 bytes of original IPv4 payload
```

This lets the original sender identify which packet caused the error.

The current implementation quotes:

```text
20 bytes original IPv4 header + min(orig payload length, 8)
```

### Do Not Send Errors About Errors

ICMP should not send an ICMP error in response to an ICMP error. Otherwise error
messages can loop forever.

The shared error helper suppresses new errors when the original packet's IPv4
protocol is ICMP and the original ICMP type is:

- Destination Unreachable
- Time Exceeded

For those cases it returns `0` and sends nothing.

### Packet Shape At ICMP Receive

`icmp_receive` is called after IP strips the IPv4 header.

At entry:

```text
pkt->data - IP_HDR_LEN  points at original IPv4 header
pkt->data               points at ICMP header
pkt->len                is ICMP header + ICMP body length
```

The stripped IPv4 header is still readable because `packet_strip` moves the
`data` pointer but does not erase old bytes.

## Purpose

The ICMP module parses received ICMP messages and builds outbound ICMP control
messages.

It provides:

- ICMP constants
- packed ICMP common header
- ICMP receive handler for IP dispatch
- echo request sender
- echo reply sender
- time exceeded sender
- destination unreachable senders
- fragmentation-needed sender
- ICMP checksum helper

It does not:

- register itself with IP
- choose routes directly
- choose Ethernet destination MAC addresses
- own an IP stack
- track ping waiters
- implement all ICMP types/codes
- generate IP-layer errors automatically

## Architecture Boundary

| Responsibility | Owner |
| --- | --- |
| Validate ICMP checksum | ICMP |
| Parse ICMP type/code | ICMP |
| Build Echo Request/Reply | ICMP |
| Build ICMP error payloads | ICMP |
| Register protocol number 1 | IP stack owner |
| Validate and strip IPv4 header | IP |
| Choose source interface and ARP resolution | IP output |
| Prepend Ethernet header | Ethernet |

ICMP sends through `ip_output`. It should not call Ethernet directly.

## Data Model

### Constants

```c
#define ICMP_ECHO_REPLY        0
#define ICMP_DEST_UNREACH      3
#define ICMP_ECHO_REQUEST      8
#define ICMP_TIME_EXCEEDED     11

#define ICMP_CODE_NET_UNREACH  0
#define ICMP_CODE_HOST_UNREACH 1
#define ICMP_CODE_PROTO_UNREACH 2
#define ICMP_CODE_PORT_UNREACH 3
#define ICMP_CODE_FRAG_NEEDED  4
#define ICMP_CODE_TTL_EXCEEDED 0

#define ICMP_HDR_LEN           8
#define ICMP_ORIG_QUOTE_LEN    28
```

`ICMP_ORIG_QUOTE_LEN` is the traditional IPv4 header plus first 8 payload bytes.
The current code may quote fewer than 28 bytes if the original payload is shorter
than 8 bytes.

### `IcmpHeader`

```c
typedef struct __attribute__((packed)) IcmpHeader {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
} IcmpHeader;
```

Field meanings:

| Field | Echo meaning | Error-message meaning |
| --- | --- | --- |
| `type` | Echo request/reply type | Error type |
| `code` | Usually `0` | Error code |
| `checksum` | ICMP checksum | ICMP checksum |
| `id` | Echo identifier | High 16 bits of rest-of-header, usually zero |
| `seq` | Echo sequence | Low 16 bits, or next-hop MTU for fragmentation-needed |

All multi-byte ICMP fields are stored in network byte order.

## Ownership And Lifetime

`icmp_receive` consumes every non-NULL packet it is given.

After `pkt != NULL`, each path must do one of:

- pass the packet to `icmp_send_echo_reply`
- free it with `packet_free`

`icmp_send_echo_reply` consumes `req_pkt`. It frees `req_pkt` on every path
after receiving a non-NULL request packet.

`icmp_send_echo_request` allocates a new packet. If `ip_output` returns `-1`,
it frees that packet. If `ip_output` returns `0` or positive, the packet is not
freed by ICMP.

`icmp_send_error` allocates a new error packet. If `ip_output` returns `-1`, it
frees that packet. If `ip_output` returns `0` or positive, it does not free it.

Current project caveat: `ip_output` may return `0` because a packet was queued
for ARP pending. Direct-send lower layers currently have broader packet
ownership gaps documented in IP/Ethernet specs.

ICMP error helpers read `orig_pkt`; they do not free it.

## Public API

```c
int icmp_receive(Interface *iface, Packet *pkt, void *ctx);

int icmp_send_echo_request(Simulator     *sim,
                           uint32_t       src_ip,
                           uint32_t       dst_ip,
                           uint16_t       id,
                           uint16_t       seq,
                           const uint8_t *payload,
                           size_t         payload_len);

int icmp_send_echo_reply(Simulator *sim,
                         Interface *iface,
                         Packet    *req_pkt);

int icmp_send_time_exceeded(Simulator *sim,
                            Interface *iface,
                            Packet    *orig_pkt);

int icmp_send_unreach_net(Simulator *sim,
                          Interface *iface,
                          Packet    *orig_pkt);

int icmp_send_unreach_host(Simulator *sim,
                           Interface *iface,
                           Packet    *orig_pkt);

int icmp_send_unreach_proto(Simulator *sim,
                            Interface *iface,
                            Packet    *orig_pkt);

int icmp_send_unreach_port(Simulator *sim,
                           Interface *iface,
                           Packet    *orig_pkt);

int icmp_send_frag_needed(Simulator *sim,
                          Interface *iface,
                          Packet    *orig_pkt,
                          uint16_t   next_hop_mtu);

uint16_t icmp_checksum(const void *data, size_t len);
```

## Function Behavior

### `icmp_receive`

Required behavior:

- Cast `ctx` to `Simulator *`.
- If `iface == NULL`, return `-1`.
- If `pkt == NULL`:
  - increment `iface->rx_errors`
  - return `-1`
- If `pkt->len < ICMP_HDR_LEN`:
  - increment `iface->rx_errors`
  - free `pkt`
  - return `-1`
- If `pkt->head == NULL` or `pkt->data == NULL`:
  - increment `iface->rx_errors`
  - free `pkt`
  - return `-1`
- Compute packet allocation end:

```c
end = pkt->head + PKT_HEADROOM + pkt->capacity
```

- If `pkt->data < pkt->head + IP_HDR_LEN` or `pkt->data >= end`:
  - increment `iface->rx_errors`
  - free `pkt`
  - return `-1`
- If `pkt->len > end - pkt->data`:
  - increment `iface->rx_errors`
  - free `pkt`
  - return `-1`
- If `icmp_checksum(pkt->data, pkt->len) != 0`:
  - increment `iface->rx_errors`
  - free `pkt`
  - return `-1`
- Interpret `pkt->data` as `IcmpHeader`.
- Dispatch:
  - Echo Request code `0`: call `icmp_send_echo_reply(sim, iface, pkt)` and
    return its result.
  - Echo Reply code `0`: free `pkt`, return `0`.
  - Destination Unreachable codes `0..4`: free `pkt`, return `0`.
  - Time Exceeded code `0`: free `pkt`, return `0`.
  - Unsupported type or code: increment `iface->rx_dropped`, free `pkt`,
    return `-1`.

Supported-but-consumed messages do not increment `rx_dropped`.

### `icmp_send_echo_request`

Required behavior:

- If `sim == NULL`, return `-1`.
- If `payload_len > 0 && payload == NULL`, return `-1`.
- Allocate a packet with capacity `ICMP_HDR_LEN + payload_len`.
- If allocation fails, return `-1`.
- Interpret `pkt->data` as `IcmpHeader`.
- Fill:
  - `type = ICMP_ECHO_REQUEST`
  - `code = 0`
  - `id = ns_htons(id)`
  - `seq = ns_htons(seq)`
  - `checksum = 0`
- Set `pkt->len = ICMP_HDR_LEN + payload_len`.
- Set `pkt->layer = 4`.
- If payload length is nonzero, copy payload bytes after the ICMP header.
- Compute checksum over the whole ICMP message.
- Call `ip_output(sim, src_ip, dst_ip, IPPROTO_ICMP, pkt)`.
- If `ip_output` returns `-1`, free `pkt`.
- Return the `ip_output` result.

`src_ip` and `dst_ip` are host order because `ip_output` expects host order.

### `icmp_send_echo_reply`

Required behavior:

- If `sim == NULL`, return `-1`.
- If `iface == NULL`, return `-1`.
- If `req_pkt == NULL`, return `-1`.
- If `req_pkt->len < ICMP_HDR_LEN`:
  - increment `iface->tx_errors`
  - free `req_pkt`
  - return `-1`
- If `req_pkt->head == NULL` or `req_pkt->data == NULL`:
  - increment `iface->tx_errors`
  - free `req_pkt`
  - return `-1`
- If `req_pkt->data < req_pkt->head + IP_HDR_LEN`:
  - increment `iface->tx_errors`
  - free `req_pkt`
  - return `-1`
- If `req_pkt->data >= end` or `req_pkt->len > end - req_pkt->data`:
  - increment `iface->tx_errors`
  - free `req_pkt`
  - return `-1`
- If request ICMP type/code is not Echo Request/code `0`:
  - increment `iface->tx_errors`
  - free `req_pkt`
  - return `-1`
- Read original IPv4 header at `req_pkt->data - IP_HDR_LEN`.
- Allocate a reply packet with capacity `req_pkt->len`.
- If allocation fails:
  - increment `iface->tx_errors`
  - free `req_pkt`
  - return `-1`
- Copy the request ICMP bytes into the reply packet.
- Set reply packet length equal to request ICMP length.
- Set reply packet layer to `4`.
- Change reply ICMP type to `ICMP_ECHO_REPLY`.
- Set checksum to zero.
- Recompute checksum over the reply ICMP message.
- Set source IP to original IPv4 destination converted to host order.
- Set destination IP to original IPv4 source converted to host order.
- Call `ip_output(sim, src_ip, dst_ip, IPPROTO_ICMP, reply_pkt)`.
- If `ip_output` returns `-1`:
  - increment `iface->tx_errors`
  - free `reply_pkt`
  - free `req_pkt`
  - return `-1`
- Free `req_pkt`.
- Return the `ip_output` result.

### `icmp_send_error`

This helper is static inside `icmp.c`. Public error helper functions all call
it.

Required behavior:

- If `sim == NULL`, `iface == NULL`, or `orig_pkt == NULL`, return `-1`.
- If `orig_pkt->head == NULL` or `orig_pkt->data == NULL`:
  - increment `iface->tx_errors`
  - return `-1`
- If `orig_pkt->data < orig_pkt->head + IP_HDR_LEN`:
  - increment `iface->tx_errors`
  - return `-1`
- If current payload range is outside the packet allocation:
  - increment `iface->tx_errors`
  - return `-1`
- Read original IP header at `orig_pkt->data - IP_HDR_LEN`.
- If original protocol is ICMP:
  - if original payload length is less than `ICMP_HDR_LEN`, return `0`
  - if original ICMP type is Destination Unreachable or Time Exceeded, return
    `0`
- Compute quoted payload length as `min(orig_pkt->len, 8)`.
- Compute error packet length as `ICMP_HDR_LEN + IP_HDR_LEN + quote_payload`.
- Allocate error packet.
- If allocation fails:
  - increment `iface->tx_errors`
  - return `-1`
- Fill ICMP error header:
  - `type` and `code` from caller
  - checksum initially `0`
  - `id = 0`
  - `seq = ns_htons(next_hop_mtu)` only for fragmentation-needed
  - otherwise `seq = 0`
- Copy original IPv4 header into quote.
- Copy first `min(orig_pkt->len, 8)` bytes of original payload into quote.
- Compute checksum.
- Use source IP `ns_ntohl(iface->ip_addr)`.
- Use destination IP `ns_ntohl(orig_ip->src_ip)`.
- Call `ip_output(sim, src_ip, dst_ip, IPPROTO_ICMP, err_pkt)`.
- If `ip_output` returns `-1`:
  - increment `iface->tx_errors`
  - free `err_pkt`
  - return `-1`
- Return the `ip_output` result.

### Public Error Helpers

Each public helper calls `icmp_send_error` with fixed type/code:

| Function | Type | Code | Extra |
| --- | ---: | ---: | --- |
| `icmp_send_time_exceeded` | `11` | `0` | none |
| `icmp_send_unreach_net` | `3` | `0` | none |
| `icmp_send_unreach_host` | `3` | `1` | none |
| `icmp_send_unreach_proto` | `3` | `2` | none |
| `icmp_send_unreach_port` | `3` | `3` | none |
| `icmp_send_frag_needed` | `3` | `4` | stores next-hop MTU in `seq` |

### `icmp_checksum`

Required behavior:

- If `data == NULL`, return `0xFFFF`.
- If `len == 0`, return `0xFFFF`.
- Treat the ICMP message as 16-bit big-endian words.
- If one trailing byte remains, treat it as the high byte of the final word.
- Fold carries.
- Return one's complement in network byte order.

Incoming validation accepts a message only when:

```text
icmp_checksum(data, len) == 0
```

## Flow Charts

### Receive

```text
icmp_receive(iface, pkt, sim)
  |
  +-- reject NULL iface
  |
  +-- reject NULL/malformed pkt:
  |     rx_errors++, free if non-null
  |
  +-- checksum invalid:
  |     rx_errors++, free pkt, return -1
  |
  +-- Echo Request code 0:
  |     return icmp_send_echo_reply(sim, iface, pkt)
  |
  +-- Echo Reply code 0:
  |     free pkt, return 0
  |
  +-- Destination Unreachable code 0..4:
  |     free pkt, return 0
  |
  +-- Time Exceeded code 0:
  |     free pkt, return 0
  |
  +-- unsupported:
        rx_dropped++, free pkt, return -1
```

### Echo Reply

```text
icmp_send_echo_reply(sim, iface, req_pkt)
  |
  +-- validate request packet shape
  |
  +-- validate type == Echo Request and code == 0
  |
  +-- req_ip = req_pkt->data - IP_HDR_LEN
  |
  +-- allocate reply packet
  |
  +-- copy ICMP request bytes
  |
  +-- change type to Echo Reply
  +-- recompute checksum
  |
  +-- src_ip = ntohl(req_ip->dst_ip)
  +-- dst_ip = ntohl(req_ip->src_ip)
  |
  +-- ip_output(sim, src_ip, dst_ip, IPPROTO_ICMP, reply_pkt)
  |
  +-- free req_pkt
```

### Error Message

```text
icmp_send_error(sim, iface, orig_pkt, type, code, mtu)
  |
  +-- validate original packet shape
  |
  +-- suppress ICMP error in response to ICMP error
  |
  +-- quote original IP header + first 8 payload bytes
  |
  +-- build ICMP error header
  |
  +-- checksum
  |
  +-- ip_output back to original source
```

## ACSL Contracts

The contracts belong in `icmp.h`. Full proof of ownership/free behavior is
harder than shape/counter checks, so KLEVA tests should cover ownership cases.

### Shared Predicates

```c
/*@
    predicate icmp_message_readable(Packet *pkt) =
        packet_visible_bytes(pkt) &&
        pkt->len >= 8;

    predicate icmp_has_stripped_ip(Packet *pkt) =
        packet_visible_bytes(pkt) &&
        pkt->data >= pkt->head + 20;

    predicate icmp_buffer_bounds_ok(Packet *pkt) =
        packet_visible_bytes(pkt) &&
        pkt->data < pkt->head + PKT_HEADROOM + pkt->capacity &&
        pkt->len <=
            (size_t)((pkt->head + PKT_HEADROOM + pkt->capacity) - pkt->data);
*/
```

### `icmp_receive`

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

    behavior valid_shape:
        assumes interface_basic_valid(iface);
        assumes pkt != \null;
        assigns iface->rx_errors,
                iface->rx_dropped;
        ensures \result == 0 || \result == -1;

    complete behaviors;
    disjoint behaviors;
*/
int icmp_receive(Interface *iface, Packet *pkt, void *ctx);
```

Additional required proof/test property:

- Malformed non-null packets increment `rx_errors` and are freed.
- Unsupported type/code increments `rx_dropped` and frees the packet.
- Echo Request/code 0 transfers packet to `icmp_send_echo_reply`.
- Supported consumed messages free the packet and return `0`.

### `icmp_send_echo_request`

```c
/*@
    behavior bad_input:
        assumes sim == \null || (payload_len > 0 && payload == \null);
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes simulator_well_formed(sim);
        assumes payload_len == 0 ||
                \valid_read(payload + (0 .. payload_len - 1));
        assigns sim->sched->eq->events,
                sim->sched->eq->events[0 .. sim->sched->eq->capacity - 1],
                sim->sched->eq->count,
                sim->sched->eq->capacity;
        ensures \result >= 0 || \result == -1;

    complete behaviors;
    disjoint behaviors;
*/
int icmp_send_echo_request(Simulator *sim,
                           uint32_t src_ip,
                           uint32_t dst_ip,
                           uint16_t id,
                           uint16_t seq,
                           const uint8_t *payload,
                           size_t payload_len);
```

Additional required proof/test property:

- Constructed message has type Echo Request and code `0`.
- Identifier and sequence are stored in network byte order.
- Payload bytes are copied exactly.
- Checksum validates the constructed message.
- On `ip_output` failure, the new packet is freed.

### `icmp_send_echo_reply`

```c
/*@
    behavior bad_input:
        assumes sim == \null || iface == \null || req_pkt == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes simulator_well_formed(sim);
        assumes interface_basic_valid(iface);
        assumes packet_visible_bytes(req_pkt);
        assigns iface->tx_errors,
                sim->sched->eq->events,
                sim->sched->eq->events[0 .. sim->sched->eq->capacity - 1],
                sim->sched->eq->count,
                sim->sched->eq->capacity;
        ensures \result >= 0 || \result == -1;

    complete behaviors;
    disjoint behaviors;
*/
int icmp_send_echo_reply(Simulator *sim,
                         Interface *iface,
                         Packet *req_pkt);
```

Additional required proof/test property:

- Invalid request packet increments `tx_errors` and frees `req_pkt`.
- Reply copies request id, seq, and payload.
- Reply source IP is original destination IP.
- Reply destination IP is original source IP.
- Reply checksum validates.
- `req_pkt` is freed on every non-null path.

### Error Helpers

```c
/*@
    behavior bad_input:
        assumes sim == \null || iface == \null || orig_pkt == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes simulator_well_formed(sim);
        assumes interface_basic_valid(iface);
        assumes packet_visible_bytes(orig_pkt);
        assigns iface->tx_errors,
                sim->sched->eq->events,
                sim->sched->eq->events[0 .. sim->sched->eq->capacity - 1],
                sim->sched->eq->count,
                sim->sched->eq->capacity;
        ensures \result >= 0 || \result == -1;
*/
```

Apply that shape to:

- `icmp_send_time_exceeded`
- `icmp_send_unreach_net`
- `icmp_send_unreach_host`
- `icmp_send_unreach_proto`
- `icmp_send_unreach_port`
- `icmp_send_frag_needed`

Additional required proof/test property:

- Error packet quotes original IPv4 header plus up to first 8 payload bytes.
- ICMP errors in response to ICMP errors are suppressed with return `0`.
- Fragmentation-needed stores `next_hop_mtu` in network byte order.

### `icmp_checksum`

```c
/*@
    behavior null:
        assumes data == \null;
        assigns \nothing;
        ensures \result == 0xFFFF;

    behavior empty:
        assumes data != \null && len == 0;
        assigns \nothing;
        ensures \result == 0xFFFF;

    behavior valid:
        assumes data != \null && len > 0;
        assumes \valid_read(((const uint8_t *)data) + (0 .. len - 1));
        assigns \nothing;

    complete behaviors;
    disjoint behaviors;
*/
uint16_t icmp_checksum(const void *data, size_t len);
```

## KLEVA Verification Plan

Minimum KLEVA tests:

1. `icmp_checksum(NULL, len)` returns `0xFFFF`.
2. `icmp_checksum(data, 0)` returns `0xFFFF`.
3. Checksum handles even-length messages.
4. Checksum handles odd trailing byte as high byte.
5. Constructed checksum validates to zero when recomputed.
6. `icmp_receive(NULL, pkt, ctx)` returns `-1`.
7. `icmp_receive(iface, NULL, ctx)` increments `rx_errors`.
8. Too-short packet increments `rx_errors` and is freed.
9. Null packet head/data increments `rx_errors` and is freed.
10. Missing stripped IP header increments `rx_errors` and is freed.
11. Out-of-bounds message range increments `rx_errors` and is freed.
12. Bad ICMP checksum increments `rx_errors` and is freed.
13. Echo Request/code 0 calls echo reply helper.
14. Echo Reply/code 0 is consumed and returns `0`.
15. Destination Unreachable codes `0..4` are consumed and return `0`.
16. Time Exceeded/code 0 is consumed and returns `0`.
17. Unsupported ICMP type increments `rx_dropped`.
18. Supported type with unsupported code increments `rx_dropped`.
19. Echo request sender rejects NULL simulator.
20. Echo request sender rejects NULL payload when payload length is nonzero.
21. Echo request sender accepts NULL payload when payload length is zero.
22. Echo request construction stores type/code/id/seq correctly.
23. Echo request payload is copied exactly.
24. Echo request frees packet on `ip_output` failure.
25. Echo reply rejects invalid request shape and increments `tx_errors`.
26. Echo reply rejects non-echo-request packet.
27. Echo reply copies id, seq, and payload.
28. Echo reply swaps original IPv4 source/destination.
29. Echo reply frees request packet.
30. Error helpers reject NULL inputs.
31. Error helper rejects malformed original packet and increments `tx_errors`.
32. Error helper suppresses ICMP error for ICMP Destination Unreachable.
33. Error helper suppresses ICMP error for ICMP Time Exceeded.
34. Error helper quotes original IPv4 header.
35. Error helper quotes at most first 8 bytes of original payload.
36. Fragmentation-needed stores MTU in `seq`.
37. Error helper frees error packet on `ip_output` failure.

## Common Mistakes

- Do not register ICMP inside `ip.c`; stack owner registers it.
- Do not include Ethernet or ARP logic in ICMP.
- Do not compute ICMP checksum with an IPv4 pseudo-header.
- Do not send ICMP errors in response to ICMP errors.
- Do not forget that Echo Reply consumes the request packet.
- Do not assume `ip_output` positive-only success; `0` can mean ARP pending.
- Do not increment `rx_dropped` for supported ICMP messages that are consumed.
