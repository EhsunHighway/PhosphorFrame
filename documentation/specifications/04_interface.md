# Module 04 - Interface

**Files:** `src/network/interface.c`, `src/network/interface.h`
**Status:** Implemented
**Depends on:** C standard library: `stdint.h`, `stddef.h`, `stdlib.h`,
`string.h`

## Concepts First

An interface is one network attachment point on a simulated node. In real
networking terms, this is the simulator's NIC or port object.

Examples:

```text
host h1:
  eth0

router r1:
  eth0
  eth1
  eth2

switch sw1:
  port0
  port1
  port2
```

The interface carries the identity and runtime state of that attachment point:

- name
- MAC address
- IPv4 address and prefix length
- MTU
- administrative up/down flag
- link pointer
- owning device pointer
- receive callback
- borrowed ARP cache pointer
- counters and timestamps

### Interface Is Not A Device

A device is the node: host, switch, router.

An interface is one port on that node.

```text
Device
  |
  +-- Interface eth0
  |
  +-- Interface eth1
```

The back pointer `iface->device` lets code that only received an `Interface *`
find the owning device. That pointer is set by `device_add_interface`, not by
`interface_create`.

### Interface Is Not A Link

A link connects two interfaces.

```text
Interface A <---- Link ----> Interface B
```

The interface stores a borrowed pointer to its attached link:

```c
struct Link *link;
```

The interface does not own or free the link.

### Receive Handler

`RxHandler` is the upward callback used after lower-layer receive processing.

```c
typedef void (*RxHandler)(struct Interface *iface,
                          struct Packet    *pkt,
                          uint16_t          ethertype,
                          void             *ctx);
```

The interface does not call this handler itself in the current implementation.
Ethernet receive code reads `iface->rx_handler` and calls it after it has
validated and stripped the Ethernet header.

The callback context is opaque. For example, a host may set the context to its
IP stack, while another path may set it to simulator state.

### ARP Cache Pointer

`iface->arp_cache` is a borrowed pointer. It lets ARP/IP paths reach the
neighbor cache associated with the host or router that owns this interface.

The interface does not allocate, initialize, or free the ARP cache.

### Administrative Up Versus Error State

The interface has both:

```c
int up;
InterfaceState state;
```

`up` is the administrative flag controlled by `interface_set_up`.

`state` is operational state. The current enum has:

```c
IFACE_OK
IFACE_ERR_DISABLED
```

`interface_set_up` changes only `up`. It does not reset `state`.

## Purpose

The interface module owns allocation and simple field operations for one NIC or
port.

It provides:

- interface creation
- interface destruction
- up/down setter and getter
- link setter and getter
- MAC getter
- IP getter
- transmit byte counter helper
- receive byte counter helper
- receive handler setter
- ARP cache pointer setter

It does not:

- transmit packets
- receive packets by itself
- parse Ethernet or IP headers
- own links
- own devices
- own ARP caches
- validate MAC uniqueness
- install routes

## Architecture Boundary

| Responsibility | Owner |
| --- | --- |
| Store per-port identity | Interface |
| Store link pointer | Interface, borrowed |
| Store owning device back pointer | Interface, assigned by Device |
| Own interface array/list | Device |
| Own link storage | Topology/link owner |
| Own ARP cache storage | Host or Router |
| Invoke receive handler | Ethernet receive path |
| Interpret `ethertype` | Receiver above Ethernet |

Forward declarations in `interface.h` avoid circular includes. The interface
header should not include `link.h`, `packet.h`, `device.h`, or `arp_cache.h`.

## Data Model

### `RxHandler`

```c
typedef void (*RxHandler)(struct Interface *iface,
                          struct Packet    *pkt,
                          uint16_t          ethertype,
                          void             *ctx);
```

Arguments:

| Argument | Meaning |
| --- | --- |
| `iface` | Interface that received the packet. |
| `pkt` | Packet after lower-layer processing. |
| `ethertype` | Ethernet type value associated with the payload. |
| `ctx` | Opaque context stored by `interface_set_rx_handler`. |

