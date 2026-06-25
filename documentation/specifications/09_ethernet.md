# Module 09 - Ethernet

**Files:** `src/protocols/ethernet.c`, `src/protocols/ethernet.h`
**Status:** Implemented
**Depends on:** `byte_order`, `packet`, `interface`, `simulator`, `event`,
`link`

## Concepts First

Ethernet is the layer-2 framing protocol used by this simulator.

Upper layers such as ARP and IPv4 produce payload bytes. Ethernet wraps those
bytes with a header that says:

```text
who should receive this frame
who sent this frame
what protocol is inside the frame
```

The Ethernet module is responsible for adding that header on send and removing
that header on receive.

### Ethernet Frame Header

The simulator uses the classic Ethernet II header:

```text
offset  size  field
0       6     destination MAC
6       6     source MAC
12      2     EtherType
14            payload begins
```

The header is exactly 14 bytes:

```c
#define ETH_HDR_LEN 14
#define ETH_ALEN    6
```

The C struct is packed so the compiler does not insert padding:

```c
typedef struct __attribute__((packed)) EthernetHeader {
    uint8_t  dst_mac[6];
    uint8_t  src_mac[6];
    uint16_t ethertype;
} EthernetHeader;
```

Without `packed`, the struct might not match the wire layout.

### MAC Address Filtering

On receive, Ethernet accepts a frame only if the destination MAC is:

- exactly this interface's MAC address, or
- the broadcast address `FF:FF:FF:FF:FF:FF`

Otherwise the frame is dropped with return value `1`.

The comparison must check all six bytes. Checking only the first byte is not a
valid Ethernet destination test.

### EtherType

EtherType says what protocol is inside the Ethernet payload.

Common values in this simulator:

```c
#define ETHERTYPE_IPV4 0x0800
#define ETHERTYPE_ARP  0x0806
#define ETHERTYPE_IPV6 0x86DD
```

The EtherType field is stored in network byte order inside the frame. The send
path uses `ns_htons`; the receive path uses `ns_ntohs`.

The value passed to upper layers is host order.

### Send Path Versus Receive Path

Send path:

```text
payload packet
  |
  +-- prepend Ethernet header
  |
  +-- update transmit stats
  |
  +-- pass framed packet to link_transmit
```

Receive path:

```text
framed packet
  |
  +-- validate destination MAC
  |
  +-- extract EtherType
  |
  +-- strip Ethernet header
  |
  +-- update receive stats
  |
  +-- hand payload to interface receive handler
```

### Event Callback Boundary

`ethernet_receive` only validates and strips the frame.

`ethernet_receive_event` is the scheduler callback that extracts the destination
interface and packet from an event, calls `ethernet_receive`, and then calls the
interface's upper-layer receive handler if the frame was accepted.

This split matters:

- `ethernet_receive` is a packet transformation function.
- `ethernet_receive_event` is event dispatch glue.

## Purpose

The Ethernet module implements Ethernet II send and receive behavior.

It provides:

- Ethernet constants
- packed Ethernet header layout
- broadcast MAC constant
- send function that prepends a header and schedules link delivery
- receive function that validates and strips a header
- event callback for scheduled packet delivery

It does not:

- learn MAC addresses
- flood frames
- switch frames
- implement VLAN tags
- implement multicast filtering
- own packets after handing them to link/scheduler paths
- decide ARP or IP behavior after stripping the header

## Architecture Boundary

| Responsibility | Owner |
| --- | --- |
| Build Ethernet header | Ethernet |
| Convert EtherType to/from network byte order | Ethernet |
| Check destination MAC on receive | Ethernet |
| Strip Ethernet header | Ethernet |
| Schedule delayed delivery | Link, called by Ethernet send |
| Execute receive event | Scheduler |
| Deliver accepted payload upward | Ethernet event via `iface->rx_handler` |
| Interpret ARP/IP payload | ARP/IP modules |
| Learn or forward by MAC table | Switch |

Ethernet should not include ARP, IP, UDP, TCP, or switch logic.

## Data Model

### Constants

```c
#define ETH_HDR_LEN      14
#define ETH_ALEN         6
#define ETHERTYPE_IPV4   0x0800
#define ETHERTYPE_ARP    0x0806
#define ETHERTYPE_IPV6   0x86DD
extern const uint8_t ETH_BROADCAST[6];
```

`ETH_BROADCAST` is:

```text
FF FF FF FF FF FF
```

### `EthernetHeader`

```c
typedef struct __attribute__((packed)) EthernetHeader {
    uint8_t  dst_mac[6];
    uint8_t  src_mac[6];
    uint16_t ethertype;
} EthernetHeader;
```

Field meanings:

| Field | Meaning |
| --- | --- |
| `dst_mac` | Destination MAC address. |
| `src_mac` | Source MAC address. |
| `ethertype` | Payload protocol, stored in network byte order. |

