# Module 13 - Switch

**Files:** `src/network/switch.c`, `src/network/switch.h`
**Status:** Implemented
**Depends on:** `device`, `interface`, `packet`, `ethernet`, `mac_table`,
`event`, `scheduler`, `simulator`

## Concepts First

A switch is a layer-2 multi-port device. It forwards Ethernet frames based on
destination MAC address.

Unlike a hub, a switch learns where MAC addresses live:

```text
source MAC seen on ingress port -> remember MAC is reachable through that port
```

Then, when a frame arrives for a known destination MAC, the switch can send it
only to the correct port instead of flooding everywhere.

### Learning Switch Behavior

For every received frame:

1. Read the source MAC.
2. Learn `source MAC -> ingress port`.
3. Read the destination MAC.
4. If destination MAC is known, forward to that port.
5. If destination MAC is unknown or broadcast, flood.

```text
Frame arrives on port g0/1
  src = AA:AA:AA:AA:AA:AA
  dst = BB:BB:BB:BB:BB:BB

Learn:
  AA:AA:AA:AA:AA:AA -> g0/1

Lookup:
  BB:BB:BB:BB:BB:BB -> maybe known, maybe unknown
```

### Forward Versus Flood

Known unicast:

```text
MAC table has dst_mac -> egress port
egress is not ingress
egress is up
send to egress only
```

Flood:

```text
destination is broadcast, or destination is unknown
send a clone to every up linked port except ingress
```

Drop:

```text
destination maps back to ingress port
egress port is down
input is invalid
MAC table learn fails in current code
```

### Switch Is A Specialized Device

`Switch` embeds `Device` as its first field:

```c
typedef struct Switch {
    Device     base;
    MacTable   mac_tbl;
    int        port_count;
    Simulator *sim;
} Switch;
```

Because `base` is first, this cast is valid:

```c
(Device *)sw
```

The switch uses that cast to reuse `device_add_interface`,
`device_get_interface_by_name`, and `device_free`.

### Receive Handler Shim

Interfaces use a generic receive-handler shape:

```c
void (*RxHandler)(Interface *iface, Packet *pkt, uint16_t ethertype, void *ctx)
```

The switch registers a small static adapter on each switch port:

```text
switch_rx_shim(iface, pkt, ethertype, sw)
  -> switch_receive(sw, iface, pkt, ethertype)
```

That keeps the Interface module generic while letting switch ports deliver
frames into switch-specific forwarding logic.

### Stripped Ethernet Header

Ethernet receive strips the Ethernet header before calling the interface receive
handler. The packet's `data` pointer moves forward, but the bytes are still in
the buffer.

Switch receive recovers the old Ethernet header at:

```c
frame->data - ETH_HDR_LEN
```

It first checks:

```c
frame->data >= frame->head + ETH_HDR_LEN
```

This works because `packet_strip` moves `data`; it does not erase stripped
bytes.

### MAC Aging

Switch MAC entries are temporary. The switch schedules an `EVT_MAC_AGE` callback
event that periodically calls:

```c
mac_table_age(&sw->mac_tbl, sw->sim->sched->now)
```

The aging handler self-reschedules another callback event for:

```text
now + SW_AGE_INTERVAL
```

Each event carries its own `Switch *` context, so multiple switches can age
independently.

## Purpose

The switch module implements layer-2 learning and forwarding around the generic
Device/Interface infrastructure.

It provides:

- switch allocation
- switch destruction
- port insertion
- receive-handler registration on ports
- source MAC learning
- known-unicast forwarding
- broadcast/unknown flood
- port-down handling
- MAC table aging callback scheduling
- port lookup by name

It does not:

- parse IP, ARP, UDP, or TCP
- run spanning tree
- prevent loops
- own links
- implement VLANs
- implement multicast filtering
- implement STP/RSTP
- enforce duplicate switch names

## Architecture Boundary

| Responsibility | Owner |
| --- | --- |
| Own switch ports through embedded Device | Switch/Device |
| Learn source MAC addresses | Switch using MAC table |
| Store learned MAC mappings | MAC table |
| Recover stripped Ethernet header | Switch receive path |
| Prepend Ethernet header for outgoing frames | Ethernet |
| Schedule link delivery | Link/Scheduler via Ethernet |
| Age MAC entries periodically | Switch age event |
| Resolve IP-to-MAC | ARP, not Switch |
| Route IP packets | Router/IP, not Switch |