### `InterfaceState`

```c
typedef enum {
    IFACE_OK,
    IFACE_ERR_DISABLED,
} InterfaceState;
```

### `Interface`

```c
typedef struct Interface {
    char             name[16];
    uint8_t          mac[6];
    uint32_t         ip_addr;
    uint8_t          prefix_len;
    uint16_t         mtu;
    int              up;
    struct Link     *link;
    struct Device   *device;
    uint64_t         tx_bytes;
    uint64_t         rx_bytes;
    RxHandler        rx_handler;
    void            *handler_ctx;
    struct ArpCache *arp_cache;
    uint64_t         rx_dropped;
    uint64_t         rx_errors;
    uint64_t         tx_errors;
    InterfaceState   state;
    uint64_t         last_rx_time;
    uint64_t         last_tx_time;
    uint64_t         last_error_time;
} Interface;
```

Field meanings:

| Field | Meaning |
| --- | --- |
| `name` | Null-terminated interface name, truncated to 15 visible bytes. |
| `mac` | Six-byte MAC address. |
| `ip_addr` | IPv4 address in network byte order. |
| `prefix_len` | CIDR prefix length, expected range `0..32`. |
| `mtu` | Maximum transmission unit. |
| `up` | Administrative up/down flag. |
| `link` | Borrowed link pointer, or `NULL`. |
| `device` | Borrowed owner back pointer, set outside this module. |
| `tx_bytes` | Transmitted byte counter. |
| `rx_bytes` | Received byte counter. |
| `rx_handler` | Upward receive callback. |
| `handler_ctx` | Opaque context passed to `rx_handler`. |
| `arp_cache` | Borrowed ARP cache pointer. |
| `rx_dropped` | Receive drop counter. |
| `rx_errors` | Receive error counter. |
| `tx_errors` | Transmit error counter. |
| `state` | Operational state. |
| `last_rx_time` | Last successful receive timestamp. |
| `last_tx_time` | Last successful transmit timestamp. |
| `last_error_time` | Last error timestamp. |

## Ownership And Lifetime

`interface_create` allocates one `Interface`.

`interface_free` frees only the interface object.

The interface does not free:

- `link`
- `device`
- `handler_ctx`
- `arp_cache`

The current implementation expects valid inputs for `interface_create`:

- `name != NULL`
- `mac != NULL`
- `prefix_len <= 32`
- `mtu > 0`

It also expects valid input for `interface_get_mac` and `interface_get_ip`.
Those two getters do not check for `NULL`.

## Public API

```c
Interface     *interface_create(const char    *name,
                                const uint8_t  mac[6],
                                uint32_t       ip_addr,
                                uint8_t        prefix_len,
                                uint16_t       mtu);

void           interface_free(Interface *iface);

void           interface_set_up(Interface *iface, int up);

int            interface_is_up(const Interface *iface);

void           interface_set_link(Interface *iface, struct Link *link);

struct Link   *interface_get_link(const Interface *iface);

const uint8_t *interface_get_mac(const Interface *iface);

uint32_t       interface_get_ip(const Interface *iface);

void           interface_add_tx_bytes(Interface *iface, uint64_t n);

void           interface_add_rx_bytes(Interface *iface, uint64_t n);

void           interface_set_rx_handler(Interface *iface,
                                        RxHandler  fn,
                                        void      *ctx);

void           interface_set_arp_cache(Interface *iface,
                                       struct ArpCache *cache);
```

## Function Behavior

### `interface_create`

Required behavior:

- Caller must pass non-NULL `name`.
- Caller must pass non-NULL `mac`.
- Caller must pass `prefix_len <= 32`.
- Caller must pass `mtu > 0`.
- Allocate one `Interface`.
- If allocation fails, return `NULL`.
- Copy at most 15 bytes of `name` into `iface->name`.
- Force `iface->name[15] == '\0'`.
- Copy exactly six MAC bytes.
- Store `ip_addr`, `prefix_len`, and `mtu`.
- Set `up == 0`.
- Set `link == NULL`.
- Set `device == NULL`.
- Set `tx_bytes == 0`.
- Set `rx_bytes == 0`.
- Set `rx_handler == NULL`.
- Set `handler_ctx == NULL`.
- Set `arp_cache == NULL`.
- Set `rx_dropped == 0`.
- Set `rx_errors == 0`.
- Set `tx_errors == 0`.
- Set `state == IFACE_OK`.
- Set all last-event timestamps to `0`.

### `interface_free`

Required behavior:

- If `iface == NULL`, return immediately.
- Free the interface object.
- Do not free borrowed pointers stored inside the interface.

### `interface_set_up`

Required behavior:

- If `iface == NULL`, return without changing state.
- Otherwise set `iface->up = up`.

This function does not change `iface->state`.

### `interface_is_up`

Required behavior:

- If `iface == NULL`, return `0`.
- If `iface->up != 0`, return `1`.
- Otherwise return `0`.

### `interface_set_link`

Required behavior:

- If `iface == NULL`, return without changing state.
- Otherwise set `iface->link = link`.

The link pointer is borrowed.

### `interface_get_link`

Required behavior:

- If `iface == NULL`, return `NULL`.
- Otherwise return `iface->link`.

### `interface_get_mac`

Required behavior:

- Caller must pass a valid interface.
- Return `iface->mac`.

The returned pointer points into the interface object. The caller must not free
it.

### `interface_get_ip`

Required behavior:

- Caller must pass a valid interface.
- Return `iface->ip_addr`.

### `interface_add_tx_bytes`

Required behavior:

- If `iface == NULL`, return without changing state.
- Otherwise add `n` to `iface->tx_bytes`.

The current implementation does not check overflow.

### `interface_add_rx_bytes`

Required behavior:

- If `iface == NULL`, return without changing state.
- Otherwise add `n` to `iface->rx_bytes`.

The current implementation does not check overflow.

### `interface_set_rx_handler`

Required behavior:

- If `iface == NULL`, return without changing state.
- Otherwise set:
  - `iface->rx_handler = fn`
  - `iface->handler_ctx = ctx`

`fn == NULL` is allowed and clears the handler.

### `interface_set_arp_cache`

Required behavior:

- If `iface == NULL`, return without changing state.
- Otherwise set `iface->arp_cache = cache`.

`cache == NULL` is allowed and clears the borrowed pointer.

## Flow Charts

### Create Interface

```text
interface_create(name, mac, ip, prefix_len, mtu)
  |
  +-- caller must provide valid arguments
  |
  +-- allocate Interface
  |
  +-- copy name and force null terminator
  |
  +-- copy MAC
  |
  +-- store IP, prefix length, MTU
  |
  +-- initialize pointers, counters, handlers, state, timestamps
  |
  +-- return Interface
```

### Attach To Device And Link

```text
interface_create(...)
  |
  +-- device_add_interface(device, iface)
  |     |
  |     +-- stores iface in device
  |     +-- sets iface->device = device
  |
  +-- interface_set_link(iface, link)
```

### Receive Callback Setup

```text
host/router setup
  |
  +-- interface_set_rx_handler(iface, fn, ctx)
  |
  +-- later Ethernet receive path:
        |
        +-- validate Ethernet frame
        +-- strip Ethernet header
        +-- call iface->rx_handler(iface, pkt, ethertype, iface->handler_ctx)
```

## ACSL Contracts

The contracts belong in `interface.h`. They should describe the current code:
some functions are null-safe, while `interface_get_mac` and `interface_get_ip`
require valid interfaces.

### Shared Predicates