## Ownership And Lifetime

`ethernet_send` does not allocate the packet. The caller passes a `Packet *`.

`ethernet_send` allocates a temporary `EthernetHeader`, fills it, prepends it
into the packet, then frees the temporary header. The packet owns the copied
header bytes after `packet_prepend` succeeds.

`link_transmit` clones the framed packet. Ethernet does not free the caller's
packet after calling `link_transmit`.

`ethernet_receive` mutates the frame in place by stripping the Ethernet header.
It does not free the packet.

`ethernet_receive_event` does not free the event or packet. The scheduler owns
event record cleanup. Packet ownership after receive belongs to the upper layer
or event delivery path.

## Public API

```c
void ethernet_receive_event(const Event *e, void *ctx);

int  ethernet_send(Simulator    *sim,
                   Interface    *iface,
                   const uint8_t dst_mac[6],
                   uint16_t      ethertype,
                   Packet       *payload);

int  ethernet_receive(Interface *iface,
                      Packet    *frame,
                      uint16_t  *out_ethertype);
```

## Function Behavior

### `ethernet_send`

Required behavior:

- If `sim == NULL`, return `-1`.
- If `iface == NULL`, return `-1`.
- If `dst_mac == NULL`, return `-1`.
- If `payload == NULL`, return `-1`.
- If `iface->state == IFACE_ERR_DISABLED`, return `-1`.
- Allocate a temporary `EthernetHeader`.
- If allocation fails:
  - increment `iface->tx_errors`
  - set `iface->last_error_time = simulator_now(sim)`
  - return `-1`
- Copy six destination MAC bytes from `dst_mac`.
- Copy six source MAC bytes from `iface->mac`.
- Store `ethertype` in network byte order with `ns_htons`.
- Prepend the header with `packet_prepend(payload, eth_hdr, ETH_HDR_LEN)`.
- If prepend fails:
  - free the temporary header
  - increment `iface->tx_errors`
  - set `iface->last_error_time = simulator_now(sim)`
  - return `-1`
- Free the temporary header.
- Add the framed packet length to `iface->tx_bytes`.
- Set `iface->last_tx_time = simulator_now(sim)`.
- Set `payload->layer = 2`.
- Call:

```c
link_transmit(iface->link,
              payload,
              iface,
              sim->sched,
              simulator_now(sim),
              ethernet_receive_event,
              sim)
```

- Return the result from `link_transmit`.

`link_transmit` returns `1` for scheduled delivery and `-1` for failure. So
`ethernet_send` currently returns `1` on successful scheduling, not `0`.

### `ethernet_receive`

Required behavior:

- If `iface == NULL`, return `-1`.
- If `frame == NULL`, return `-1`.
- If `out_ethertype == NULL`, return `-1`.
- If `frame->len < ETH_HDR_LEN`, return `-1`.
- If `iface->state == IFACE_ERR_DISABLED`, return `-1`.
- Interpret `frame->data` as an `EthernetHeader`.
- If destination MAC is neither `iface->mac` nor `ETH_BROADCAST`, return `1`.
- Convert header EtherType from network to host order and store it in
  `*out_ethertype`.
- Strip `ETH_HDR_LEN` bytes from the packet.
- If strip fails, return `-1`.
- Add the post-strip payload length to `iface->rx_bytes`.
- Set `frame->layer = 3`.
- Return `0`.

Return codes:

| Return | Meaning |
| --- | --- |
| `0` | Accepted and stripped. |
| `1` | Dropped because destination MAC is not ours and not broadcast. |
| `-1` | Bad input, disabled interface, too-short frame, or strip failure. |

### `ethernet_receive_event`

Required behavior:

- Ignore `ctx`.
- Read destination interface from `e->dst_device`.
- Read packet from `e->packet`.
- If either is `NULL`, return immediately.
- Call `ethernet_receive(iface, frame, &ethertype)`.
- If receive returns `0`:
  - set `iface->last_rx_time = e->timestamp`
  - if `iface->rx_handler != NULL`, call it with:

```c
iface->rx_handler(iface, frame, ethertype, iface->handler_ctx)
```

- If receive returns `1`, increment `iface->rx_dropped`.
- If receive returns `-1`, increment `iface->rx_errors` and set
  `iface->last_error_time = e->timestamp`.

The function assumes `e` itself is valid. It does not check `e == NULL`.

## Flow Charts

### Send

```text
ethernet_send(sim, iface, dst_mac, ethertype, payload)
  |
  +-- reject NULL inputs
  |
  +-- reject IFACE_ERR_DISABLED
  |
  +-- allocate EthernetHeader
  |
  +-- fill dst MAC, src MAC, network-order EtherType
  |
  +-- packet_prepend(payload, header, 14)
  |
  +-- failure:
  |     free header
  |     tx_errors++
  |     last_error_time = simulator_now(sim)
  |     return -1
  |
  +-- free temporary header
  |
  +-- tx_bytes += payload->len
  +-- last_tx_time = simulator_now(sim)
  +-- payload->layer = 2
  |
  +-- link_transmit(... ethernet_receive_event, sim)
  |
  +-- return link_transmit result
```

