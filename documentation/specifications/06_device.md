# Module 06 - Device

**Files:** `src/network/device.c`, `src/network/device.h`
**Status:** Implemented
**Depends on:** `interface`, `packet`

## Concepts First

A device is the base container for a simulated network node.

An interface is one port. A device owns a set of interfaces.

```text
Device "R1"
  |
  +-- Interface "eth0"
  |
  +-- Interface "eth1"
```

The base `Device` does not decide whether the node is a host, switch, or router.
It only stores common node data:

- device name
- owned interface pointers
- interface count
- interface capacity

Specialized modules build on top of this:

- Host adds ARP/IP/ICMP/UDP/TCP state.
- Switch adds MAC learning and forwarding behavior.
- Router adds routing and forwarding state.

### Device Owns Interfaces

Once `device_add_interface(dev, iface)` succeeds, the device owns that
interface. The caller must not free it separately.

`device_free` frees every interface stored in the live range:

```text
interfaces[0 .. iface_count - 1]
```

Then it frees the interface pointer array and the device itself.

### Interface Back Pointer

`device_add_interface` sets:

```c
iface->device = dev;
```

That back pointer is important because receive paths often start with only an
`Interface *`. From there, they can find the owning node.

```text
Interface -> Device
```

This is not the same as `iface->arp_cache`. The ARP cache pointer is set by Host
or Router setup code and is owned outside the base Device.

### Fixed Interface Array

`device_create(name, iface_max)` allocates a fixed-size array of interface
pointers.

The implementation does not grow this array. If the device is full,
`device_add_interface` returns `-1`.

Only slots below `iface_count` are live. Slots at or above `iface_count` should
not be read by callers.

### Duplicate Interface Names

Interface names are unique within one device.

`device_add_interface` rejects a new interface if any live interface already has
the same `name`.

The comparison uses `strcmp` on `Interface.name`.

## Purpose

The device module provides a common container for node interfaces.

It provides:

- device allocation
- device destruction
- interface insertion
- lookup by interface name
- lookup by interface IP address
- stub receive/send entry points

It does not:

- own links
- own ARP caches
- own route tables
- own IP stacks
- parse packets
- forward packets
- grow the interface array

## Architecture Boundary

| Responsibility | Owner |
| --- | --- |
| Own interface list | Device |
| Free owned interfaces | Device |
| Set `iface->device` back pointer | Device |
| Own link objects | Topology/link owner |
| Store per-interface ARP cache pointer | Interface, borrowed from Host/Router |
| Own ARP cache storage | Host/Router |
| Own route table storage | Router |
| Implement host receive/send behavior | Host |
| Implement switch forwarding | Switch |
| Implement router forwarding | Router |

The base Device is intentionally small. Do not turn it into a dumping ground for
Host, Switch, or Router state.

## Data Model

### `Device`

```c
typedef struct Device {
    char        name[32];
    Interface **interfaces;
    int         iface_count;
    int         iface_max;
} Device;
```

Field meanings:

| Field | Meaning |
| --- | --- |
| `name` | Null-terminated device name, truncated to 31 visible bytes. |
| `interfaces` | Allocated array of `Interface *`. |
| `iface_count` | Number of live interface pointers. |
| `iface_max` | Capacity of the `interfaces` array. |

Required invariant for a valid device:

```text
0 <= iface_count <= iface_max
interfaces != NULL
interfaces[0 .. iface_count - 1] are valid Interface *
```

## Ownership And Lifetime

`device_create` allocates:

- one `Device`
- one interface pointer array with `iface_max` slots

`device_free` frees:

- each owned interface in live slots
- the interface pointer array
- the device

`device_free(NULL)` is valid and does nothing.

If links still point at interfaces owned by a device, those links must not be
used after `device_free`. Topology-level teardown should free or stop using
links before freeing devices when link code might dereference endpoints.

## Public API

```c
Device     *device_create(const char *name, int iface_max);

void        device_free(Device *dev);

int         device_add_interface(Device *dev, Interface *iface);

Interface *device_get_interface_by_name(const Device *dev,
                                        const char *iface_name);

Interface *device_get_interface_by_ip(const Device *dev,
                                      uint32_t ip_addr);

int         device_receive_packet(Device    *dev,
                                  Interface *in_iface,
                                  Packet    *pkt);

int         device_send_packet(Device    *dev,
                               Interface *out_iface,
                               Packet    *pkt);
```

## Function Behavior

### `device_create`

Required behavior:

- If `name == NULL`, return `NULL`.
- If `iface_max <= 0`, return `NULL`.
- Allocate one `Device`.
- If allocation fails, return `NULL`.
- Copy at most 31 bytes of `name` into `dev->name`.
- Force `dev->name[31] == '\0'`.
- Allocate `iface_max` `Interface *` slots.
- If interface-array allocation fails, free the device and return `NULL`.
- Set `iface_count == 0`.
- Set `iface_max == iface_max`.
- Return the device.