```c
/*@
    predicate interface_name_terminated(Interface *iface) =
        iface->name[15] == '\0';

    predicate interface_basic_valid(Interface *iface) =
        \valid(iface) &&
        interface_name_terminated(iface) &&
        iface->prefix_len <= 32 &&
        iface->mtu > 0 &&
        (iface->state == IFACE_OK || iface->state == IFACE_ERR_DISABLED);

    predicate interface_initial_state(Interface *iface) =
        interface_basic_valid(iface) &&
        iface->up == 0 &&
        iface->link == \null &&
        iface->device == \null &&
        iface->tx_bytes == 0 &&
        iface->rx_bytes == 0 &&
        iface->rx_handler == \null &&
        iface->handler_ctx == \null &&
        iface->arp_cache == \null &&
        iface->rx_dropped == 0 &&
        iface->rx_errors == 0 &&
        iface->tx_errors == 0 &&
        iface->state == IFACE_OK &&
        iface->last_rx_time == 0 &&
        iface->last_tx_time == 0 &&
        iface->last_error_time == 0;
*/
```

### `interface_create`

```c
/*@
    requires name != \null;
    requires mac != \null;
    requires \valid_read(mac + (0 .. 5));
    requires prefix_len <= 32;
    requires mtu > 0;
    allocates \result;
    ensures \result == \null || interface_initial_state(\result);
    ensures \result != \null ==> \result->ip_addr == ip_addr;
    ensures \result != \null ==> \result->prefix_len == prefix_len;
    ensures \result != \null ==> \result->mtu == mtu;
*/
Interface *interface_create(const char *name,
                            const uint8_t mac[6],
                            uint32_t ip_addr,
                            uint8_t prefix_len,
                            uint16_t mtu);
```

Additional required proof/test property:

- On success, `iface->mac[0 .. 5]` equals `mac[0 .. 5]`.
- On success, `iface->name[15] == '\0'`.

### `interface_free`

```c
/*@
    assigns \nothing;
*/
void interface_free(Interface *iface);
```

Implementation rule: accept `NULL`; otherwise free only the interface object.

### `interface_set_up`

```c
/*@
    behavior null:
        assumes iface == \null;
        assigns \nothing;

    behavior valid:
        assumes \valid(iface);
        assigns iface->up;
        ensures iface->up == up;

    complete behaviors;
    disjoint behaviors;
*/
void interface_set_up(Interface *iface, int up);
```

### `interface_is_up`

```c
/*@
    behavior null:
        assumes iface == \null;
        assigns \nothing;
        ensures \result == 0;

    behavior valid:
        assumes \valid_read(iface);
        assigns \nothing;
        ensures \result == 0 || \result == 1;
        ensures iface->up != 0 ==> \result == 1;
        ensures iface->up == 0 ==> \result == 0;

    complete behaviors;
    disjoint behaviors;
*/
int interface_is_up(const Interface *iface);
```

### `interface_set_link`

```c
/*@
    behavior null:
        assumes iface == \null;
        assigns \nothing;

    behavior valid:
        assumes \valid(iface);
        assigns iface->link;
        ensures iface->link == link;

    complete behaviors;
    disjoint behaviors;
*/
void interface_set_link(Interface *iface, struct Link *link);
```

### `interface_get_link`

```c
/*@
    behavior null:
        assumes iface == \null;
        assigns \nothing;
        ensures \result == \null;

    behavior valid:
        assumes \valid_read(iface);
        assigns \nothing;
        ensures \result == iface->link;

    complete behaviors;
    disjoint behaviors;
*/
struct Link *interface_get_link(const Interface *iface);
```

### `interface_get_mac`

```c
/*@
    requires \valid_read(iface);
    assigns \nothing;
    ensures \result == iface->mac;
*/
const uint8_t *interface_get_mac(const Interface *iface);
```

### `interface_get_ip`

```c
/*@
    requires \valid_read(iface);
    assigns \nothing;
    ensures \result == iface->ip_addr;
*/
uint32_t interface_get_ip(const Interface *iface);
```

### `interface_add_tx_bytes`