### Receive

```text
ethernet_receive(iface, frame, out_ethertype)
  |
  +-- reject NULL inputs or short frame
  |
  +-- reject IFACE_ERR_DISABLED
  |
  +-- eth_hdr = frame->data
  |
  +-- dst MAC not iface MAC and not broadcast:
  |     return 1
  |
  +-- *out_ethertype = ntohs(eth_hdr->ethertype)
  |
  +-- packet_strip(frame, 14)
  |
  +-- rx_bytes += frame->len
  +-- frame->layer = 3
  |
  +-- return 0
```

### Receive Event

```text
ethernet_receive_event(e, ctx)
  |
  +-- iface = e->dst_device
  +-- frame = e->packet
  |
  +-- missing iface/frame: return
  |
  +-- result = ethernet_receive(iface, frame, &ethertype)
  |
  +-- result == 0:
  |     last_rx_time = e->timestamp
  |     if rx_handler exists, call it
  |
  +-- result == 1:
  |     rx_dropped++
  |
  +-- result == -1:
        rx_errors++
        last_error_time = e->timestamp
```

## ACSL Contracts

The contracts belong in `ethernet.h`. The key properties are frame length
changes, byte-order conversion, and six-byte MAC filtering.

### Shared Predicates

```c
/*@
    predicate eth_mac_equal{L}(uint8_t *a, uint8_t *b) =
        \valid_read(a + (0 .. 5)) &&
        \valid_read(b + (0 .. 5)) &&
        a[0] == b[0] &&
        a[1] == b[1] &&
        a[2] == b[2] &&
        a[3] == b[3] &&
        a[4] == b[4] &&
        a[5] == b[5];

    predicate eth_frame_readable(Packet *frame) =
        packet_visible_bytes(frame) &&
        frame->len >= 14;
*/
```

### `ethernet_send`

```c
/*@
    behavior null:
        assumes sim == \null || iface == \null ||
                dst_mac == \null || payload == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior disabled:
        assumes simulator_well_formed(sim);
        assumes interface_basic_valid(iface);
        assumes \valid_read(dst_mac + (0 .. 5));
        assumes packet_layout(payload);
        assumes iface->state == IFACE_ERR_DISABLED;
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes simulator_well_formed(sim);
        assumes interface_basic_valid(iface);
        assumes \valid_read(dst_mac + (0 .. 5));
        assumes packet_layout(payload);
        assumes iface->state != IFACE_ERR_DISABLED;
        assigns payload->data,
                payload->len,
                payload->layer,
                payload->head[0 .. PKT_HEADROOM + payload->capacity - 1],
                iface->tx_bytes,
                iface->tx_errors,
                iface->last_tx_time,
                iface->last_error_time,
                sim->sched->eq->events,
                sim->sched->eq->events[0 .. sim->sched->eq->capacity - 1],
                sim->sched->eq->count,
                sim->sched->eq->capacity;
        ensures \result == 1 || \result == -1;
        ensures \result == 1 ==> payload->len == \old(payload->len) + 14;
        ensures \result == 1 ==> payload->layer == 2;

    complete behaviors;
    disjoint behaviors;
*/
int ethernet_send(Simulator *sim,
                  Interface *iface,
                  const uint8_t dst_mac[6],
                  uint16_t ethertype,
                  Packet *payload);
```

Additional required proof/test property:

- On successful prepend, the first six bytes of `payload->data` equal
  `dst_mac[0 .. 5]`.
- On successful prepend, bytes `6..11` equal `iface->mac[0 .. 5]`.
- On successful prepend, bytes `12..13` store `ethertype` in network byte
  order.
- If temporary header allocation or prepend fails, `tx_errors` increments and
  `last_error_time` is updated.

### `ethernet_receive`

