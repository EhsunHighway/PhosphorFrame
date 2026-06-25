# Module 29 - CLI

**Files:** `src/cli/cli.c`, `src/cli/cli.h`, `src/cli/commands.c`,
`src/cli/commands.h`
**Status:** Ready for implementation; current source files are empty
**Depends on:** `simulator`, `scheduler`, `topology`, `device`, `interface`,
`link`, `topology_view`, `header_view`, `icmp`, `route_table`, `arp_cache`

## Concepts First

The CLI is the command-line control surface for the simulator.

It lets a user or script:

- inspect topology
- inspect interfaces
- inspect ARP and routes when those owners exist
- step or run the scheduler
- stop the simulator
- change link state
- inject simple actions such as ping

The CLI is not the simulator. It is a parser and dispatcher around public APIs.

### Interactive Mode And Script Mode

The CLI reads text lines from a `FILE *`.

Interactive mode:

```text
in  = stdin
out = stdout
```

Script mode:

```text
in  = script file
out = stdout or log file
```

Both modes use the same parser and command handlers.

### Command Dispatch

A command line becomes tokens:

```text
show ip route R1
```

tokens:

```text
argv[0] = "show"
argv[1] = "ip"
argv[2] = "route"
argv[3] = "R1"
```

Some commands have multi-word names:

```text
show interfaces
show ip route
set link
add route
```

Dispatch must choose the longest matching command name. That means
`show ip route` wins over `show`.

### Terminal Output Boundary

Display modules accept `FILE *out`.

The CLI owns terminal/script interaction and decides which output stream to
pass to display helpers.

Other modules should not call `printf` for user-facing command output. They
should expose data or print to a provided `FILE *`.

### Command Return Codes

Use stable command return codes:

| Code | Meaning |
| ---: | --- |
| `0` | success |
| `1` | usage error |
| `-1` | runtime error |

The CLI may print usage when a handler returns `1`.

### Current Capability Boundaries

Some commands depend on modules that are still design-stage.

Examples:

- `show ip route` needs Router/RouteTable ownership.
- `add route` needs route-table and Router integration.
- `ping` depends on ICMP send and IP output limitations.

The CLI should expose the command names, but handlers must return a clear
runtime error when the underlying module is unavailable.

Do not fake success.

## Purpose

The CLI module parses command lines and dispatches built-in command handlers.

It provides:

- CLI state allocation and cleanup
- built-in command registration
- command registration for tests/extensions
- line tokenization
- longest-prefix command lookup
- read/eval loop
- command handlers for inspection and simulator control

It does not:

- own the simulator
- mutate internals without public APIs
- parse packet headers itself
- render topology itself when display helpers exist
- implement routing protocol logic

## Architecture Boundary

| Responsibility | Owner |
| --- | --- |
| Read command lines | CLI |
| Parse tokens | CLI |
| Dispatch handlers | CLI |
| Print topology | Topology display |
| Print packet headers | Header display |
| Run/step/stop simulation | Simulator/Scheduler APIs |
| Store network state | Topology/Device/Protocol modules |
| Route-table algorithms | RouteTable |
| Packet forwarding | Router/IP |

Command handlers should use public APIs from the owning modules.

## Data Model

### Constants

```c
#define CLI_MAX_ARGS     16
#define CLI_LINE_BUF     256
#define CLI_PROMPT       "sim> "
#define CLI_MAX_COMMANDS 64
#define CLI_CMD_NAME_LEN 32
```

### `CliCommand`

```c
typedef int (*CliHandler)(CliState *state, int argc, char **argv);

typedef struct CliCommand {
    char        name[CLI_CMD_NAME_LEN];
    CliHandler  handler;
    const char *usage;
} CliCommand;
```

Handler receives `CliState *`, not only `Simulator *`, so it can use:

- `state->sim`
- `state->out`
- `state->running`
- registered command table for help

### `CliState`

```c
typedef struct CliState {
    CliCommand cmds[CLI_MAX_COMMANDS];
    int        cmd_count;

    Simulator *sim;
    FILE      *in;
    FILE      *out;
    int        running;
} CliState;
```

The CLI borrows `Simulator *`, `FILE *in`, and `FILE *out`.

## Ownership And Lifetime

`cli_create` allocates `CliState`.

`cli_free` frees only the `CliState`.

CLI does not free:

- simulator
- topology
- scheduler
- input stream
- output stream

`cli_exec_line` tokenizes the input line in place. The caller must pass a
writable buffer.

Command names and usage strings are copied or borrowed as documented:

- command name is copied into `CliCommand.name`
- usage pointer may point to static storage

## Public API

