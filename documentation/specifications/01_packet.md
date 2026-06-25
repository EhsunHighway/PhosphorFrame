# Module 01 - Packet Buffer

**Files:** `src/network/packet.c`, `src/network/packet.h`
**Status:** Implemented
**Depends on:** C standard library: `stdint.h`, `stddef.h`, `stdlib.h`,
`string.h`, `stdio.h`

## Concepts First

A packet buffer is the object that carries bytes through the simulator. Ethernet,
IP, ICMP, UDP, TCP, switches, links, and hosts all pass around `Packet *`.

The important idea is that the packet owns one allocated byte buffer, but the
start of the currently visible data can move inside that allocation.

```text
allocated bytes:

head
 |
 v
+----------------------+-----------------------------+
| reserved headroom    | payload/data capacity        |
| 64 bytes             | capacity bytes               |
+----------------------+-----------------------------+
                       ^
                       |
                      data at creation
```

`head` is the original allocation pointer. It never moves.

`data` is the pointer to the first byte visible to the current layer. It moves
backward when a header is prepended and forward when a header is stripped.

`len` is the number of visible bytes starting at `data`.

`capacity` is the usable payload capacity after the fixed headroom. It does not
include the 64 bytes of headroom.

### Why Headroom Exists

When an application or transport layer creates a payload, lower layers still
need to add headers in front of it:

```text
TCP header -> IP header -> Ethernet header -> payload
```

Without headroom, every layer would need to allocate a bigger buffer and copy
the old bytes. With headroom, `packet_prepend` only moves `data` backward and
copies the new header into the newly exposed space.

The current implementation reserves:

```c
#define PKT_HEADROOM 64
```

That is enough for the common Ethernet + IPv4 + TCP path:

```text
Ethernet 14 bytes + IPv4 20 bytes + TCP 20 bytes = 54 bytes
```

The extra 10 bytes give a small safety margin. This module does not grow the
allocation if headroom runs out.

### Strip Does Not Erase Bytes

`packet_strip(p, n)` advances `data` by `n` and decreases `len` by `n`.

It does not zero, free, or overwrite the stripped header bytes. The old bytes
remain in memory before the new `data` pointer until a later prepend overwrites
them.

That matters for receive paths. A protocol may read a header, validate it, then
strip it so the next protocol sees only its own payload.

### Clone Means Independent Ownership

`packet_clone` creates a second `Packet` with a separate allocation. The clone
copies only the currently visible bytes, not old stripped bytes and not unused
headroom.

This matters for switch flooding and retransmission-style paths. If the same
`Packet *` were sent to multiple owners, one receiver might free or mutate bytes
that another receiver still expects to use.

### Checksum

`packet_checksum` computes a 16-bit one's-complement checksum over a caller
provided byte range. It is shared by protocols that need IP-style checksums.

The function treats the input as `uint16_t` words in host memory order. For an
odd trailing byte, the current implementation adds that final byte value
directly. The spec must preserve that behavior unless the checksum code and all
dependent tests are intentionally changed together.

## Purpose

The packet module provides owned byte storage plus simple operations for moving
the visible data window.

It provides:

- packet allocation with fixed headroom
- prepend of bytes before current `data`
- strip of bytes from current `data`
- deep clone of visible bytes
- packet destruction
- one's-complement checksum helper
- debug dump

It does not:

- parse Ethernet, IP, UDP, TCP, or ICMP
- know MTU rules
- grow buffers dynamically
- own interfaces, links, hosts, or simulator state
- decide when a packet should be freed after transmission

## Architecture Boundary

| Responsibility | Owner |
| --- | --- |
| Own packet byte allocation | `Packet` |
| Move visible data pointer | Packet module |
| Interpret header bytes | Protocol modules |
| Decide packet route or egress interface | Host/router/switch/IP modules |
| Clone before multi-destination sends | Caller, using `packet_clone` |
| Free packet storage | Final owner, using `packet_free` |

The packet module is intentionally low-level. It should not include protocol
headers or call protocol functions.

## Data Model

### Constant

```c
#define PKT_HEADROOM 64
```

### `Packet`

```c
typedef struct Packet {
    uint8_t *head;
    uint8_t *data;
    size_t   len;
    size_t   capacity;
    uint32_t id;
    int      layer;
} Packet;
```

Field meanings:

| Field | Meaning |
| --- | --- |
| `head` | Start of the allocated byte buffer. This is the pointer that is freed. |
| `data` | Start of the currently visible packet bytes. |
| `len` | Number of visible bytes beginning at `data`. |
| `capacity` | Payload/data capacity, excluding `PKT_HEADROOM`. |
| `id` | Monotonically increasing packet identifier for tracing/debugging. |
| `layer` | Current OSI-ish display layer used by render/debug code. |

### Required Layout Invariant

For a valid packet:

```text
head <= data
data + len <= head + PKT_HEADROOM + capacity
```

The allocated byte range is:

```text
head[0 .. PKT_HEADROOM + capacity - 1]
```

The visible byte range is:

```text
data[0 .. len - 1]
```