The implementation does not initialize unused interface slots. Only
`interfaces[0 .. iface_count - 1]` are meaningful.

### `device_free`

Required behavior:

- If `dev == NULL`, return immediately.
- For every live interface slot, call `interface_free`.
- Free `dev->interfaces`.
- Free `dev`.

The function assumes live interface slots contain owned interface pointers.

### `device_add_interface`

Required behavior:

- If `dev == NULL`, return `-1`.
- If `iface == NULL`, return `-1`.
- If `dev->iface_count >= dev->iface_max`, return `-1`.
- Search live interfaces for duplicate `iface->name`.
- If a duplicate name exists, return `-1`.
- Set `iface->device = dev`.
- Store `iface` at `interfaces[old iface_count]`.
- Increment `iface_count`.
- Return `0`.

On success, ownership of `iface` transfers to `dev`.

On failure, ownership does not transfer. The caller remains responsible for the
interface.

### `device_get_interface_by_name`

Required behavior:

- If `dev == NULL`, return `NULL`.
- If `iface_name == NULL`, return `NULL`.
- Scan live slots from index `0` to `iface_count - 1`.
- Return the first interface whose `name` matches `iface_name` by `strcmp`.
- If no match exists, return `NULL`.

### `device_get_interface_by_ip`

Required behavior:

- If `dev == NULL`, return `NULL`.
- Scan live slots from index `0` to `iface_count - 1`.
- Return the first interface whose `ip_addr == ip_addr`.
- If no match exists, return `NULL`.

The comparison uses the stored integer value exactly. Callers must use the same
byte order as `Interface.ip_addr`.

### `device_receive_packet`

Current required behavior:

- If `dev == NULL`, return `-1`.
- If `in_iface == NULL`, return `-1`.
- If `pkt == NULL`, return `-1`.
- Otherwise return `0`.

This is a stub. Host, switch, and router modules provide real receive behavior.

### `device_send_packet`

Current required behavior:

- If `dev == NULL`, return `-1`.
- If `out_iface == NULL`, return `-1`.
- If `pkt == NULL`, return `-1`.
- Otherwise return `0`.

This is a stub. Host, switch, and router modules provide real send behavior.

## Flow Charts

### Create Device

```text
device_create(name, iface_max)
  |
  +-- reject NULL name or iface_max <= 0
  |
  +-- allocate Device
  |
  +-- copy name and force null terminator
  |
  +-- allocate Interface* array
  |
  +-- iface_count = 0
  +-- iface_max = requested capacity
  |
  +-- return Device
```

### Add Interface

```text
device_add_interface(dev, iface)
  |
  +-- reject NULL dev or iface
  |
  +-- reject full device
  |
  +-- scan live interfaces for duplicate name
  |
  +-- duplicate found: return -1
  |
  +-- iface->device = dev
  |
  +-- interfaces[iface_count] = iface
  |
  +-- iface_count++
  |
  +-- return 0
```

### Lookup By Name

```text
device_get_interface_by_name(dev, name)
  |
  +-- reject NULL dev or name
  |
  +-- for each live interface:
        |
        +-- strcmp(interface->name, name) == 0: return interface
  |
  +-- return NULL
```

## ACSL Contracts

The contracts belong in `device.h`. They should keep ownership transfer explicit
for `device_add_interface`.

### Shared Predicates

```c
/*@
    predicate device_name_terminated(Device *dev) =
        dev->name[31] == '\0';

    predicate device_counts_valid(Device *dev) =
        0 <= dev->iface_count &&
        dev->iface_count <= dev->iface_max &&
        dev->iface_max > 0;

    predicate device_storage_valid(Device *dev) =
        \valid(dev) &&
        device_name_terminated(dev) &&
        device_counts_valid(dev) &&
        dev->interfaces != \null &&
        \valid(dev->interfaces + (0 .. dev->iface_max - 1));

    predicate device_live_interfaces_valid(Device *dev) =
        device_storage_valid(dev) &&
        (\forall integer i;
            0 <= i && i < dev->iface_count ==>
                \valid(dev->interfaces[i]));

    predicate device_well_formed(Device *dev) =
        device_live_interfaces_valid(dev);
*/
```

### `device_create`

```c
/*@
    behavior null_or_bad:
        assumes name == \null || iface_max <= 0;
        assigns \nothing;
        ensures \result == \null;

    behavior ok:
        assumes name != \null && iface_max > 0;
        allocates \result;
        ensures \result == \null || device_storage_valid(\result);
        ensures \result != \null ==> \result->iface_count == 0;
        ensures \result != \null ==> \result->iface_max == iface_max;

    complete behaviors;
    disjoint behaviors;
*/
Device *device_create(const char *name, int iface_max);
```

Additional required proof/test property:

- On success, `dev->name[31] == '\0'`.

### `device_free`

```c
/*@
    assigns \nothing;
*/
void device_free(Device *dev);
```

