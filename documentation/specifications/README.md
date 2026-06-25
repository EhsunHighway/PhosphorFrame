# Module Specifications

This directory contains one implementation specification per simulator module.

Each numbered module spec is written to teach the concept first, then define
the implementation contract clearly enough that the module can be implemented
and tested without guessing.

## Standard Format

Every numbered spec uses this structure:

1. Concepts First
2. Purpose
3. Architecture Boundary
4. Data Model
5. Ownership And Lifetime
6. Public API
7. Function Behavior
8. Flow Charts
9. ACSL Contracts
10. KLEVA Verification Plan
11. Common Mistakes

For implemented modules, the spec should match the current `src/` behavior.

For future modules, the spec is a design contract. When implementation changes
the real behavior, update the spec in the same change.

## Phase 1 - Core Infrastructure

| # | Module | Spec | Spec Status |
| --- | --- | --- | --- |
| 01 | Packet | [01_packet.md](01_packet.md) | Updated |
| 02 | Event | [02_event.md](02_event.md) | Updated |
| 03 | Scheduler | [03_scheduler.md](03_scheduler.md) | Updated |
| 04 | Interface | [04_interface.md](04_interface.md) | Updated |
| 05 | Link | [05_link.md](05_link.md) | Updated |
| 06 | Device | [06_device.md](06_device.md) | Updated |
| 07 | Topology | [07_topology.md](07_topology.md) | Updated |
| 08 | Simulator | [08_simulator.md](08_simulator.md) | Updated |

## Phase 2 - Protocols

| # | Module | Spec | Spec Status |
| --- | --- | --- | --- |
| 09 | Ethernet | [09_ethernet.md](09_ethernet.md) | Updated |
| 10 | ARP Cache | [10_arp_cache.md](10_arp_cache.md) | Updated |
| 11 | ARP | [11_arp.md](11_arp.md) | Updated |
| 12 | MAC Table | [12_mac_table.md](12_mac_table.md) | Updated |
| 13 | Switch | [13_switch.md](13_switch.md) | Updated |
| 14 | IPv4 | [14_ip.md](14_ip.md) | Updated |
| 15 | ICMP | [15_icmp.md](15_icmp.md) | Updated |
| 16 | UDP | [16_udp.md](16_udp.md) | Updated |
| 17 | TCP | [17_tcp.md](17_tcp.md) | Updated |
| 18 | Host | [18_host.md](18_host.md) | Updated |

## Phase 3 - Routing

| # | Module | Spec | Spec Status |
| --- | --- | --- | --- |
| 19 | Route Table | [19_route_table.md](19_route_table.md) | Updated |
| 20 | Router | [20_router.md](20_router.md) | Updated |
| 21 | RIP | [21_rip.md](21_rip.md) | Updated |
| 22 | OSPF | [22_ospf.md](22_ospf.md) | Updated |
| 23 | BGP | [23_bgp.md](23_bgp.md) | Updated |
| 24 | EIGRP | [24_eigrp.md](24_eigrp.md) | Updated |
| 25 | IS-IS | [25_isis.md](25_isis.md) | Updated |
| 26 | NAT / PAT | [26_nat.md](26_nat.md) | Updated |

## Phase 4 - Display And CLI

| # | Module | Spec | Spec Status |
| --- | --- | --- | --- |
| 27 | Topology Display | [27_display_topology.md](27_display_topology.md) | Updated |
| 28 | Packet Header Display | [28_display_packet.md](28_display_packet.md) | Updated |
| 29 | CLI | [29_cli.md](29_cli.md) | Updated |

## How To Use These Files

Read `Concepts First` before coding. That section defines the vocabulary,
algorithm, and module boundaries.

Use `Function Behavior` as the implementation checklist.

Use `ACSL Contracts` as the header-contract target.

Use `KLEVA Verification Plan` as the generated-test checklist.

Use `Common Mistakes` as a review checklist before committing.

When code and spec disagree, do not guess. Either update the code to satisfy
the spec or update the spec to document the real behavior.