## Ownership And Lifetime

`packet_create` allocates two objects:

- the `Packet` struct
- the byte buffer stored in `packet->head`

`packet_free` frees both. It must free `head`, not `data`, because `data` may
have moved.

`packet_free(NULL)` is valid and does nothing.

The current implementation does not defend against `NULL` in
`packet_prepend`, `packet_strip`, `packet_clone`, `packet_checksum`, or
`packet_dump`. Callers must pass valid arguments to those functions.

## Public API

```c
Packet  *packet_create(size_t capacity);

int      packet_prepend(Packet     *p,
                        const void *header,
                        size_t      header_len);

int      packet_strip(Packet *p, size_t header_len);

Packet  *packet_clone(const Packet *p);

void     packet_free(Packet *p);

uint16_t packet_checksum(const void *data, size_t len);

void     packet_dump(const Packet *p);
```

## Function Behavior

### `packet_create`

Required behavior:

- Allocate a `Packet`.
- Allocate `PKT_HEADROOM + capacity` bytes for `head`.
- If either allocation fails, return `NULL` and leak nothing from this call.
- Set `data == head + PKT_HEADROOM`.
- Set `len == 0`.
- Set `capacity == capacity`.
- Set `layer == 0`.
- Assign a fresh packet `id`.

The implementation currently accepts `capacity == 0` at the C level, but the
ACSL contract requires `capacity > 0`. Tests and new callers should treat
zero-capacity packet creation as outside the supported API.

### `packet_prepend`

Required behavior:

- Caller must pass a valid packet.
- Caller must pass a readable `header` buffer of `header_len` bytes.
- If available headroom before `data` is smaller than `header_len`, return `-1`.
- On failure, leave `data` and `len` unchanged.
- On success:
  - move `data` backward by `header_len`
  - increase `len` by `header_len`
  - copy `header_len` bytes from `header` into the new bytes at `data`
  - return `0`

The function does not check whether `header` is `NULL`. A nonzero
`header_len` with a bad header pointer is caller error.

### `packet_strip`

Required behavior:

- Caller must pass a valid packet.
- If `header_len > len`, return `-1`.
- On failure, leave `data` and `len` unchanged.
- On success:
  - move `data` forward by `header_len`
  - decrease `len` by `header_len`
  - return `0`

`header_len == 0` is accepted by the implementation and is a no-op success.

### `packet_clone`

Required behavior:

- Caller must pass a valid source packet.
- Allocate a new packet with capacity equal to `p->len`.
- If allocation fails, return `NULL`.
- Copy exactly the visible bytes from `p->data[0 .. p->len - 1]`.
- Set clone `len == p->len`.
- Set clone `layer == p->layer`.
- Give the clone its own allocation and its own `id`.

The clone starts with fresh headroom, so lower layers can prepend headers to the
clone independently.

### `packet_free`

Required behavior:

- If `p == NULL`, return immediately.
- Free `p->head`.
- Free `p`.

The function must not free `p->data`.

### `packet_checksum`

Required behavior:

- Caller must pass a readable byte range of `len` bytes.
- Sum 16-bit words until fewer than two bytes remain.
- If one byte remains, add that byte value to the sum.
- Fold carries until the sum fits in 16 bits.
- Return the one's complement of the folded sum.

The implementation currently requires `len > 0` in its ACSL contract.

### `packet_dump`

Required behavior:

- Caller must pass a valid packet.
- Print packet id, length, layer, and visible data bytes.
- Assign no packet state.

This is a debugging helper, not a verification target.

## Flow Charts

### Outbound Header Construction

```text
packet_create(capacity)
  |
  +-- data starts at head + PKT_HEADROOM
  |
  +-- write payload bytes, if caller has payload
  |
  +-- packet_prepend(TCP header)
  |
  +-- packet_prepend(IP header)
  |
  +-- packet_prepend(Ethernet header)
  |
  +-- transmit packet
```

### Inbound Header Consumption

```text
received Packet
  |
  +-- Ethernet reads bytes at data
  +-- packet_strip(ethernet header length)
  |
  +-- IP reads bytes at new data
  +-- packet_strip(ip header length)
  |
  +-- UDP/TCP/ICMP reads bytes at new data
```

### Clone For Multiple Consumers

```text
switch needs to flood one ingress packet
  |
  +-- for each egress interface:
        |
        +-- packet_clone(original)
        +-- send clone to that interface
  |
  +-- original owner frees or consumes original once
```

## ACSL Contracts

The contracts belong in `packet.h`. Keep literal numeric behavior simple for
KLEVA/EVA.

### Shared Predicates

```c
/*@
    predicate packet_layout(Packet *p) =
        \valid(p) &&
        p->head != \null &&
        \valid(p->head + (0 .. PKT_HEADROOM + p->capacity - 1)) &&
        p->head <= p->data &&
        p->data + p->len <= p->head + PKT_HEADROOM + p->capacity;

    predicate packet_visible_bytes(Packet *p) =
        packet_layout(p) &&
        (p->len == 0 || \valid(p->data + (0 .. p->len - 1)));
*/
```

