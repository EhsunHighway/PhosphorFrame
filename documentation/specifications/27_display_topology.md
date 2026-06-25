# Module 27 - Display Topology

**Files:** `src/display/topology_view.c`, `src/display/topology_view.h`
**Status:** Ready for implementation; current source files are empty
**Depends on:** `topology`, `device`, `interface`, `link`, `byte_order`

## Concepts First

Topology display is a read-only view of the simulated network graph.

It answers:

```text
What devices exist?
Which interfaces do they have?
Which interfaces are connected by links?
What IP, MAC, and state is visible on each interface?
```

This module must not simulate anything. It prints existing state.

### Display Is Not Simulation

Display code must not:

- add devices
- add links
- change interface state
- transmit packets
- schedule events
- free topology objects

It only reads `Topology`, `Device`, `Interface`, and `Link` fields and writes
formatted text to a `FILE *`.

### Graph View

The topology is a graph:

- devices are nodes
- links are edges
- interfaces are the endpoints of edges

The current `Topology` struct stores:

- `Device **devices`
- `Link **links`
- counts and capacities

The display should iterate the topology arrays by count, not by capacity.

### Device Type Limitation

Current `Device` has no `DeviceType` field.

That means topology display cannot reliably know whether a `Device *` is a
Host, Router, or Switch from the base struct alone.

First milestone behavior:

- print `[Device]` for all base `Device *` objects

Future behavior:

- add an explicit type tag to Device or use a safe wrapper/type registry
- then print `[Host]`, `[Router]`, or `[Switch]`

Do not infer type from unrelated fields or from the device name.

### Address Formatting

`Interface.ip_addr` is network order in the current stack.

Topology display should print IP addresses in dotted decimal. Therefore it must
convert from network order to host-order octets before formatting, or extract
octets consistently from the network-order value.

MAC addresses are six bytes and should print as lowercase hex:

```text
aa:bb:cc:11:22:33
```

### Plain Text Output

Use plain ASCII.

No ANSI color codes in the first milestone. Plain output is easier to test with
KLEVA wrappers, `fmemopen`, captured stdout, and log files.

## Purpose

The topology display module renders the current topology as text.

It provides:

- full topology printing
- one-device block printing
- one-interface line printing
- link list printing
- IP string formatting
- MAC string formatting

It does not:

- mutate topology state
- infer unavailable device types
- allocate long-lived state
- depend on simulator or scheduler
- print packets
- draw an interactive UI

## Architecture Boundary

| Responsibility | Owner |
| --- | --- |
| Store devices and links | Topology |
| Store interface fields | Interface |
| Store link fields | Link |
| Format topology text | Topology display |
| Simulate packets/events | Simulator/Scheduler |
| Packet-specific display | Display packet module |

Topology display may include standard C headers such as `<stdio.h>` and
`<stdint.h>`.

It should not include protocol modules except `byte_order` if needed for IP
formatting.

## Data Model

### Constants

```c
#define TV_LINE_WIDTH    80
#define TV_IFACE_INDENT  2
#define TV_MAX_LABEL_LEN 64
#define TV_IP_BUFSZ      16
#define TV_MAC_BUFSZ     18
```

IP buffer size `16` holds `"255.255.255.255"` plus NUL.

MAC buffer size `18` holds `"ff:ff:ff:ff:ff:ff"` plus NUL.

### Output Shape

Minimum full topology output:

```text
Topology - 2 devices, 1 links
--------------------------------------------------------------------------------
[Device] R1
  eth0  IP: 192.168.1.1/24  MAC: aa:bb:cc:11:22:33  UP

[Device] H1
  eth0  IP: 192.168.1.10/24  MAC: dd:ee:ff:00:00:01  UP

Links:
  R1:eth0 --[1000 Mbps / 1 ms / 0.0% loss / UP]-- H1:eth0
--------------------------------------------------------------------------------
```

Use ASCII separators and connectors.

### Null Field Display

If a valid topology contains a NULL device slot inside `0 .. dev_count - 1`,
print a placeholder line and continue:

```text
[Device] <null>
```

If a link endpoint is NULL, print `<null>` for that endpoint.

This keeps display robust for partially constructed test topologies.

## Ownership And Lifetime

Display borrows every pointer passed to it.

It does not free:

- `Topology *`
- `Device *`
- `Interface *`
- `Link *`
- `FILE *`

String formatting helpers write into caller-provided buffers. They do not
allocate.

## Public API

```c
int topology_view_print(const Topology *topo, FILE *out);

int topology_view_print_device(const Device *dev, FILE *out);

int topology_view_print_links(const Topology *topo, FILE *out);

int topology_view_print_iface(const Interface *iface, FILE *out);

int topology_view_sprint_ip(uint32_t ip_addr,
                            char *buf,
                            size_t bufsz);

int topology_view_sprint_mac(const uint8_t mac[6],
                             char *buf,
                             size_t bufsz);
```

All functions return `0` on success and `-1` on invalid required input.

Printing functions treat `FILE *out == NULL` as invalid input.

## Function Behavior

### `topology_view_sprint_ip`

Required behavior:

- If `buf == NULL`, return `-1`.
- If `bufsz < 16`, return `-1`.
- Convert `ip_addr` from the current Interface storage format to dotted
  decimal.
- Write a NUL-terminated string.
- Return `0`.

