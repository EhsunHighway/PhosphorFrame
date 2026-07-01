# Networking Simulator — Implementation Plan

Implementation order follows strict dependency: each module only calls modules
above it in the table. Numbers are sequential build order. Coverage targets:
≥90% line, ≥80% branch (kleva).

---

## Phase 1 — Core Infrastructure

| #  | Module              | File(s)                     | Depends On              | Status          | Tests (line / branch)  |
|----|---------------------|-----------------------------|-------------------------|-----------------|------------------------|
|  1 | Packet buffer       | `network/packet.c/h`        | stdlib                  | ✅ Done         | 93% / 80% ✅           |
|  2 | Event system        | `engine/event.c/h`          | packet                  | ✅ Done         | 92% / 81% ✅           |
|  3 | Scheduler           | `engine/scheduler.c/h`      | event                   | ✅ Done         | 95% / 84% ✅           |
|  4 | Interface (NIC)     | `network/interface.c/h`     | stdlib                  | ✅ Done         | 98% / 93% ✅           |
|  5 | Link                | `network/link.c/h`          | interface, packet       | ✅ Done         | 89% / 80% ⚠️           |
|  6 | Device              | `network/device.c/h`        | interface, link, packet | ✅ Done         | 93% / 83% ✅           |
|  7 | Topology            | `network/topology.c/h`      | device, link            | ✅ Done         | 92% / 83% ✅           |
|  8 | Simulator           | `engine/simulator.c/h`      | topology, scheduler     | ✅ Done         | 96% / 81% ✅           |

---

## Phase 2 — L2 Protocols & Nodes

| #  | Module              | File(s)                     | Depends On              | Status          | Tests (line / branch)  |
|----|---------------------|-----------------------------|-------------------------|-----------------|------------------------|
|  9 | Ethernet (L2)       | `protocols/ethernet.c/h`    | packet, interface       | ✅ Done         | 57% / 67% ❌           |
| 10 | ARP cache           | `protocols/arp_cache.c/h`   | interface, packet, ip   | ✅ Done         | —                      |
| 11 | ARP                 | `protocols/arp.c/h`         | ethernet, arp_cache     | ✅ Done         | 30% / 38% ❌           |
| 12 | MAC table           | `network/mac_table.c/h`     | interface               | ✅ Done         | 100% / 100% ✅         |
| 13 | Switch (L2)         | `network/switch.c/h`        | mac_table, ethernet     | ✅ Done         | 68% / 56% ❌           |

---

## Phase 3 — L3 Protocols & Host Node

| #  | Module              | File(s)                     | Depends On               | Status          | Tests (line / branch)  |
|----|---------------------|-----------------------------|--------------------------|-----------------|------------------------|
| 14 | IPv4                | `protocols/ip.c/h`          | ethernet, arp_cache, arp | ✅ Done         | 62% / 48% ❌           |
| 15 | ICMP                | `protocols/icmp.c/h`        | ip                       | ✅ Done         | 79% / 61% ❌           |
| 16 | UDP                 | `protocols/udp.c/h`         | ip, icmp                 | ✅ Done         | 80% / 73% ❌           |
| 17 | TCP                 | `protocols/tcp.c/h`         | ip                       | ✅ Done         | —                      |
| 18 | Host                | `network/host.c/h`          | device, arp_cache, ip    | ✅ Done         | —                      |

---

## Phase 4 — Routing & Router Node

| #  | Module              | File(s)                     | Depends On                         | Status          | Tests (line / branch)  |
|----|---------------------|-----------------------------|------------------------------------|-----------------|------------------------|
| 19 | Routing table       | `routing/route_table.c/h`   | device, ip                         | ✅ Done         | —                      |
| 20 | Router (L3)         | `network/router.c/h`        | device, arp_cache, route_table, ip | ⬜ Not started  | —                      |
| 21 | RIP                 | `routing/rip.c/h`           | route_table, router, scheduler     | ⬜ Not started  | —                      |
| 22 | OSPF                | `routing/ospf.c/h`          | route_table, router, scheduler     | ⬜ Not started  | —                      |
| 23 | BGP                 | `routing/bgp.c/h`           | route_table, router, scheduler     | ⬜ Not started  | —                      |
| 24 | EIGRP               | `routing/eigrp.c/h`         | route_table, router, scheduler     | ⬜ Not started  | —                      |
| 25 | IS-IS               | `routing/isis.c/h`          | route_table, router, scheduler     | ⬜ Not started  | —                      |
| 26 | NAT / PAT           | `routing/nat.c/h`           | route_table, ip                    | ⬜ Not started  | —                      |

---

## Phase 5 — Display + CLI

| #  | Module              | File(s)                     | Depends On           | Status          | Tests (line / branch)  |
|----|---------------------|-----------------------------|----------------------|-----------------|------------------------|
| 27 | Topology renderer   | `display/topology.c/h`      | topology             | ⬜ Not started  | —                      |
| 28 | Packet renderer     | `display/packet.c/h`        | packet, protocols    | ⬜ Not started  | —                      |
| 29 | CLI / REPL          | `cli/cli.c/h`               | simulator, all       | ⬜ Not started  | —                      |

---

## Dependency Graph

```
packet ─────────────────────────────────────────────┐
event ──────────────────────────────────────────┐   │
scheduler ──────────────────────────────────┐   │   │
                                            │   │   │
interface ──────────────┐                   │   │   │
link (interface)        │                   │   │   │
device (interface,link) │                   │   │   │
topology (device,link)  │                   │   │   │
simulator ──────────────┴───────────────────┘   │   │
                                                │   │
ethernet (packet, interface) ───────────────────┘   │
arp_cache (interface, packet, ip)                   │
arp (ethernet, arp_cache)                           │
mac_table (interface)                               │
switch (mac_table, ethernet)                        │
ip (ethernet, arp_cache, arp) ──────────────────────┘
icmp / udp / tcp (ip)
host (device, arp_cache, ip)
route_table (device, ip)
router (device, arp_cache, route_table, ip)
rip / ospf / bgp / eigrp / isis (route_table, router, scheduler)
nat (route_table, ip)
display / cli (everything)
```

---

## Key Design Rules

- Every `.c` file has a matching `.h` with ACSL contracts on all public functions
- Every module gets a `kleva/` YAML and reaches ≥90% line / ≥80% branch before moving on
- No module reaches upward (e.g. `interface.c` must not call `device.c`)
- `malloc` failure branches are excluded from coverage targets (guarded by `-eva-no-alloc-returns-null`)

## Latest Coverage Notes

Fresh coverage for Ethernet, ARP, Switch, IPv4, and ICMP was generated on
2026-06-11 under
`tests/coverage/project_plan_20260611/`.

UDP coverage was generated on 2026-06-16 under
`tests/coverage/udp_kleva_shapes/` after KLEVA generated 26 UDP test vectors
with 38 EVA-proven assertions and 0 unproven assertions.

- MAC table is above the coverage baseline.
- Ethernet, ARP, Switch, IPv4, and ICMP are implemented but below the
  required coverage baseline.
- UDP is implemented and has generated KLEVA tests, but remains below the
  required line and branch coverage baseline.
- Switch coverage compilation currently needs the generated test to see the
  system declaration for `htons`; the coverage run used `-include arpa/inet.h`.