```c
/*@
    behavior null_or_short:
        assumes iface == \null || frame == \null ||
                out_ethertype == \null ||
                (frame != \null && frame->len < 14);
        assigns \nothing;
        ensures \result == -1;

    behavior disabled:
        assumes interface_basic_valid(iface);
        assumes eth_frame_readable(frame);
        assumes \valid(out_ethertype);
        assumes iface->state == IFACE_ERR_DISABLED;
        assigns \nothing;
        ensures \result == -1;

    behavior drop:
        assumes interface_basic_valid(iface);
        assumes eth_frame_readable(frame);
        assumes \valid(out_ethertype);
        assumes iface->state != IFACE_ERR_DISABLED;
        assumes !eth_mac_equal(((EthernetHeader *)frame->data)->dst_mac, iface->mac);
        assumes !eth_mac_equal(((EthernetHeader *)frame->data)->dst_mac, (uint8_t *)ETH_BROADCAST);
        assigns \nothing;
        ensures \result == 1;

    behavior accepted:
        assumes interface_basic_valid(iface);
        assumes eth_frame_readable(frame);
        assumes \valid(out_ethertype);
        assumes iface->state != IFACE_ERR_DISABLED;
        assumes eth_mac_equal(((EthernetHeader *)frame->data)->dst_mac, iface->mac) ||
                eth_mac_equal(((EthernetHeader *)frame->data)->dst_mac, (uint8_t *)ETH_BROADCAST);
        assigns frame->data,
                frame->len,
                frame->layer,
                *out_ethertype,
                iface->rx_bytes;
        ensures \result == 0 || \result == -1;
        ensures \result == 0 ==> frame->len == \old(frame->len) - 14;
        ensures \result == 0 ==> frame->data == \old(frame->data) + 14;
        ensures \result == 0 ==>
                *out_ethertype == ((\old(frame->data[12]) << 8) |
                                   \old(frame->data[13]));
        ensures \result == 0 ==> frame->layer == 3;

    complete behaviors;
    disjoint behaviors;
*/
int ethernet_receive(Interface *iface,
                     Packet *frame,
                     uint16_t *out_ethertype);
```

### `ethernet_receive_event`

```c
/*@
    behavior missing_payload:
        assumes \valid_read(e);
        assumes e->dst_device == \null || e->packet == \null;
        assigns \nothing;

    behavior valid_event:
        assumes \valid_read(e);
        assumes e->dst_device != \null && e->packet != \null;
        assumes interface_basic_valid((Interface *)e->dst_device);
        assumes packet_visible_bytes((Packet *)e->packet);
        assigns ((Interface *)e->dst_device)->last_rx_time,
                ((Interface *)e->dst_device)->last_error_time,
                ((Interface *)e->dst_device)->rx_dropped,
                ((Interface *)e->dst_device)->rx_errors,
                ((Interface *)e->dst_device)->rx_bytes,
                ((Packet *)e->packet)->data,
                ((Packet *)e->packet)->len,
                ((Packet *)e->packet)->layer;

    complete behaviors;
    disjoint behaviors;
*/
void ethernet_receive_event(const Event *e, void *ctx);
```

The event callback is hard to specify completely without conditional validity
predicates for opaque event pointers. KLEVA tests should cover the concrete
cases below.

## KLEVA Verification Plan

Minimum KLEVA tests:

1. `ethernet_send` rejects NULL simulator, interface, destination MAC, or
   payload.
2. `ethernet_send` rejects `IFACE_ERR_DISABLED`.
3. Header allocation failure increments `tx_errors`.
4. Header allocation failure updates `last_error_time`.
5. Packet prepend failure increments `tx_errors`.
6. Packet prepend failure updates `last_error_time`.
7. Successful send prepends exactly 14 bytes.
8. Successful send writes destination MAC.
9. Successful send writes source MAC from interface.
10. Successful send writes network-order EtherType.
11. Successful send increments `tx_bytes` by framed packet length.
12. Successful send updates `last_tx_time`.
13. Successful send sets packet layer to `2`.
14. Successful send calls link transmit and returns its success value.
15. `ethernet_receive` rejects NULL interface, frame, or output pointer.
16. `ethernet_receive` rejects frames shorter than 14 bytes.
17. `ethernet_receive` rejects `IFACE_ERR_DISABLED`.
18. `ethernet_receive` drops wrong destination MAC with return `1`.
19. Wrong-MAC drop does not strip the frame.
20. Receive accepts exact interface MAC.
21. Receive accepts broadcast MAC.
22. Accepted receive converts EtherType to host order.
23. Accepted receive strips exactly 14 bytes.
24. Accepted receive increments `rx_bytes` by post-strip payload length.
25. Accepted receive sets packet layer to `3`.
26. `ethernet_receive_event` ignores missing destination interface or packet.
27. Receive event accepted path updates `last_rx_time`.
28. Receive event accepted path calls `iface->rx_handler` if present.
29. Receive event wrong-MAC path increments `rx_dropped`.
30. Receive event error path increments `rx_errors` and updates
   `last_error_time`.

## Common Mistakes

- Do not compare only the first MAC byte.
- Do not return `0` from `ethernet_send` success unless `link_transmit` changes;
  current success is `1`.
- Do not forget EtherType byte-order conversion.
- Do not call ARP or IP directly from `ethernet_receive`.
- Do not free the caller's packet in `ethernet_send`.
- Do not assume `ctx` is used by `ethernet_receive_event`; current code ignores
  it.
- Do not describe VLAN or multicast support as implemented.