For an interface IP, callers pass `iface->ip_addr`, which is network order.

### `topology_view_sprint_mac`

Required behavior:

- If `mac == NULL || buf == NULL`, return `-1`.
- If `bufsz < 18`, return `-1`.
- Write lowercase colon-separated hex.
- Write a NUL-terminated string.
- Return `0`.

### `topology_view_print_iface`

Required behavior:

- If `iface == NULL || out == NULL`, return `-1`.
- Format IP and MAC.
- Print:
  - interface name
  - IP/prefix
  - MAC
  - administrative state `UP` or `DOWN`
  - link state `LINK-UP`, `LINK-DOWN`, or `UNLINKED`
- Return `0` if printing succeeds.

### `topology_view_print_device`

Required behavior:

- If `dev == NULL || out == NULL`, return `-1`.
- Print `[Device] name`.
- Iterate `dev->interfaces[0 .. iface_count - 1]`.
- For each non-NULL interface, call `topology_view_print_iface`.
- For NULL interface slots inside count, print a placeholder.
- Return `0` if all required printing succeeds.

Current type behavior: print `[Device]`, not `[Host]`, `[Router]`, or
`[Switch]`.

### `topology_view_print_links`

Required behavior:

- If `topo == NULL || out == NULL`, return `-1`.
- Print `Links:`.
- Iterate `topo->links[0 .. link_count - 1]`.
- For each link:
  - print endpoint device/interface labels when available
  - print bandwidth Mbps
  - print delay ms
  - print loss percentage
  - print link up/down state
- Return `0` if printing succeeds.

Endpoint device name comes from `link->end_a->device->name` and
`link->end_b->device->name` when available.

### `topology_view_print`

Required behavior:

- If `topo == NULL || out == NULL`, return `-1`.
- Print header with device and link counts.
- Print separator.
- Print each device block.
- Print link list.
- Print closing separator.
- Return `0` if all required printing succeeds.

## Flow Charts

### Full Print

```text
topology_view_print(topo, out)
  |
  +-- reject NULL topo/out
  +-- print header and separator
  +-- for each device slot < dev_count:
  |     topology_view_print_device
  |
  +-- topology_view_print_links
  +-- print separator
  +-- return 0
```

### Device Print

```text
topology_view_print_device(dev, out)
  |
  +-- print [Device] name
  +-- for each interface slot < iface_count:
        topology_view_print_iface
```

## ACSL Contracts

The contracts belong in `topology_view.h`. Use literal buffer sizes:

- IP string buffer: `16`
- MAC string buffer: `18`

### `topology_view_sprint_ip`

```c
/*@
    behavior bad_input:
        assumes buf == \null || bufsz < 16;
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes buf != \null;
        assumes bufsz >= 16;
        assigns buf[0 .. bufsz - 1];
        ensures \result == 0;
        ensures buf[0] != '\0';

    complete behaviors;
    disjoint behaviors;
*/
int topology_view_sprint_ip(uint32_t ip_addr, char *buf, size_t bufsz);
```

### `topology_view_sprint_mac`

```c
/*@
    behavior bad_input:
        assumes mac == \null || buf == \null || bufsz < 18;
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes mac != \null && buf != \null;
        assumes bufsz >= 18;
        assigns buf[0 .. bufsz - 1];
        ensures \result == 0;
        ensures buf[17] == '\0';

    complete behaviors;
    disjoint behaviors;
*/
int topology_view_sprint_mac(const uint8_t mac[6], char *buf, size_t bufsz);
```

### Print Functions

```c
/*@
    behavior bad_input:
        assumes topo == \null || out == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes \valid_read(topo);
        assumes out != \null;
        assigns \nothing;
        ensures \result == 0 || \result == -1;

    complete behaviors;
    disjoint behaviors;
*/
int topology_view_print(const Topology *topo, FILE *out);
```

Equivalent bad-input and valid-read contracts should be used for
`topology_view_print_device`, `topology_view_print_links`, and
`topology_view_print_iface`.

## KLEVA Verification Plan

Minimum KLEVA tests:

1. `topology_view_sprint_ip` rejects NULL buffer.
2. `topology_view_sprint_ip` rejects buffer smaller than 16.
3. IP formatting prints zero address.
4. IP formatting prints max address.
5. IP formatting prints a normal interface address correctly.
6. `topology_view_sprint_mac` rejects NULL MAC.
7. `topology_view_sprint_mac` rejects NULL buffer.
8. `topology_view_sprint_mac` rejects buffer smaller than 18.
9. MAC formatting uses lowercase hex and colons.
10. `topology_view_print` rejects NULL topology.
11. `topology_view_print` rejects NULL output stream.
12. Full print includes device count and link count.
13. Device print includes device name.
14. Interface print includes IP, prefix, MAC, and state.
15. Link print includes endpoint labels.
16. Link print includes bandwidth, delay, loss, and state.
17. Display functions do not mutate topology counts.
18. Display functions do not mutate interface counters or state.

## Common Mistakes

- Do not infer Host/Router/Switch type from the device name.
- Do not mutate topology while printing.
- Do not iterate to capacity; iterate to count.
- Do not assume every pointer inside a partially built topology is non-NULL.
- Do not print ANSI color codes in the first milestone.
- Do not use non-ASCII box drawing characters; keep output plain ASCII.
- Do not forget that `Interface.ip_addr` is network order.
