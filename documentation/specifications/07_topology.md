# Module 07 - Topology

**Files:** `src/network/topology.c`, `src/network/topology.h`
**Status:** Implemented
**Depends on:** `device`, `link`, `interface`

## Concepts First

A topology is the whole simulated network graph.

Devices are nodes. Links are edges.

```text
H1 ---- SW1 ---- R1 ---- R2 ---- H2
```

In code, the topology owns two growable pointer arrays:

```text
Topology
  |
  +-- devices[0 .. dev_count - 1]
  |
  +-- links[0 .. link_count - 1]
```

The topology is the registry that lets other modules find devices and links
without keeping their own global lists.

### Topology Owns Devices And Links

After `topology_add_device(topo, dev)` succeeds, the topology owns `dev`.

After `topology_add_link(topo, link)` succeeds, the topology owns `link`.

Ownership means `topology_free` will eventually free them.

If an add operation fails, ownership does not transfer. The caller still owns
the object.

### Devices Own Interfaces

Topology owns devices. Devices own interfaces.

```text
Topology
  |
  +-- Device
        |
        +-- Interface
```

Topology lookup by IP walks through devices and then through each device's live
interfaces.

### Links Borrow Interfaces

Topology owns links, but links borrow interface pointers.

```text
Topology owns Link
Link borrows Interface endpoints
Device owns Interface endpoints
```

The current `topology_free` frees devices first and links second. That is safe
only because `link_free` does not dereference link endpoints. If link teardown
ever needs endpoint fields, the free order must change to links first, then
devices.

### Growable Arrays

The topology starts with capacity `8` for devices and `8` for links.

When a live array is full, add operations double the capacity with `realloc`.

Unused slots are not meaningful. Only these ranges are live:

```text
devices[0 .. dev_count - 1]
links[0 .. link_count - 1]
```

### Linear Lookup

The current implementation uses linear scans:

- device by name: scan devices
- device by IP: scan devices, then their interfaces
- link between interfaces: scan links

This is acceptable for the simulator's small topology size and keeps behavior
easy to verify.

## Purpose

The topology module owns and indexes the network graph.

It provides:

- topology allocation
- topology destruction
- device insertion
- link insertion
- device lookup by name
- device lookup by interface IP
- link lookup between two interfaces
- device count accessor
- link count accessor

It does not:

- create devices
- create interfaces
- create links
- attach links to interfaces
- enforce unique device names
- enforce unique IP addresses
- run packet forwarding
- render the network

## Architecture Boundary

| Responsibility | Owner |
| --- | --- |
| Own all devices after successful add | Topology |
| Own all links after successful add | Topology |
| Own interfaces | Device |
| Store link endpoints | Link |
| Attach a link to an interface | Caller via `interface_set_link` |
| Find a device by name or IP | Topology |
| Find a link between two interfaces | Topology |
| Render topology | Display module |
| Execute simulation events | Scheduler/simulator |

Topology is storage and lookup. It should not know protocol behavior.

## Data Model

### `Topology`

```c
typedef struct Topology {
    Device **devices;
    int      dev_count;
    int      dev_cap;
    Link   **links;
    int      link_count;
    int      link_cap;
} Topology;
```

Field meanings:

| Field | Meaning |
| --- | --- |
| `devices` | Allocated array of `Device *`. |
| `dev_count` | Number of live device pointers. |
| `dev_cap` | Allocated device pointer capacity. |
| `links` | Allocated array of `Link *`. |
| `link_count` | Number of live link pointers. |
| `link_cap` | Allocated link pointer capacity. |

Required shape:

```text
0 <= dev_count <= dev_cap
0 <= link_count <= link_cap
dev_cap > 0
link_cap > 0
devices != NULL
links != NULL
```

## Ownership And Lifetime

`topology_create` allocates:

- one `Topology`
- one device pointer array
- one link pointer array

`topology_free`:

- frees each live device with `device_free`
- frees each live link with `link_free`
- frees the device pointer array
- frees the link pointer array
- frees the topology

`topology_free(NULL)` is valid and does nothing.

Adding a pointer transfers ownership only if the add function returns `0`.

## Public API