Switch is layer 2. It should not become a layer-3 router.

## Data Model

### Constants

```c
#define SW_MAX_PORTS    48
#define SW_AGE_INTERVAL 30000
```

### `Switch`

```c
typedef struct Switch {
    Device     base;
    MacTable   mac_tbl;
    int        port_count;
    Simulator *sim;
} Switch;
```

Field meanings:

| Field | Meaning |
| --- | --- |
| `base` | Embedded device. Must remain the first field. |
| `mac_tbl` | Embedded learned MAC table. |
| `port_count` | Number of live switch ports, synced from `base.iface_count`. |
| `sim` | Borrowed simulator pointer used for time and scheduling. |

`base.interfaces` is allocated by creating a temporary `Device`, copying its
struct into `sw->base`, and freeing only the temporary wrapper.

## Ownership And Lifetime

`switch_create` allocates one `Switch`.

During creation, it also obtains an owned `base.interfaces` array through
`device_create`.

`switch_free(sw)` calls:

```c
device_free((Device *)sw)
```

Because `Device base` is at offset zero, `device_free` frees:

- all interfaces owned by the embedded base
- `base.interfaces`
- the `Switch` allocation itself

Do not call `free(sw)` after `switch_free`.

After successful `switch_add_port`, the switch owns the interface through its
embedded device.

Current packet ownership caveat:

- Flood path creates clones and passes them to `ethernet_send`.
- Known-unicast path passes the original frame to `ethernet_send`.
- Current `ethernet_send`/`link_transmit` schedule another clone and do not free
  the caller packet.

That means the current switch send paths have ownership gaps unless a higher
layer or future Ethernet contract consumes those packets. The spec should not
hide this; tests should expose and settle it.

## Public API

```c
Switch    *switch_create(const char *name, Simulator *sim);

void       switch_free(Switch *sw);

int        switch_add_port(Switch *sw, Interface *iface);

void       switch_receive(Switch    *sw,
                          Interface *in_port,
                          Packet    *frame,
                          uint16_t   ethertype);

void       switch_port_down(Switch *sw, Interface *port);

Interface *switch_get_port_by_name(Switch *sw, const char *name);
```

## Function Behavior

### `mac_age_handler`

This function is static inside `switch.c`.

Required behavior:

- Ignore the event pointer.
- Cast `ctx` to `Switch *`.
- If `sw == NULL`, return.
- If `sw->sim == NULL`, return.
- If `sw->sim->sched == NULL`, return.
- Call `mac_table_age(&sw->mac_tbl, sw->sim->sched->now)`.
- Create a new callback event:
  - type `EVT_MAC_AGE`
  - timestamp `sw->sim->sched->now + SW_AGE_INTERVAL`
  - handler `mac_age_handler`
  - handler context `sw`
- If event creation succeeds, schedule it.
- If scheduling fails, free the new event.

This is per-switch callback scheduling, not a global fallback handler.

### `switch_rx_shim`

This function is static inside `switch.c`.

Required behavior:

- Cast `ctx` to `Switch *`.
- If `sw == NULL`, return.
- If `iface == NULL`, return.
- If `pkt == NULL`, return.
- Call `switch_receive(sw, iface, pkt, ethertype)`.

### `switch_create`

Required behavior:

- If `name == NULL`, return `NULL`.
- If `sim == NULL`, return `NULL`.
- Allocate one `Switch`.
- If allocation fails, return `NULL`.
- Clear the whole `Switch` object.
- Create a temporary `Device` with `SW_MAX_PORTS`.
- If device creation fails:
  - free the switch
  - return `NULL`
- Copy the temporary `Device` struct into `sw->base`.
- Free only the temporary `Device` wrapper.
- Set `port_count == 0`.
- Set `sim == sim`.
- Initialize `mac_tbl`.
- Create the first `EVT_MAC_AGE` callback event for
  `sim->sched->now + SW_AGE_INTERVAL`.