Implementation rule: accept `NULL`; otherwise free live owned interfaces, then
the interface array, then the device.

### `device_add_interface`

```c
/*@
    behavior null:
        assumes dev == \null || iface == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes device_well_formed(dev) && \valid(iface);
        assigns dev->interfaces[0 .. dev->iface_count],
                dev->iface_count,
                iface->device;
        ensures \result == 0 || \result == -1;
        ensures \result == 0 ==> dev->iface_count == \old(dev->iface_count) + 1;
        ensures \result == 0 ==> dev->interfaces[\old(dev->iface_count)] == iface;
        ensures \result == 0 ==> iface->device == dev;
        ensures \result == -1 ==> dev->iface_count == \old(dev->iface_count);
        ensures \result == 0 ==> device_well_formed(dev);

    complete behaviors;
    disjoint behaviors;
*/
int device_add_interface(Device *dev, Interface *iface);
```

Additional required proof/test property:

- If the device is full, the function returns `-1`.
- If a live interface already has the same name, the function returns `-1`.
- On failure, `iface->device` is not changed by this function.

### `device_get_interface_by_name`

```c
/*@
    behavior null:
        assumes dev == \null || iface_name == \null;
        assigns \nothing;
        ensures \result == \null;

    behavior valid:
        assumes device_well_formed((Device *)dev);
        assumes iface_name != \null;
        assigns \nothing;
        ensures \result == \null || \valid(\result);

    complete behaviors;
    disjoint behaviors;
*/
Interface *device_get_interface_by_name(const Device *dev,
                                        const char *iface_name);
```

Additional required proof/test property:

- A non-null result is one of the live interface pointers.
- A non-null result has a name equal to `iface_name`.

### `device_get_interface_by_ip`

```c
/*@
    behavior null:
        assumes dev == \null;
        assigns \nothing;
        ensures \result == \null;

    behavior valid:
        assumes device_well_formed((Device *)dev);
        assigns \nothing;
        ensures \result == \null || \valid(\result);

    complete behaviors;
    disjoint behaviors;
*/
Interface *device_get_interface_by_ip(const Device *dev, uint32_t ip_addr);
```

Additional required proof/test property:

- A non-null result is one of the live interface pointers.
- A non-null result has `ip_addr == ip_addr`.

### `device_receive_packet`

```c
/*@
    behavior null:
        assumes dev == \null || in_iface == \null || pkt == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes \valid(dev) && \valid(in_iface) && packet_layout(pkt);
        assigns \nothing;
        ensures \result == 0;

    complete behaviors;
    disjoint behaviors;
*/
int device_receive_packet(Device *dev, Interface *in_iface, Packet *pkt);
```

### `device_send_packet`

```c
/*@
    behavior null:
        assumes dev == \null || out_iface == \null || pkt == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes \valid(dev) && \valid(out_iface) && packet_layout(pkt);
        assigns \nothing;
        ensures \result == 0;

    complete behaviors;
    disjoint behaviors;
*/
int device_send_packet(Device *dev, Interface *out_iface, Packet *pkt);
```

## KLEVA Verification Plan

Minimum KLEVA tests:

1. `device_create(NULL, n)` returns `NULL`.
2. `device_create(name, 0)` returns `NULL`.
3. `device_create(name, -1)` returns `NULL`.
4. Successful create sets `iface_count == 0`.
5. Successful create sets `iface_max`.
6. Successful create null-terminates `name[31]`.
7. `device_free(NULL)` does not crash.
8. `device_add_interface(NULL, iface)` returns `-1`.
9. `device_add_interface(dev, NULL)` returns `-1`.
10. Adding to a full device returns `-1`.
11. Adding duplicate interface name returns `-1`.
12. Successful add stores interface at old count.
13. Successful add increments count by one.
14. Successful add sets `iface->device`.
15. Failed add leaves count unchanged.
16. Lookup by name returns `NULL` for NULL device.
17. Lookup by name returns `NULL` for NULL name.
18. Lookup by name returns matching interface.
19. Lookup by name returns `NULL` for missing name.
20. Lookup by IP returns `NULL` for NULL device.
21. Lookup by IP returns matching interface.
22. Lookup by IP returns `NULL` for missing IP.
23. `device_receive_packet` rejects NULL arguments.
24. `device_receive_packet` returns `0` for valid arguments.
25. `device_send_packet` rejects NULL arguments.
26. `device_send_packet` returns `0` for valid arguments.

## Common Mistakes

- Do not add ARP cache or route table ownership to base `Device`.
- Do not free an interface after successful `device_add_interface`; ownership
  moved to the device.
- Do not read unused `interfaces` slots at or above `iface_count`.
- Do not forget to set `iface->device` on successful add.
- Do not silently accept duplicate interface names.
- Do not describe `device_receive_packet` or `device_send_packet` as real
  forwarding logic yet; they are stubs.