```c
Topology *topology_create(void);

void      topology_free(Topology *topo);

int       topology_add_device(Topology *topo, Device *dev);

int       topology_add_link(Topology *topo, Link *link);

Device   *topology_find_device_by_name(const Topology *topo,
                                       const char *name);

Device   *topology_find_device_by_ip(const Topology *topo,
                                     uint32_t ip_addr);

Link     *topology_get_link_between(const Topology  *topo,
                                    const Interface *iface_a,
                                    const Interface *iface_b);

int       topology_device_count(const Topology *topo);

int       topology_link_count(const Topology *topo);
```

## Function Behavior

### `topology_create`

Required behavior:

- Allocate one `Topology`.
- If allocation fails, return `NULL`.
- Set `dev_count == 0`.
- Set `dev_cap == 8`.
- Set `link_count == 0`.
- Set `link_cap == 8`.
- Allocate `dev_cap` device pointer slots.
- Allocate `link_cap` link pointer slots.
- If either array allocation fails:
  - free any array that was allocated
  - free the topology
  - return `NULL`
- Return the topology.

Unused pointer slots are not initialized by the current implementation.

### `topology_free`

Required behavior:

- If `topo == NULL`, return immediately.
- Free live devices from index `0` to `dev_count - 1`.
- Free live links from index `0` to `link_count - 1`.
- Free `topo->devices`.
- Free `topo->links`.
- Free `topo`.

The current free order is devices first, then links.

### `topology_add_device`

Required behavior:

- If `topo == NULL`, return `-1`.
- If `dev == NULL`, return `-1`.
- If `dev_count == dev_cap`, grow the device array to `dev_cap * 2`.
- If `realloc` fails, return `-1` and leave `dev_count` unchanged.
- Store `dev` at `devices[old dev_count]`.
- Increment `dev_count`.
- Return `0`.

The current implementation does not reject duplicate device names.

### `topology_add_link`

Required behavior:

- If `topo == NULL`, return `-1`.
- If `link == NULL`, return `-1`.
- If `link_count == link_cap`, grow the link array to `link_cap * 2`.
- If `realloc` fails, return `-1` and leave `link_count` unchanged.
- Store `link` at `links[old link_count]`.
- Increment `link_count`.
- Return `0`.

The current implementation does not reject duplicate links.

### `topology_find_device_by_name`

Required behavior:

- If `topo == NULL`, return `NULL`.
- If `name == NULL`, return `NULL`.
- Scan live devices from index `0` to `dev_count - 1`.
- Return the first device whose `device->name` matches `name` by `strcmp`.
- If no match exists, return `NULL`.

### `topology_find_device_by_ip`

Required behavior:

- If `topo == NULL`, return `NULL`.
- For each live device:
  - scan its live interfaces from index `0` to `iface_count - 1`
  - if any interface has `ip_addr == ip_addr`, return that device
- If no match exists, return `NULL`.

The comparison uses the stored integer IP value exactly.

### `topology_get_link_between`

Required behavior:

- If `topo == NULL`, return `NULL`.
- If `iface_a == NULL`, return `NULL`.
- If `iface_b == NULL`, return `NULL`.
- Scan live links from index `0` to `link_count - 1`.
- Return the first link where endpoints match either order:
  - `end_a == iface_a && end_b == iface_b`
  - `end_a == iface_b && end_b == iface_a`
- If no match exists, return `NULL`.

The comparison uses interface pointer identity.

### `topology_device_count`

Required behavior:

- If `topo == NULL`, return `0`.
- Otherwise return `topo->dev_count`.

### `topology_link_count`

Required behavior:

- If `topo == NULL`, return `0`.
- Otherwise return `topo->link_count`.

## Flow Charts

### Create Topology

```text
topology_create()
  |
  +-- allocate Topology
  |
  +-- set counts to 0
  +-- set capacities to 8
  |
  +-- allocate devices array
  +-- allocate links array
  |
  +-- any allocation failed:
  |     free partial allocations and return NULL
  |
  +-- return Topology
```

### Add Device

```text
topology_add_device(topo, dev)
  |
  +-- reject NULL topo or dev
  |
  +-- if device array full:
  |     realloc to double capacity
  |     failure: return -1
  |
  +-- devices[dev_count] = dev
  |
  +-- dev_count++
  |
  +-- return 0
```

### Find Link Between Interfaces