- If event creation fails or scheduling fails:
  - free the event if it exists
  - call `device_free((Device *)sw)`
  - return `NULL`
- Return the switch.

Current implementation assumes `sim->sched` is valid. It does not check
`sim->sched == NULL`.

### `switch_free`

Required behavior:

- If `sw == NULL`, return immediately.
- Call `device_free((Device *)sw)`.

Do not free `sw` again after this call.

### `switch_add_port`

Required behavior:

- If `sw == NULL`, return `-1`.
- If `iface == NULL`, return `-1`.
- If `sw->port_count >= SW_MAX_PORTS`, return `-1`.
- Call `device_add_interface((Device *)sw, iface)`.
- If device add fails, return that failure.
- If device add succeeds:
  - set interface receive handler to `switch_rx_shim`
  - set handler context to `sw`
  - set interface up with `interface_set_up(iface, 1)`
  - set `sw->port_count = sw->base.iface_count`
  - return `0`

`device_add_interface` sets `iface->device = (Device *)sw`.

### `switch_receive`

Required behavior:

- If `sw == NULL`, return immediately.
- If `in_port == NULL`, return immediately.
- If `frame == NULL`, return immediately.
- If `in_port` is down:
  - free `frame`
  - return
- If the stripped Ethernet header is not readable at
  `frame->data - ETH_HDR_LEN`:
  - free `frame`
  - increment `in_port->rx_errors`
  - return
- Copy source MAC and destination MAC from the stripped Ethernet header into
  local arrays.
- Learn source MAC on `in_port` using the current simulator time.
- If learning fails:
  - increment `in_port->rx_dropped`
  - free `frame`
  - return
- If destination MAC is broadcast, flood.
- Else if MAC lookup for destination returns `NULL`, flood.
- Else lookup destination again and treat it as known unicast.

Flood behavior:

- Iterate `sw->base.interfaces[0 .. sw->port_count - 1]`.
- For each `out_port`:
  - skip `in_port`
  - skip down ports
  - skip ports without a link
  - clone `frame`
  - if clone succeeds, call `ethernet_send(sw->sim, out_port, dst_mac,
    ethertype, clone)`
  - if clone fails, increment `in_port->rx_errors` and stop the flood loop
- Free the original `frame`.

Known-unicast behavior:

- If `egress != NULL`, `egress != in_port`, and `egress` is up:
  - call `ethernet_send(sw->sim, egress, dst_mac, ethertype, frame)`
- Otherwise:
  - increment `in_port->rx_dropped`
  - free `frame`

Current implementation note: known-unicast does not check `egress->link`
before calling `ethernet_send`.

### `switch_port_down`

Required behavior:

- If `sw == NULL`, return immediately.
- If `port == NULL`, return immediately.
- Set the port down with `interface_set_up(port, 0)`.
- Flush MAC table entries pointing at that port.

### `switch_get_port_by_name`

Required behavior:

- If `sw == NULL`, return `NULL`.
- If `name == NULL`, return `NULL`.
- Return `device_get_interface_by_name((Device *)sw, name)`.

## Flow Charts

### Create Switch

```text
switch_create(name, sim)
  |
  +-- reject NULL name or sim
  |
  +-- allocate Switch
  +-- memset Switch to zero
  |
  +-- dev = device_create(name, 48)
  |
  +-- copy *dev into sw->base
  +-- free temporary dev wrapper
  |
  +-- mac_table_init(&sw->mac_tbl)
  +-- sw->sim = sim
  |
  +-- create first MAC age callback event
  |
  +-- schedule event
  |
  +-- return switch
```

### Receive Frame

```text
switch_receive(sw, in_port, frame, ethertype)
  |
  +-- reject NULLs
  |
  +-- if ingress down:
  |     free frame, return
  |
  +-- if stripped Ethernet header is not readable:
  |     free frame
  |     rx_errors++
  |     return
  |
  +-- copy src_mac and dst_mac from old Ethernet header
  |
  +-- mac_table_learn(src_mac -> in_port)
  |
  +-- learn failed:
  |     rx_dropped++
  |     free frame
  |     return
  |
  +-- broadcast or unknown dst:
  |     flood clones, free original
  |
  +-- known unicast:
        |
        +-- egress usable and not ingress:
        |     ethernet_send(original frame)
        |
        +-- otherwise:
              rx_dropped++
              free frame
```