```c
typedef struct CliState CliState;

typedef int (*CliHandler)(CliState *state, int argc, char **argv);

CliState *cli_create(Simulator *sim, FILE *in, FILE *out);

void cli_free(CliState *state);

int cli_register(CliState *state,
                 const char *name,
                 CliHandler handler,
                 const char *usage);

int cli_loop(CliState *state);

int cli_exec_line(CliState *state, char *line);

const CliCommand *cli_find_command(const CliState *state,
                                   int argc,
                                   char **argv,
                                   int *matched_words);
```

## Built-In Commands

Minimum built-ins:

| Command | Usage |
| --- | --- |
| `show topology` | `show topology` |
| `show interfaces` | `show interfaces [device]` |
| `show arp` | `show arp [device]` |
| `show ip route` | `show ip route [device]` |
| `ping` | `ping <src_device> <dst_ip> [count]` |
| `set link` | `set link <dev>:<iface> up|down` |
| `add route` | `add route <dev> <prefix/len> <next-hop> <iface>` |
| `run` | `run [end_time_us]` |
| `step` | `step [n]` |
| `stop` | `stop` |
| `help` | `help [command]` |
| `exit` | `exit` |

Handlers belong in `commands.c` and are declared in `commands.h`.

## Function Behavior

### `cli_create`

Required behavior:

- If `sim == NULL || in == NULL || out == NULL`, return `NULL`.
- Allocate and zero `CliState`.
- Store borrowed pointers.
- Set `running = 1`.
- Register built-in commands.
- Return CLI state.

On allocation or registration failure, free the state and return `NULL`.

### `cli_free`

Required behavior:

- If `state == NULL`, return.
- Free only `state`.

### `cli_register`

Required behavior:

- If `state == NULL || name == NULL || handler == NULL`, return `-1`.
- If command table is full, return `-1`.
- Reject empty command name.
- Reject duplicate command name.
- Copy name into fixed command name buffer with NUL termination.
- Store handler and usage.
- Increment command count.
- Return `0`.

### `cli_find_command`

Required behavior:

- If state, argv, or matched_words is NULL, return NULL.
- Search registered commands.
- Treat command name as one or more space-separated words.
- Choose the command whose words match the beginning of argv and whose word
  count is largest.
- Store matched word count.
- Return command pointer or NULL.

Example:

```text
argv: show ip route R1
matches:
  show
  show ip
  show ip route
winner:
  show ip route
```

### `cli_exec_line`

Required behavior:

- If `state == NULL || line == NULL`, return `-1`.
- Strip trailing newline and carriage return.
- Ignore empty lines and comment lines beginning with `#`.
- Tokenize in place on spaces and tabs.
- If token count exceeds `CLI_MAX_ARGS`, print error and return `1`.
- Find longest matching command.
- If none found, print unknown-command error and return `1`.
- Call handler with full argc/argv.
- If handler returns `1` and command usage exists, print usage.
- Return handler result.

### `cli_loop`

Required behavior:

- If `state == NULL`, return `-1`.
- Set `running = 1`.
- While `running` and input has lines:
  - if input is interactive, print prompt
  - read line into fixed buffer
  - call `cli_exec_line`
- Return `0` on EOF or exit command.

### `cmd_show_topology`

Required behavior:

- If `state->sim == NULL || state->sim->topo == NULL`, return `-1`.
- Call `topology_view_print(state->sim->topo, state->out)`.
- Return result.

### `cmd_show_interfaces`

Required behavior:

- With no device argument, print every device and interface.
- With device argument, find device by name and print its interfaces.
- Return `1` for wrong argument count.
- Return `-1` if device is not found.

### `cmd_show_arp`

Required behavior:

- If no device argument, print a clear "device required" usage error until a
  topology-wide ARP cache iterator exists.
- With device argument, find device.
- If device is a Host or Router with public ARP cache access, print entries.
- If current type information cannot safely identify ARP cache ownership,
  return `-1` with a clear message.

Current limitation: base `Device` has no type tag. Do not guess Host/Router by
name.

### `cmd_show_route`

Required behavior:

- With device argument, find device.
- If device is a Router with route table access, print route table.
- If Router/RouteTable integration is not implemented, return `-1` with a
  clear message.

### `cmd_ping`

Required behavior:

- Parse `src_device`, destination IP, optional count.
- Find source device.
- Choose source interface/IP by public helper or first usable interface.
- Call `icmp_send_echo_request`.
- Step/run simulator enough for scheduled events according to command design.
- Print success/failure per request.

Current limitation: if ICMP/IP output cannot reach the destination because of
same-subnet or route limitations, report failure rather than pretending ping
succeeded.

### `cmd_set_link`

Required behavior:

- Parse `<dev>:<iface>` and `up|down`.
- Find device and interface.
- Find interface link.
- Call `link_set_up`.
- Return `0` on success.

### `cmd_add_route`

Required behavior:

- Parse device, prefix/length, next hop, and interface.
- Find Router and interface.
- Call Router/RouteTable public add-route API.
- Return `-1` if Router route-table integration is unavailable.

### Simulator Control Commands

`cmd_run`:

- Optional end time is microseconds.
- If provided, call `simulator_set_end_time`.
- Call `simulator_run`.

`cmd_step`:

- Default count is `1`.
- Call `simulator_step` count times or until no event remains.

`cmd_stop`:

- Call `simulator_stop`.

`cmd_exit`:

- Set `state->running = 0`.
- Return `0`.

`cmd_help`:

- With no argument, print registered command names and usage.
- With command words, print matching command usage.

## Flow Charts

### Execute One Line

```text
cli_exec_line(state, line)
  |
  +-- strip newline
  +-- empty/comment? return 0
  +-- tokenize in place
  +-- find longest command match
  |
  +-- no match:
  |     print unknown command
  |     return 1
  |
  +-- call handler
  +-- print usage if handler returned 1
  +-- return handler result
```

### CLI Loop

```text
cli_loop(state)
  |
  +-- running = 1
  +-- while running and fgets succeeds:
        print prompt if interactive
        cli_exec_line
  +-- return 0
```

## ACSL Contracts

The contracts belong in `cli.h`. Use literal bounds:

- max arguments: `16`
- line buffer: `256`
- max commands: `64`
- command name length: `32`

### Shared Predicates

```c
/*@
    predicate cli_command_count_valid(CliState *state) =
        0 <= state->cmd_count && state->cmd_count <= 64;

    predicate cli_state_well_formed(CliState *state) =
        \valid(state) &&
        cli_command_count_valid(state) &&
        state->sim != \null &&
        state->in != \null &&
        state->out != \null;
*/
```

### `cli_create`

```c
/*@
    behavior bad_input:
        assumes sim == \null || in == \null || out == \null;
        assigns \nothing;
        ensures \result == \null;

    behavior valid:
        assumes sim != \null && in != \null && out != \null;
        allocates \result;
        ensures \result == \null || \valid(\result);
        ensures \result != \null ==> \result->sim == sim;
        ensures \result != \null ==> \result->in == in;
        ensures \result != \null ==> \result->out == out;
        ensures \result != \null ==> \result->running == 1;

    complete behaviors;
    disjoint behaviors;
*/
CliState *cli_create(Simulator *sim, FILE *in, FILE *out);
```

### `cli_register`

```c
/*@
    behavior bad_input:
        assumes state == \null || name == \null || handler == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes cli_state_well_formed(state);
        assumes name != \null && handler != \null;
        assigns state->cmds[0 .. 63],
                state->cmd_count;
        ensures \result == 0 || \result == -1;
        ensures \result == 0 ==>
                state->cmd_count == \old(state->cmd_count) + 1;

    complete behaviors;
    disjoint behaviors;
*/
int cli_register(CliState *state,
                 const char *name,
                 CliHandler handler,
                 const char *usage);
```

### `cli_exec_line`

```c
/*@
    behavior bad_input:
        assumes state == \null || line == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes cli_state_well_formed(state);
        assumes line != \null;
        assigns line[0 .. 255],
                state->running;
        ensures \result == 0 || \result == 1 || \result == -1;

    complete behaviors;
    disjoint behaviors;
*/
int cli_exec_line(CliState *state, char *line);
```

### `cli_loop`

```c
/*@
    behavior bad_input:
        assumes state == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes cli_state_well_formed(state);
        assigns state->running;
        ensures \result == 0 || \result == -1;

    complete behaviors;
    disjoint behaviors;
*/
int cli_loop(CliState *state);
```

## KLEVA Verification Plan

Minimum KLEVA tests:

1. `cli_create` rejects NULL simulator, input, and output.
2. Valid create stores simulator/input/output and sets running.
3. Built-in registration creates nonzero command count.
4. `cli_register` rejects NULL state, name, and handler.
5. `cli_register` rejects duplicate command.
6. `cli_register` rejects full command table.
7. `cli_exec_line` rejects NULL state and line.
8. Empty line returns success.
9. Comment line returns success.
10. Too many tokens returns usage error.
11. Unknown command returns usage error.
12. Longest-prefix dispatch chooses `show ip route` over `show`.
13. `exit` sets running to zero.
14. `help` prints registered commands.
15. `show topology` calls topology display.
16. `step 3` calls simulator step three times unless events end.
17. `run` calls simulator run.
18. `stop` calls simulator stop.
19. `set link dev:iface down` calls link state change.
20. Commands depending on unavailable Router/RouteTable integration report
    runtime error instead of success.

## Common Mistakes

- Do not let CLI own or free the simulator.
- Do not guess Host/Router/Switch type from device name.
- Do not parse into more than `CLI_MAX_ARGS`.
- Do not match `show` when `show ip route` is the longest command match.
- Do not use milliseconds for simulator end time if the simulator uses
  microseconds.
- Do not let command handlers write to hard-coded stdout when `state->out`
  exists.
- Do not fake command success when the underlying module is not implemented.