```text
topology_get_link_between(topo, iface_a, iface_b)
  |
  +-- reject NULL inputs
  |
  +-- for each live link:
        |
        +-- endpoints match A/B or B/A:
              return link
  |
  +-- return NULL
```

## ACSL Contracts

The contracts belong in `topology.h`. The useful properties are array shape,
count changes, and returned pointers coming from live entries.

### Shared Predicates

```c
/*@
    predicate topology_counts_valid(Topology *topo) =
        0 <= topo->dev_count &&
        topo->dev_count <= topo->dev_cap &&
        topo->dev_cap > 0 &&
        0 <= topo->link_count &&
        topo->link_count <= topo->link_cap &&
        topo->link_cap > 0;

    predicate topology_storage_valid(Topology *topo) =
        \valid(topo) &&
        topology_counts_valid(topo) &&
        topo->devices != \null &&
        topo->links != \null &&
        \valid(topo->devices + (0 .. topo->dev_cap - 1)) &&
        \valid(topo->links + (0 .. topo->link_cap - 1));

    predicate topology_live_devices_valid(Topology *topo) =
        topology_storage_valid(topo) &&
        (\forall integer i;
            0 <= i && i < topo->dev_count ==>
                device_well_formed(topo->devices[i]));

    predicate topology_live_links_valid(Topology *topo) =
        topology_storage_valid(topo) &&
        (\forall integer i;
            0 <= i && i < topo->link_count ==>
                link_well_formed(topo->links[i]));

    predicate topology_well_formed(Topology *topo) =
        topology_live_devices_valid(topo) &&
        topology_live_links_valid(topo);
*/
```

### `topology_create`

```c
/*@
    allocates \result;
    ensures \result == \null || topology_storage_valid(\result);
    ensures \result != \null ==> \result->dev_count == 0;
    ensures \result != \null ==> \result->dev_cap == 8;
    ensures \result != \null ==> \result->link_count == 0;
    ensures \result != \null ==> \result->link_cap == 8;
*/
Topology *topology_create(void);
```

### `topology_free`

```c
/*@
    assigns \nothing;
*/
void topology_free(Topology *topo);
```

Implementation rule: accept `NULL`; otherwise free live devices, live links,
arrays, and topology storage.

### `topology_add_device`

```c
/*@
    behavior null:
        assumes topo == \null || dev == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes topology_well_formed(topo) && device_well_formed(dev);
        assigns topo->devices,
                topo->devices[0 .. topo->dev_cap - 1],
                topo->dev_count,
                topo->dev_cap;
        ensures \result == 0 || \result == -1;
        ensures \result == 0 ==> topo->dev_count == \old(topo->dev_count) + 1;
        ensures \result == 0 ==> topo->devices[\old(topo->dev_count)] == dev;
        ensures \result == -1 ==> topo->dev_count == \old(topo->dev_count);
        ensures \result == 0 ==> topology_well_formed(topo);

    complete behaviors;
    disjoint behaviors;
*/
int topology_add_device(Topology *topo, Device *dev);
```

### `topology_add_link`

```c
/*@
    behavior null:
        assumes topo == \null || link == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes topology_well_formed(topo) && link_well_formed(link);
        assigns topo->links,
                topo->links[0 .. topo->link_cap - 1],
                topo->link_count,
                topo->link_cap;
        ensures \result == 0 || \result == -1;
        ensures \result == 0 ==> topo->link_count == \old(topo->link_count) + 1;
        ensures \result == 0 ==> topo->links[\old(topo->link_count)] == link;
        ensures \result == -1 ==> topo->link_count == \old(topo->link_count);
        ensures \result == 0 ==> topology_well_formed(topo);

    complete behaviors;
    disjoint behaviors;
*/
int topology_add_link(Topology *topo, Link *link);
```

### `topology_find_device_by_name`

```c
/*@
    behavior null:
        assumes topo == \null || name == \null;
        assigns \nothing;
        ensures \result == \null;

    behavior valid:
        assumes topology_well_formed((Topology *)topo);
        assumes name != \null;
        assigns \nothing;
        ensures \result == \null || device_well_formed(\result);

    complete behaviors;
    disjoint behaviors;
*/
Device *topology_find_device_by_name(const Topology *topo, const char *name);
```