```c
/*@
    behavior null:
        assumes iface == \null;
        assigns \nothing;

    behavior valid:
        assumes \valid(iface);
        assigns iface->tx_bytes;
        ensures iface->tx_bytes == \old(iface->tx_bytes) + n;

    complete behaviors;
    disjoint behaviors;
*/
void interface_add_tx_bytes(Interface *iface, uint64_t n);
```

### `interface_add_rx_bytes`

```c
/*@
    behavior null:
        assumes iface == \null;
        assigns \nothing;

    behavior valid:
        assumes \valid(iface);
        assigns iface->rx_bytes;
        ensures iface->rx_bytes == \old(iface->rx_bytes) + n;

    complete behaviors;
    disjoint behaviors;
*/
void interface_add_rx_bytes(Interface *iface, uint64_t n);
```

### `interface_set_rx_handler`

```c
/*@
    behavior null:
        assumes iface == \null;
        assigns \nothing;

    behavior valid:
        assumes \valid(iface);
        assigns iface->rx_handler, iface->handler_ctx;
        ensures iface->rx_handler == fn;
        ensures iface->handler_ctx == ctx;

    complete behaviors;
    disjoint behaviors;
*/
void interface_set_rx_handler(Interface *iface, RxHandler fn, void *ctx);
```

### `interface_set_arp_cache`

```c
/*@
    behavior null:
        assumes iface == \null;
        assigns \nothing;

    behavior valid:
        assumes \valid(iface);
        assigns iface->arp_cache;
        ensures iface->arp_cache == cache;

    complete behaviors;
    disjoint behaviors;
*/
void interface_set_arp_cache(Interface *iface, struct ArpCache *cache);
```

## KLEVA Verification Plan

Minimum KLEVA tests:

1. `interface_create` with valid inputs returns either `NULL` or initialized
   interface state.
2. Successful create stores `ip_addr`.
3. Successful create stores `prefix_len`.
4. Successful create stores `mtu`.
5. Successful create copies six MAC bytes.
6. Successful create null-terminates `name[15]`.
7. Successful create sets `up == 0`.
8. Successful create sets `link == NULL`.
9. Successful create sets `device == NULL`.
10. Successful create sets `rx_handler == NULL`.
11. Successful create sets `handler_ctx == NULL`.
12. Successful create sets `arp_cache == NULL`.
13. Successful create sets counters and timestamps to zero.
14. Successful create sets `state == IFACE_OK`.
15. `interface_free(NULL)` does not crash.
16. `interface_set_up(NULL, value)` is a no-op.
17. `interface_set_up` stores the exact `up` value.
18. `interface_is_up(NULL)` returns `0`.
19. `interface_is_up` returns `1` for any nonzero `up`.
20. `interface_is_up` returns `0` for `up == 0`.
21. `interface_set_link(NULL, link)` is a no-op.
22. `interface_set_link` stores the borrowed link pointer.
23. `interface_get_link(NULL)` returns `NULL`.
24. `interface_get_link` returns the stored link pointer.
25. `interface_get_mac` returns a pointer to the interface's internal MAC.
26. `interface_get_ip` returns the stored IP address.
27. `interface_add_tx_bytes(NULL, n)` is a no-op.
28. `interface_add_tx_bytes` adds exactly `n`.
29. `interface_add_rx_bytes(NULL, n)` is a no-op.
30. `interface_add_rx_bytes` adds exactly `n`.
31. `interface_set_rx_handler` stores callback and context.
32. `interface_set_arp_cache` stores the borrowed cache pointer.

## Common Mistakes

- Do not say `interface_create` validates bad input; current code relies on
  caller preconditions.
- Do not free `link`, `device`, `handler_ctx`, or `arp_cache` in
  `interface_free`.
- Do not set `iface->device` in `interface_create`; device ownership is wired
  later.
- Do not treat `iface->arp_cache` as owned by the interface.
- Do not call `interface_get_mac(NULL)` or `interface_get_ip(NULL)`.
- Do not make `interface_set_up` reset `IFACE_ERR_DISABLED`.