### MAC Age Event

```text
mac_age_handler(event, sw)
  |
  +-- reject missing sw/sim/sched
  |
  +-- mac_table_age(&sw->mac_tbl, current scheduler time)
  |
  +-- create next EVT_MAC_AGE callback event
  |
  +-- schedule it
  |
  +-- if schedule fails, free event
```

## ACSL Contracts

The contracts belong in `switch.h`. The static helpers should be covered by
KLEVA tests through observable effects: age-event scheduling and port receive
handler behavior.

### Shared Predicates

```c
/*@
    predicate switch_base_valid(Switch *sw) =
        \valid(sw) &&
        device_well_formed((Device *)sw) &&
        sw->base.iface_max == 48 &&
        0 <= sw->port_count &&
        sw->port_count == sw->base.iface_count &&
        sw->port_count <= 48;

    predicate switch_well_formed(Switch *sw) =
        switch_base_valid(sw) &&
        mac_table_well_formed(&sw->mac_tbl) &&
        sw->sim != \null;
*/
```

### `switch_create`

```c
/*@
    behavior bad_input:
        assumes name == \null || sim == \null;
        assigns \nothing;
        ensures \result == \null;

    behavior valid:
        assumes name != \null;
        assumes simulator_well_formed(sim);
        allocates \result;
        ensures \result == \null || switch_well_formed(\result);
        ensures \result != \null ==> \result->port_count == 0;
        ensures \result != \null ==> \result->base.iface_count == 0;
        ensures \result != \null ==> \result->base.iface_max == 48;
        ensures \result != \null ==> \result->mac_tbl.count == 0;
        ensures \result != \null ==> \result->sim == sim;

    complete behaviors;
    disjoint behaviors;
*/
Switch *switch_create(const char *name, Simulator *sim);
```

Additional required proof/test property:

- Successful create schedules one `EVT_MAC_AGE` callback event.
- The scheduled age event uses `mac_age_handler` with context `sw`.

### `switch_free`

```c
/*@
    behavior null:
        assumes sw == \null;
        assigns \nothing;

    behavior valid:
        assumes switch_well_formed(sw);
        assigns \nothing;

    complete behaviors;
    disjoint behaviors;
*/
void switch_free(Switch *sw);
```

Implementation rule: valid behavior frees through `device_free((Device *)sw)`.
Do not call `free(sw)` afterward.

### `switch_add_port`

```c
/*@
    behavior bad_input:
        assumes sw == \null || iface == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior full:
        assumes switch_well_formed(sw);
        assumes iface != \null;
        assumes sw->port_count >= 48;
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes switch_well_formed(sw);
        assumes interface_basic_valid(iface);
        assumes sw->port_count < 48;
        assigns sw->base.interfaces[0 .. sw->base.iface_count],
                sw->base.iface_count,
                sw->port_count,
                iface->device,
                iface->rx_handler,
                iface->handler_ctx,
                iface->up;
        ensures \result == 0 || \result == -1;
        ensures \result == 0 ==> sw->port_count == sw->base.iface_count;
        ensures \result == 0 ==> iface->device == (Device *)sw;
        ensures \result == 0 ==> iface->handler_ctx == sw;
        ensures \result == 0 ==> iface->up == 1;

    complete behaviors;
    disjoint behaviors;
*/
int switch_add_port(Switch *sw, Interface *iface);
```

Additional required proof/test property:

- On success, `iface->rx_handler` is the internal switch receive shim.
- On failure, `port_count` is unchanged.

### `switch_receive`

```c
/*@
    behavior bad_input:
        assumes sw == \null || in_port == \null || frame == \null;
        assigns \nothing;

    behavior valid:
        assumes switch_well_formed(sw);
        assumes interface_basic_valid(in_port);
        assumes packet_visible_bytes(frame);
        assigns sw->mac_tbl.entries[0 .. 1023],
                sw->mac_tbl.count,
                in_port->rx_dropped,
                in_port->rx_errors;

    complete behaviors;
    disjoint behaviors;
*/
void switch_receive(Switch *sw,
                    Interface *in_port,
                    Packet *frame,
                    uint16_t ethertype);
```