Additional required proof/test property:

- A non-null result is one of `topo->devices[0 .. dev_count - 1]`.
- A non-null result has a name equal to `name`.

### `topology_find_device_by_ip`

```c
/*@
    behavior null:
        assumes topo == \null;
        assigns \nothing;
        ensures \result == \null;

    behavior valid:
        assumes topology_well_formed((Topology *)topo);
        assigns \nothing;
        ensures \result == \null || device_well_formed(\result);

    complete behaviors;
    disjoint behaviors;
*/
Device *topology_find_device_by_ip(const Topology *topo, uint32_t ip_addr);
```

Additional required proof/test property:

- A non-null result is one of the live topology devices.
- A non-null result owns at least one live interface with `ip_addr == ip_addr`.

### `topology_get_link_between`

```c
/*@
    behavior null:
        assumes topo == \null || iface_a == \null || iface_b == \null;
        assigns \nothing;
        ensures \result == \null;

    behavior valid:
        assumes topology_well_formed((Topology *)topo);
        assumes \valid_read(iface_a) && \valid_read(iface_b);
        assigns \nothing;
        ensures \result == \null || link_well_formed(\result);

    complete behaviors;
    disjoint behaviors;
*/
Link *topology_get_link_between(const Topology *topo,
                                const Interface *iface_a,
                                const Interface *iface_b);
```

Additional required proof/test property:

- A non-null result is one of `topo->links[0 .. link_count - 1]`.
- A non-null result connects `iface_a` and `iface_b` in either endpoint order.

### Count Accessors

```c
/*@
    behavior null:
        assumes topo == \null;
        assigns \nothing;
        ensures \result == 0;

    behavior valid:
        assumes topology_storage_valid((Topology *)topo);
        assigns \nothing;
        ensures \result == topo->dev_count;

    complete behaviors;
    disjoint behaviors;
*/
int topology_device_count(const Topology *topo);

/*@
    behavior null:
        assumes topo == \null;
        assigns \nothing;
        ensures \result == 0;

    behavior valid:
        assumes topology_storage_valid((Topology *)topo);
        assigns \nothing;
        ensures \result == topo->link_count;

    complete behaviors;
    disjoint behaviors;
*/
int topology_link_count(const Topology *topo);
```

## KLEVA Verification Plan

Minimum KLEVA tests:

1. `topology_create` returns either `NULL` or zero counts.
2. Successful create sets `dev_cap == 8`.
3. Successful create sets `link_cap == 8`.
4. `topology_free(NULL)` does not crash.
5. `topology_add_device(NULL, dev)` returns `-1`.
6. `topology_add_device(topo, NULL)` returns `-1`.
7. Successful add device increments `dev_count`.
8. Successful add device stores pointer at old count.
9. Add device grows capacity when full.
10. Realloc failure while adding device leaves `dev_count` unchanged.
11. `topology_add_link(NULL, link)` returns `-1`.
12. `topology_add_link(topo, NULL)` returns `-1`.
13. Successful add link increments `link_count`.
14. Successful add link stores pointer at old count.
15. Add link grows capacity when full.
16. Realloc failure while adding link leaves `link_count` unchanged.
17. Find device by name rejects NULL topology.
18. Find device by name rejects NULL name.
19. Find device by name returns the first matching live device.
20. Find device by name returns `NULL` when missing.
21. Find device by IP rejects NULL topology.
22. Find device by IP returns device owning matching interface IP.
23. Find device by IP returns `NULL` when missing.
24. Get link between rejects NULL inputs.
25. Get link between matches endpoint order A/B.
26. Get link between matches endpoint order B/A.
27. Get link between returns `NULL` when missing.
28. `topology_device_count(NULL)` returns `0`.
29. `topology_link_count(NULL)` returns `0`.
30. Count accessors return stored counts for valid topology.

## Common Mistakes

- Do not say topology creates devices or links; it only stores ownership after
  successful add.
- Do not read unused slots beyond `dev_count` or `link_count`.
- Do not claim duplicate device names are rejected; current code allows them.
- Do not claim duplicate links are rejected; current code allows them.
- Do not dereference link endpoints during `topology_free` unless the free order
  is changed.
- Do not compare links by interface name or MAC; current lookup uses pointer
  identity.
