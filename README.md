# FrameRunner

FrameRunner is a C network simulator with a retro CRT-style terminal vision.
It models packet flow through OSI-inspired layers using ordinary C data
structures, event queues, protocol headers, devices, and links.

The project is under active development. The core packet, event, scheduler,
interface, link, device, topology, simulator, Ethernet, ARP, MAC table, switch,
IPv4, ICMP, UDP, and TCP source modules are present. Some files are scaffolding
or specifications for upcoming work, especially the CLI, display, routing, host,
and router surfaces.

```text
FRAMERUNNER :: packet view

time  000042us
path  host-01.eth0  ->  router-0.eth1
link  lan0

+------------------------- ETHERNET ------------------------+
| dst   02:00:00:00:00:fe                                   |
| src   02:00:00:00:00:01                                   |
| type  0x0800                                              |
+--------------------------- IPv4 --------------------------+
| src   10.0.0.10                dst   10.0.1.20            |
| ttl   64                       proto 17                   |
| len   60                       checksum 0x8f21            |
+--------------------------- UDP ---------------------------+
| sport 49152                    dport 53                   |
| len   40                       checksum 0x12a7            |
+-----------------------------------------------------------+

event queue
  now   TX_FRAME   t=000042us   bytes=60   status=dispatch
  next  RX_FRAME   t=000047us   dev=router-0.eth1
  next  ROUTE_LPM  t=000048us   dev=router-0
```

## Goals

- Simulate packet movement across Layer 2, Layer 3, and Layer 4 protocols.
- Keep packet headers and protocol state visible in C data structures.
- Drive simulation through an event queue rather than live network traffic.
- Provide a terminal-first display style using ASCII/CRT-style views.
- Keep module behavior documented with implementation-oriented specs.

## Current Scope

Implemented source areas include:

- Packet buffers
- Event and scheduler infrastructure
- Interfaces, links, devices, topology, and simulator state
- Ethernet
- ARP and ARP cache support
- MAC table and switch support
- IPv4
- ICMP
- UDP
- TCP
- Generated unit-test C files under `tests/unit/`

Scaffolded, planned, or incomplete areas include:

- Final CLI/REPL integration
- Terminal display renderer
- Host and router behavior completion
- Static and dynamic routing modules
- Build/test packaging
- End-user simulation scenarios

## Repository Layout

```text
documentation/
  ARCHITECTURE.md
  PROJECT_PLAN.md
  SPECIFICATION.md
  specifications/

src/
  cli/
  common/
  display/
  engine/
  network/
  protocols/
  routing/

tests/
  unit/
```

## Documentation

Start here:

- [Architecture](documentation/ARCHITECTURE.md)
- [Specification](documentation/SPECIFICATION.md)
- [Implementation Plan](documentation/PROJECT_PLAN.md)
- [Module Specifications](documentation/specifications/README.md)

The module specifications are intended to be close to the C implementation:
function contracts, packet ownership, protocol boundaries, and expected
success/failure behavior are documented per module.

## Building

FrameRunner does not yet ship a complete public build command or runnable CLI
application. The current focus is the simulator core, protocol modules, and
implementation specifications.

A proper build entry point will be added when the CLI/display layer is ready.

## Testing

FrameRunner uses generated C unit tests as part of its testing strategy. The
tests are produced with KLEVA, a KLEE + Frama-C EVA workflow for turning
contracted C modules into concrete unit tests.

At a high level:

- Public C functions are documented with ACSL annotations in the headers.
- KLEVA reads those contracts and generates KLEE harnesses for the target
  module.
- KLEE explores symbolic inputs and emits concrete `.ktest` executions.
- KLEVA converts those executions into Frama-C EVA probes.
- Frama-C EVA proves concrete output values where possible.
- KLEVA emits C unit tests that keep only EVA-proven assertions.

This keeps expected values tied to contract-guided symbolic execution and EVA
proofs rather than hand-written guesses.

Generated unit-test source files live in:

```text
tests/unit/
```

## License

FrameRunner is licensed under the MIT License. See [LICENSE](LICENSE).

## Provenance

This project was developed with LLM-assisted specification review and code
review. The implementation is original project code and does not intentionally
copy source code from other networking simulators.