Additional required proof/test property:

- Down ingress port frees/drops the frame and does not learn.
- Missing stripped Ethernet header increments `rx_errors`.
- Valid receive attempts to learn source MAC before forwarding decision.
- Learn failure increments `rx_dropped` and frees the frame.
- Broadcast destination floods.
- Unknown destination floods.
- Known destination on ingress port drops.
- Known destination on up different egress calls `ethernet_send`.
- Flood skips ingress, down ports, and ports without links.

### `switch_port_down`

```c
/*@
    behavior bad_input:
        assumes sw == \null || port == \null;
        assigns \nothing;

    behavior valid:
        assumes switch_well_formed(sw);
        assumes \valid(port);
        assigns port->up,
                sw->mac_tbl.entries[0 .. 1023],
                sw->mac_tbl.count;
        ensures port->up == 0;
        ensures \forall integer i; 0 <= i && i < 1024 ==>
                (sw->mac_tbl.entries[i].valid == 0 ||
                 sw->mac_tbl.entries[i].port != port);

    complete behaviors;
    disjoint behaviors;
*/
void switch_port_down(Switch *sw, Interface *port);
```

### `switch_get_port_by_name`

```c
/*@
    behavior bad_input:
        assumes sw == \null || name == \null;
        assigns \nothing;
        ensures \result == \null;

    behavior valid:
        assumes switch_well_formed(sw);
        assumes name != \null;
        assigns \nothing;
        ensures \result == \null || \valid(\result);

    complete behaviors;
    disjoint behaviors;
*/
Interface *switch_get_port_by_name(Switch *sw, const char *name);
```

Additional required proof/test property:

- A non-null result is one of the switch base interfaces and has matching name.

## KLEVA Verification Plan

Minimum KLEVA tests:

1. `switch_create(NULL, sim)` returns `NULL`.
2. `switch_create(name, NULL)` returns `NULL`.
3. Successful create sets `base.iface_count == 0`.
4. Successful create sets `base.iface_max == 48`.
5. Successful create initializes MAC table count to zero.
6. Successful create stores simulator pointer.
7. Successful create schedules first MAC age event.
8. Age handler ages MAC table entries.
9. Age handler reschedules itself.
10. `switch_free(NULL)` does not crash.
11. `switch_add_port(NULL, iface)` returns `-1`.
12. `switch_add_port(sw, NULL)` returns `-1`.
13. `switch_add_port` rejects full switch.
14. Successful add increments embedded device interface count.
15. Successful add sets `iface->device`.
16. Successful add registers switch receive shim and context.
17. Successful add sets port up.
18. Successful add syncs `port_count`.
19. `switch_receive` ignores NULL switch, port, or frame.
20. Receive on down ingress frees/drops frame.
21. Receive with missing stripped Ethernet header increments `rx_errors`.
22. Valid receive learns source MAC on ingress port.
23. Learn failure increments `rx_dropped`.
24. Broadcast destination floods to eligible ports.
25. Unknown destination floods to eligible ports.
26. Flood skips ingress port.
27. Flood skips down ports.
28. Flood skips ports without links.
29. Flood frees original frame after clone attempts.
30. Known destination on different up port forwards once.
31. Known destination on ingress port drops.
32. Known destination on down port drops.
33. `switch_port_down(NULL, port)` is a no-op.
34. `switch_port_down(sw, NULL)` is a no-op.
35. `switch_port_down` sets port down.
36. `switch_port_down` flushes MAC entries for that port.
37. `switch_get_port_by_name` rejects NULL inputs.
38. `switch_get_port_by_name` returns matching port.

## Common Mistakes

- Do not move `Device base` away from offset zero.
- Do not call `free(sw)` after `switch_free`; `device_free((Device *)sw)`
  already frees the allocation.
- Do not read the Ethernet header at `frame->data`; after Ethernet receive it is
  at `frame->data - ETH_HDR_LEN`.
- Do not scan only MAC-table `count`; holes are possible.
- Do not flood back out the ingress port.
- Do not send to down ports in flood.
- Do not claim STP or loop prevention exists.
- Do not hide current packet ownership gaps in switch-to-Ethernet send paths.