### `packet_create`

```c
/*@
    requires capacity > 0;
    allocates \result;
    ensures \result == \null || packet_layout(\result);
    ensures \result != \null ==> \result->len == 0;
    ensures \result != \null ==> \result->capacity == capacity;
    ensures \result != \null ==> \result->layer == 0;
    ensures \result != \null ==> \result->data == \result->head + PKT_HEADROOM;
*/
Packet *packet_create(size_t capacity);
```

### `packet_prepend`

```c
/*@
    requires packet_layout(p);
    requires header_len > 0;
    requires \valid_read((uint8_t *)header + (0 .. header_len - 1));
    assigns p->data, p->len, p->head[0 .. PKT_HEADROOM + p->capacity - 1];

    behavior ok:
        assumes (size_t)(p->data - p->head) >= header_len;
        ensures \result == 0;
        ensures p->data == \old(p->data) - header_len;
        ensures p->len == \old(p->len) + header_len;
        ensures packet_layout(p);

    behavior no_headroom:
        assumes (size_t)(p->data - p->head) < header_len;
        ensures \result == -1;
        ensures p->data == \old(p->data);
        ensures p->len == \old(p->len);
        ensures packet_layout(p);

    complete behaviors;
    disjoint behaviors;
*/
int packet_prepend(Packet *p, const void *header, size_t header_len);
```

### `packet_strip`

```c
/*@
    requires packet_layout(p);
    assigns p->data, p->len;

    behavior valid_strip:
        assumes header_len <= p->len;
        ensures \result == 0;
        ensures p->data == \old(p->data) + header_len;
        ensures p->len == \old(p->len) - header_len;
        ensures packet_layout(p);

    behavior overflow:
        assumes header_len > p->len;
        ensures \result == -1;
        ensures p->data == \old(p->data);
        ensures p->len == \old(p->len);
        ensures packet_layout(p);

    complete behaviors;
    disjoint behaviors;
*/
int packet_strip(Packet *p, size_t header_len);
```

### `packet_clone`

```c
/*@
    requires packet_visible_bytes((Packet *)p);
    allocates \result;
    ensures \result == \null || packet_layout(\result);
    ensures \result != \null ==> \result->len == p->len;
    ensures \result != \null ==> \result->capacity == p->len;
    ensures \result != \null ==> \result->layer == p->layer;
    ensures \result != \null ==> \result->id != p->id;
*/
Packet *packet_clone(const Packet *p);
```

Additional required proof/test property:

- If `packet_clone` succeeds, the clone's visible byte sequence equals the
  source packet's visible byte sequence.
- The clone's `head` allocation is independent from the source packet's `head`
  allocation.

### `packet_free`

```c
/*@
    assigns \nothing;
*/
void packet_free(Packet *p);
```

ACSL cannot model `free` ownership precisely with the lightweight style used in
this project. The important implementation rule is natural-language: free
`p->head`, then free `p`; accept `NULL`.

### `packet_checksum`

```c
/*@
    requires len > 0;
    requires \valid_read((uint8_t *)data + (0 .. len - 1));
    assigns \nothing;
*/
uint16_t packet_checksum(const void *data, size_t len);
```

### `packet_dump`

```c
/*@
    requires packet_visible_bytes((Packet *)p);
    assigns \nothing;
*/
void packet_dump(const Packet *p);
```

## KLEVA Verification Plan

Minimum KLEVA tests:

1. `packet_create(1)` returns either `NULL` or a packet with `len == 0`.
2. Successful create sets `data == head + 64`.
3. Successful create sets `capacity` to the requested capacity.
4. `packet_prepend` succeeds when enough headroom exists.
5. Successful prepend moves `data` backward by exactly `header_len`.
6. Successful prepend increases `len` by exactly `header_len`.
7. Successful prepend copies header bytes into the visible front.
8. `packet_prepend` fails when headroom is insufficient.
9. Failed prepend leaves `data` and `len` unchanged.
10. `packet_strip` succeeds when `header_len <= len`.
11. Successful strip moves `data` forward by exactly `header_len`.
12. Successful strip decreases `len` by exactly `header_len`.
13. `packet_strip` fails when `header_len > len`.
14. Failed strip leaves `data` and `len` unchanged.
15. `packet_strip(p, 0)` is a no-op success.
16. `packet_clone` copies visible bytes and `layer`.
17. `packet_clone` gives the clone a different `id`.
18. Mutating clone visible bytes does not mutate source visible bytes.
19. `packet_free(NULL)` does not crash.
20. `packet_checksum` handles even-length input.
21. `packet_checksum` handles odd-length input according to current behavior.

## Common Mistakes

- Do not free `data`; free `head`.
- Do not assume `data == head + PKT_HEADROOM` after prepends or strips.
- Do not assume stripped bytes were erased.
- Do not send the same `Packet *` to multiple owners without cloning.
- Do not call `packet_prepend` with a nonzero length and a bad header pointer.
- Do not add protocol parsing to this module.
- Do not make `capacity` include `PKT_HEADROOM`; the code treats them
  separately.
