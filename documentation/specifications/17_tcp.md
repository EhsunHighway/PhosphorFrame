# Module 17 - TCP

**Files:** `src/protocols/tcp.c`, `src/protocols/tcp.h`
**Status:** Implemented small stop-and-wait TCP
**Depends on:** `ip`, `packet`, `interface`, `scheduler`, `event`, `simulator`,
`byte_order`

## Concepts First

TCP is a transport protocol above IPv4.

UDP sends independent datagrams. TCP builds a connection and presents bytes as
an ordered stream.

This simulator implements a deliberately small TCP:

- one TCP table per protocol owner
- active open with SYN
- passive open with SYN-ACK
- final ACK transition to established
- stop-and-wait data transfer
- one queued unacknowledged segment per connection
- retransmission timer for queued segments
- FIN-based close

It does not implement:

- sliding windows beyond one in-flight segment
- congestion control
- receive reassembly
- TCP options
- TCP checksum validation
- random initial sequence numbers
- RST generation

### TCP Is Per Host, Not Global

TCP state is not global to the simulator.

Two different hosts can both listen on port `80`. Therefore each host, router,
or protocol owner that uses TCP owns its own `TcpTable`.

The IP receive path stores a `TcpContext` as opaque protocol context for
`IPPROTO_TCP`. That context lets `tcp_receive` find:

- the simulator, for output and timers
- the TCP table owned by this host or protocol owner

IP must not inspect TCP connections.

### TCB

`Tcb` means Transmission Control Block.

A TCB is one TCP connection record. It is not a packet and it is not a TCP wire
header. It is the memory TCP keeps so it can understand future packets for the
same flow.

A TCB stores:

- local IP and port
- remote IP and port
- TCP state
- send sequence numbers
- receive sequence numbers
- one-entry retransmission queue
- application callbacks

There are three common TCB roles:

| Role | State | Meaning |
| --- | --- | --- |
| Listener | `TCP_LISTEN` | Waits for incoming SYN packets on a local port. |
| Active opener | `TCP_SYN_SENT` | Created by `tcp_connect`; waits for SYN-ACK. |
| Connected child | `TCP_SYN_RECEIVED` or later | Created from a listener when a SYN arrives. |

A listener TCB is not the connection itself. When a SYN arrives for a listener,
TCP allocates a child TCB with the full peer address and port.

### Four-Tuple Lookup

A connected TCP flow is identified by four values:

```text
local_ip, local_port, remote_ip, remote_port
```

For a received packet:

- local values come from the packet destination
- remote values come from the packet source

TCP first searches for an exact connected TCB. If no exact TCB exists and the
received segment is exactly SYN, TCP searches for a listener on the destination
IP and port.

### Sequence Numbers

TCP numbers bytes.

Each TCB tracks:

| Field | Meaning |
| --- | --- |
| `snd_una` | Oldest sent sequence number not yet acknowledged. |
| `snd_nxt` | Next sequence number this side will send. |
| `rcv_nxt` | Next sequence number expected from the peer. |

`SYN` consumes one sequence number.

`FIN` consumes one sequence number.

Pure `ACK` does not consume a sequence number.

This implementation uses initial sequence number `0` for both active and
passive opens. That is deterministic and easy to test, but it is not production
TCP behavior.

### Stop-And-Wait

The send queue has one slot:

```c
#define TCP_MAX_INFLIGHT 1
```

That means the module can have at most one unacknowledged SYN, SYN-ACK, data
segment, or FIN at a time.

If a data segment or FIN is still queued, `tcp_send` or `tcp_close` fails
instead of sending another segment.

### Packet Clone For Retransmission

`ip_output` takes ownership of the packet it sends.

TCP cannot keep the same `Packet *` after handing it to IP. For retransmittable
segments, `tcp_send_segment` clones the packet before sending the original to
IP. The clone is stored in the TCB send queue.

Segments that get a clone:

- SYN
- SYN-ACK
- data
- FIN-ACK

Pure ACKs do not get a clone.

### Duplicate ACK

If payload arrives out of order, TCP cannot deliver it because this module has
no receive reassembly buffer.

Current behavior:

- send a pure ACK for the current `rcv_nxt`
- free the received out-of-order packet
- return `0`

This is a duplicate ACK: it says "I am still waiting for byte `rcv_nxt`."

### Close States

FIN closes one sending direction.

Important close states:

| State | Meaning |
| --- | --- |
| `TCP_FIN_WAIT_1` | Local side sent FIN and waits for ACK of that FIN. |
| `TCP_FIN_WAIT_2` | Local FIN was ACKed; waiting for peer FIN. |
| `TCP_CLOSE_WAIT` | Peer sent FIN; local application has not closed yet. |
| `TCP_LAST_ACK` | Local side sent FIN after peer FIN; waiting for ACK. |
| `TCP_TIME_WAIT` | Close handshake is done; absorb late duplicate packets. |

Current implementation note: `TCP_CLOSING` exists in the enum and receive
switch, but the implemented `TCP_FIN_WAIT_1` branch does not enter it. The code
handles ACK of our FIN in `FIN_WAIT_1`; it does not implement simultaneous
close from `FIN_WAIT_1`.

## Purpose

The TCP module provides a small deterministic connection-oriented transport for
the simulator.

It provides:

- TCP header layout and constants
- TCP table initialization
- passive open with listener TCBs
- active open with SYN
- SYN/SYN-ACK/ACK handshake
- stop-and-wait data send
- in-order payload receive and callback dispatch
- ACK processing for one queued segment
- retransmission event handling
- FIN-based local and remote close
- TIME_WAIT release timer

It does not:

- allocate `TcpTable`
- register itself with IP
- own the simulator
- compute TCP checksums
- implement options or variable header sizes
- buffer out-of-order payload
- implement full RFC 793 edge cases

## Architecture Boundary

| Responsibility | Owner |
| --- | --- |
| Store TCBs | `TcpTable` |
| Own `TcpTable` storage | Host/router/protocol owner |
| Register TCP with IP | Stack owner |
| Parse TCP header | TCP |
| Maintain connection state | TCP |
| Maintain retransmission queue | TCP |
| Schedule retransmission/TIME_WAIT events | TCP through scheduler |
| Build IPv4 header and resolve next hop | IP output |
| Free delivered payload | TCP receive callback owner |

TCP calls `ip_output` to send packets.

IP calls `tcp_receive` only through the registered protocol handler for
`IPPROTO_TCP`.

## Data Model

### Constants

```c
#define TCP_HDR_LEN         20
#define TCP_HDR_WORDS       5
#define TCP_FLAG_FIN        0x01
#define TCP_FLAG_SYN        0x02
#define TCP_FLAG_RST        0x04
#define TCP_FLAG_PSH        0x08
#define TCP_FLAG_ACK        0x10
#define TCP_FLAG_URG        0x20
#define TCP_MAX_CONNS       64
#define TCP_MAX_INFLIGHT    1
#define TCP_RETRANSMIT_US   1000000ULL
#define TCP_TIME_WAIT_US    (2 * TCP_RETRANSMIT_US)
#define TCP_MSS             1460
#define TCP_DEFAULT_WINDOW  65535
```

`IPPROTO_TCP` is defined by IP as protocol number `6`.

### `TcpHeader`

```c
typedef struct __attribute__((packed)) TcpHeader {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq_num;
    uint32_t ack_num;
    uint16_t data_off_flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent_ptr;
} TcpHeader;
```

Wire layout:

```text
offset  size  field
0       2     source port
2       2     destination port
4       4     sequence number
8       4     acknowledgment number
12      2     data offset and flags
14      2     window
16      2     checksum, currently zero
18      2     urgent pointer, currently zero
20            payload begins
```

All TCP header integer fields are network byte order on the wire.

`data_off_flags` in host order:

```text
high 4 bits: TCP header length in 32-bit words
low  6 bits: FIN/SYN/RST/PSH/ACK/URG flags
```

Current implementation accepts only `TCP_HDR_WORDS == 5`, meaning no TCP
options.

### `TcpState`

```c
typedef enum TcpState {
    TCP_CLOSED,
    TCP_LISTEN,
    TCP_SYN_SENT,
    TCP_SYN_RECEIVED,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_CLOSE_WAIT,
    TCP_CLOSING,
    TCP_LAST_ACK,
    TCP_TIME_WAIT
} TcpState;
```

`TCP_CLOSED` is a named state, but released TCB slots are represented by
`valid == 0`.

### Callbacks

```c
typedef void (*TcpRecvHandler)(Tcb *tcb, Packet *payload, void *ctx);
typedef void (*TcpConnectHandler)(Tcb *tcb, void *ctx);
```

`TcpRecvHandler` receives ownership of the stripped payload packet.

`TcpConnectHandler` is called when a TCB becomes established.

Both callbacks may be `NULL`.

### `TcpSegment`

```c
typedef struct TcpSegment {
    Packet   *pkt;
    uint32_t  seq_start;
    uint32_t  seq_end;
    uint8_t   flags;
    uint64_t  sent_ts;
    int       retransmits;
    int       acked;
} TcpSegment;
```

This is a retransmission queue entry. `pkt` is a clone, not the packet already
given to IP.

### `TcbSendQueue`

```c
typedef struct TcbSendQueue {
    TcpSegment entries[TCP_MAX_INFLIGHT];
    int        count;
} TcbSendQueue;
```

Current maximum count is `1`.

### `Tcb`

```c
struct Tcb {
    uint32_t           local_ip;
    uint32_t           remote_ip;
    uint16_t           local_port;
    uint16_t           remote_port;

    TcpState           state;
    uint32_t           snd_una;
    uint32_t           snd_nxt;
    uint32_t           rcv_nxt;
    uint16_t           rcv_wnd;
    uint16_t           snd_wnd;

    TcbSendQueue       sendq;
    uint64_t           retransmit_ts;

    TcpRecvHandler     recv_handler;
    TcpConnectHandler  connect_handler;
    void              *handler_ctx;

    int                valid;
};
```

IP addresses and ports in TCBs are host order.

### `TcpTable`

```c
typedef struct TcpTable {
    Tcb tcbs[TCP_MAX_CONNS];
    int count;
} TcpTable;
```

`count` is the number of valid TCB slots.

### `TcpContext`

```c
typedef struct TcpContext {
    Simulator *sim;
    TcpTable  *table;
} TcpContext;
```

This is the opaque context registered with IP:

```c
ip_stack_register_protocol(stack, IPPROTO_TCP, tcp_receive, &tcp_ctx);
```

## Ownership And Lifetime

`tcp_init` initializes existing `TcpTable` storage. It does not allocate.

`tcp_listen` allocates one TCB slot from the table.

`tcp_connect` allocates one TCB slot, sends a SYN, stores a SYN clone in the
send queue, and releases the TCB on local failure.

`tcp_receive` consumes every non-NULL packet passed to it:

- malformed receive paths free the packet
- no matching TCB frees the packet and increments `rx_dropped`
- data receive strips the TCP header and transfers packet ownership to
  `recv_handler`
- if no receive handler exists, TCP frees the stripped packet

`tcp_send` creates a packet and sends it through IP. If the send succeeds, IP
owns the original packet and the TCB send queue owns the clone.

`tcp_close` creates and queues a FIN-ACK clone. If the send succeeds, IP owns
the original packet and the TCB send queue owns the clone.

`tcp_sendq_ack` frees acknowledged queued clones.

`tcp_release_tcb` frees queued clones before zeroing the TCB.

## Public API

```c
void tcp_init(TcpTable *table);

Tcb *tcp_listen(TcpTable         *table,
                uint32_t          local_ip,
                uint16_t          local_port,
                TcpRecvHandler    recv_fn,
                TcpConnectHandler connect_fn,
                void             *ctx);

Tcb *tcp_connect(Simulator        *sim,
                 TcpTable         *table,
                 uint32_t          local_ip,
                 uint32_t          remote_ip,
                 uint16_t          local_port,
                 uint16_t          remote_port,
                 TcpRecvHandler    recv_fn,
                 TcpConnectHandler connect_fn,
                 void             *ctx);

int tcp_send(Simulator     *sim,
             Tcb           *tcb,
             const uint8_t *data,
             size_t         len);

int tcp_receive(Interface *iface,
                Packet    *pkt,
                void      *ctx);

int tcp_close(Simulator *sim, Tcb *tcb);

void tcp_retransmit_handler(const Event *e, void *ctx);
```

## Function Behavior

### `tcp_init`

Required behavior:

- If `table == NULL`, return immediately.
- Zero the whole table.

After this, `table->count == 0` and every TCB slot has `valid == 0`.

### `tcp_listen`

Required behavior:

- If `table == NULL`, return `NULL`.
- If `local_port == 0`, return `NULL`.
- If a listener already exists for the same `local_ip` and `local_port`, return
  `NULL`.
- Allocate the first invalid TCB slot.
- If the table is full, return `NULL`.
- Initialize a listener:
  - `local_ip = local_ip`
  - `local_port = local_port`
  - `remote_ip = 0`
  - `remote_port = 0`
  - `state = TCP_LISTEN`
  - `rcv_wnd = TCP_DEFAULT_WINDOW`
  - callbacks and handler context from arguments
- Return the listener TCB.

`local_ip == 0` means wildcard listener.

### `tcp_connect`

Required behavior:

- Reject NULL simulator or table.
- Reject zero local IP, remote IP, local port, or remote port.
- Reject duplicate exact four-tuple.
- Allocate a TCB.
- Initialize active-open state:
  - state `TCP_SYN_SENT`
  - `snd_una = 0`
  - `snd_nxt = 1`
  - `rcv_nxt = 0`
  - default send and receive windows
  - callbacks and handler context from arguments
- Send SYN with:
  - `seq = 0`
  - `ack = 0`
  - flags `TCP_FLAG_SYN`
  - no payload
  - clone requested for retransmission
- Track the SYN clone as sequence range `[0, 1)`.
- If `sim->sched != NULL`, schedule retransmission.
- Return the TCB.

On any local failure after allocating the TCB, release the TCB and return
`NULL`.

### `tcp_send`

Required behavior:

- If `sim == NULL || tcb == NULL`, return `-1`.
- If `tcb->valid != 1`, return `-1`.
- If `tcb->state != TCP_ESTABLISHED`, return `-1`.
- If `len > 0 && data == NULL`, return `-1`.
- If `len == 0`, return `0`.
- Send at most `TCP_MSS` bytes from `data`.
- If any unacknowledged segment exists, return `-1`.
- Send data with:
  - `seq = old snd_nxt`
  - `ack = rcv_nxt`
  - flags `TCP_FLAG_ACK | TCP_FLAG_PSH`
  - clone requested for retransmission
- Track the clone as `[old_snd_nxt, old_snd_nxt + send_len)`.
- Advance `snd_nxt` by `send_len`.
- If `sim->sched != NULL`, schedule retransmission.
- Return `0`.

Current implementation sends only the first `TCP_MSS` bytes when `len > TCP_MSS`
and returns `0`; it does not loop over the remaining data.

### `tcp_receive`

Required validation behavior:

- If `iface == NULL`, return `-1` without touching `pkt`.
- If `pkt == NULL`, increment `iface->rx_errors` and return `-1`.
- If `ctx == NULL`, free `pkt`, increment `iface->rx_errors`, return `-1`.
- If `TcpContext.sim == NULL || TcpContext.table == NULL`, free `pkt`,
  increment `iface->rx_errors`, return `-1`.
- Reject packets shorter than `TCP_HDR_LEN`.
- Reject NULL `pkt->head` or `pkt->data`.
- Reject packets where `pkt->data < pkt->head + IP_HDR_LEN`.
- Reject packets whose visible bytes exceed packet allocation.
- Recover stripped IP header at `pkt->data - IP_HDR_LEN`.
- Reject packets whose recovered IP protocol is not `IPPROTO_TCP`.
- Decode TCP header fields from network byte order.
- Reject packets where header words are not `TCP_HDR_WORDS`.

Lookup behavior:

- Search exact non-listener TCB for destination tuple as local values and
  source tuple as remote values.
- If no exact TCB exists and `flags == TCP_FLAG_SYN`, search a listener by
  destination IP and port.
- If no TCB or listener matches, free `pkt`, increment `iface->rx_dropped`, and
  return `-1`.

Listener SYN behavior:

- Allocate child TCB.
- If allocation fails, free `pkt`, increment `rx_dropped`, return `-1`.
- Initialize child in `TCP_SYN_RECEIVED`.
- Copy callbacks and context from listener.
- Set `child->rcv_nxt = seq_num + 1`.
- Send SYN-ACK with clone.
- Track SYN-ACK clone as `[0, 1)`.
- Free received SYN packet and return `0`.

`TCP_SYN_SENT` behavior:

- Accept segment with SYN and ACK when `ack_num == tcb->snd_nxt`.
- Set `rcv_nxt = seq_num + 1`.
- Set `snd_una = ack_num`.
- Acknowledge queued SYN.
- Move to `TCP_ESTABLISHED`.
- Send pure ACK.
- Call connect handler if present.
- Free received packet and return `0`.

`TCP_SYN_RECEIVED` behavior:

- Accept segment with ACK when `ack_num == tcb->snd_nxt`.
- Set `snd_una = ack_num`.
- Acknowledge queued SYN-ACK.
- Move to `TCP_ESTABLISHED`.
- Call connect handler if present.
- Free packet and return `0`.

`TCP_ESTABLISHED` behavior:

- If ACK flag is set and `ack_num` is in `(snd_una, snd_nxt]`, update
  `snd_una` and clear acknowledged queued segments.
- If payload length is nonzero and `seq_num == rcv_nxt`, advance `rcv_nxt`,
  send pure ACK, strip TCP header, set `layer = 5`, and deliver to receive
  handler.
- If payload length is nonzero and `seq_num != rcv_nxt`, send duplicate ACK,
  free packet, and return `0`.
- If payload length is zero and FIN is present with `seq_num == rcv_nxt`,
  advance `rcv_nxt`, send pure ACK, move to `TCP_CLOSE_WAIT`, free packet, and
  return `0`.
- If payload length is zero and FIN is absent, free packet and return `0`.

`TCP_FIN_WAIT_1` current behavior:

- Only ACK processing is implemented in this state.
- If ACK flag is set and `ack_num > snd_una`, update `snd_una`, clear
  acknowledged queue entries, and if the queued FIN is gone, move to
  `TCP_FIN_WAIT_2`.
- Free packet and return `0`.
- A peer FIN in this state is not handled as simultaneous close by current
  code.

`TCP_FIN_WAIT_2` behavior:

- If FIN is present and `seq_num == rcv_nxt`, advance `rcv_nxt`, send pure ACK,
  move to `TCP_TIME_WAIT`, schedule TIME_WAIT release if scheduler exists, free
  packet, and return `0`.
- Otherwise free packet and return `0`.

`TCP_CLOSING` current behavior:

- If `ack_num > snd_una`, update `snd_una`, clear acknowledged queued segments,
  and move to `TCP_TIME_WAIT` when the queued FIN is gone.
- Schedule TIME_WAIT release if scheduler exists.
- Free packet and return `0`.
- Current code does not have a receive branch that transitions into
  `TCP_CLOSING`.

`TCP_LAST_ACK` behavior:

- If ACK flag is set and `ack_num > snd_una`, update `snd_una`, clear
  acknowledged queued segments, and release the TCB when the queued FIN is gone.
- Free packet and return `0`.

`TCP_TIME_WAIT` behavior:

- Free packet.
- Return `0`.

Unhandled state or unaccepted segment:

- Free packet.
- Increment `iface->rx_dropped`.
- Return `-1`.

### `tcp_close`

Required behavior:

- If `sim == NULL || tcb == NULL`, return `-1`.
- If `tcb->valid != 1`, return `-1`.
- If state is neither `TCP_ESTABLISHED` nor `TCP_CLOSE_WAIT`, return `-1`.
- If any unacknowledged segment exists, return `-1`.
- Send FIN-ACK with:
  - `seq = snd_nxt`
  - `ack = rcv_nxt`
  - flags `TCP_FLAG_FIN | TCP_FLAG_ACK`
  - no payload
  - clone requested for retransmission
- Track clone as `[snd_nxt, snd_nxt + 1)`.
- Advance `snd_nxt` by `1`.
- If old state was `TCP_ESTABLISHED`, move to `TCP_FIN_WAIT_1`.
- If old state was `TCP_CLOSE_WAIT`, move to `TCP_LAST_ACK`.
- If `sim->sched != NULL`, schedule retransmission.
- Return `0`.

### `tcp_retransmit_handler`

Required behavior:

- If event is NULL, return.
- If context is NULL or lacks simulator/table, return.
- If event data is NULL or does not point to a valid TCB, return.
- For each unacknowledged queued segment:
  - clone the queued packet
  - call `ip_output` with TCB local/remote IPs and `IPPROTO_TCP`
  - if IP output fails, free the clone and return
  - increment retransmit count
  - if scheduler exists, update `sent_ts` and schedule another retransmit event

Current behavior can schedule another retransmit for each unacknowledged queue
entry. Since `TCP_MAX_INFLIGHT == 1`, that means at most one new event.

## Flow Charts

### Active Open

```text
tcp_connect
  |
  +-- validate sim/table/addresses/ports
  +-- reject duplicate four-tuple
  +-- allocate TCB
  +-- state = SYN_SENT
  +-- snd_una = 0
  +-- snd_nxt = 1
  |
  +-- send SYN with clone
  +-- queue SYN clone [0, 1)
  +-- schedule retransmit if scheduler exists
  |
  +-- return TCB
```

### Passive Open

```text
tcp_receive SYN
  |
  +-- no exact TCB
  +-- listener matches destination IP/port
  |
  +-- allocate child TCB
  +-- state = SYN_RECEIVED
  +-- rcv_nxt = peer_seq + 1
  |
  +-- send SYN-ACK with clone
  +-- queue SYN-ACK clone [0, 1)
  +-- free received SYN
  |
  +-- return 0
```

### Established Data Receive

```text
tcp_receive established data
  |
  +-- process ACK field if useful
  |
  +-- payload_len > 0?
        |
        +-- seq == rcv_nxt:
        |     rcv_nxt += payload_len
        |     send pure ACK
        |     strip TCP header
        |     layer = 5
        |     deliver to callback or free
        |
        +-- seq != rcv_nxt:
              send duplicate ACK
              free packet
              return 0
```

### Local Close

```text
tcp_close
  |
  +-- require ESTABLISHED or CLOSE_WAIT
  +-- require no unacknowledged segment
  |
  +-- send FIN-ACK with clone
  +-- queue FIN clone
  +-- snd_nxt += 1
  |
  +-- ESTABLISHED -> FIN_WAIT_1
  +-- CLOSE_WAIT  -> LAST_ACK
```

## ACSL Contracts

The contracts belong in `tcp.h`. Use literal bounds:

- TCB slots: `0 .. 63`
- in-flight slots: `0 .. 0`
- TCP header bytes: `20`
- IPv4 stripped header bytes: `20`

### Shared Predicates

```c
/*@
    predicate tcp_state_valid(integer state) =
        TCP_CLOSED <= state && state <= TCP_TIME_WAIT;

    predicate tcp_sendq_count_valid(Tcb *tcb) =
        0 <= tcb->sendq.count && tcb->sendq.count <= 1;

    predicate tcp_tcb_basic_valid(Tcb *tcb) =
        \valid(tcb) &&
        tcb->valid == 1 &&
        tcp_state_valid(tcb->state) &&
        tcp_sendq_count_valid(tcb);

    predicate tcp_table_count_valid(TcpTable *table) =
        0 <= table->count && table->count <= 64;

    predicate tcp_table_well_formed(TcpTable *table) =
        \valid(table) &&
        tcp_table_count_valid(table) &&
        \forall integer i; 0 <= i && i < 64 ==>
            (table->tcbs[i].valid == 0 ||
             tcp_tcb_basic_valid(&table->tcbs[i]));
*/
```

### `tcp_init`

```c
/*@
    behavior null:
        assumes table == \null;
        assigns \nothing;

    behavior valid:
        assumes \valid(table);
        assigns table->tcbs[0 .. 63],
                table->count;
        ensures table->count == 0;
        ensures \forall integer i; 0 <= i && i < 64 ==>
                table->tcbs[i].valid == 0;

    complete behaviors;
    disjoint behaviors;
*/
void tcp_init(TcpTable *table);
```

### `tcp_listen`

```c
/*@
    behavior bad_input:
        assumes table == \null || local_port == 0;
        assigns \nothing;
        ensures \result == \null;

    behavior valid:
        assumes tcp_table_well_formed(table);
        assumes local_port != 0;
        assigns table->tcbs[0 .. 63],
                table->count;
        ensures \result == \null || \valid(\result);
        ensures \result != \null ==> \result->valid == 1;
        ensures \result != \null ==> \result->state == TCP_LISTEN;
        ensures \result != \null ==> \result->local_ip == local_ip;
        ensures \result != \null ==> \result->local_port == local_port;

    complete behaviors;
    disjoint behaviors;
*/
Tcb *tcp_listen(TcpTable *table,
                uint32_t local_ip,
                uint16_t local_port,
                TcpRecvHandler recv_fn,
                TcpConnectHandler connect_fn,
                void *ctx);
```

Additional required proof/test property:

- Duplicate listener for the same local IP and port returns NULL.
- Wildcard listener uses `local_ip == 0`.

### `tcp_connect`

```c
/*@
    behavior bad_input:
        assumes sim == \null || table == \null ||
                local_ip == 0 || remote_ip == 0 ||
                local_port == 0 || remote_port == 0;
        assigns \nothing;
        ensures \result == \null;

    behavior valid:
        assumes simulator_well_formed(sim);
        assumes tcp_table_well_formed(table);
        assumes local_ip != 0 && remote_ip != 0;
        assumes local_port != 0 && remote_port != 0;
        assigns table->tcbs[0 .. 63],
                table->count,
                sim->sched->eq->events,
                sim->sched->eq->events[0 .. sim->sched->eq->capacity - 1],
                sim->sched->eq->count;
        ensures \result == \null || \valid(\result);
        ensures \result != \null ==> \result->state == TCP_SYN_SENT;
        ensures \result != \null ==> \result->snd_una == 0;
        ensures \result != \null ==> \result->snd_nxt == 1;

    complete behaviors;
    disjoint behaviors;
*/
Tcb *tcp_connect(Simulator *sim,
                 TcpTable *table,
                 uint32_t local_ip,
                 uint32_t remote_ip,
                 uint16_t local_port,
                 uint16_t remote_port,
                 TcpRecvHandler recv_fn,
                 TcpConnectHandler connect_fn,
                 void *ctx);
```

### `tcp_send`

```c
/*@
    behavior bad_input:
        assumes sim == \null || tcb == \null ||
                (len > 0 && data == \null);
        assigns \nothing;
        ensures \result == -1;

    behavior invalid_tcb:
        assumes sim != \null && tcb != \null;
        assumes tcb->valid != 1 || tcb->state != TCP_ESTABLISHED;
        assigns \nothing;
        ensures \result == -1;

    behavior empty:
        assumes simulator_well_formed(sim);
        assumes tcp_tcb_basic_valid(tcb);
        assumes tcb->state == TCP_ESTABLISHED;
        assumes len == 0;
        assigns \nothing;
        ensures \result == 0;

    behavior valid:
        assumes simulator_well_formed(sim);
        assumes tcp_tcb_basic_valid(tcb);
        assumes tcb->state == TCP_ESTABLISHED;
        assumes len > 0;
        assumes \valid_read(data + (0 .. len - 1));
        assigns tcb->snd_nxt,
                tcb->sendq,
                tcb->retransmit_ts,
                sim->sched->eq->events,
                sim->sched->eq->events[0 .. sim->sched->eq->capacity - 1],
                sim->sched->eq->count;
        ensures \result == 0 || \result == -1;

    complete behaviors;
    disjoint behaviors;
*/
int tcp_send(Simulator *sim, Tcb *tcb, const uint8_t *data, size_t len);
```

### `tcp_receive`

```c
/*@
    behavior null_iface:
        assumes iface == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior null_pkt:
        assumes iface != \null && pkt == \null;
        assigns iface->rx_errors;
        ensures \result == -1;

    behavior bad_ctx:
        assumes iface != \null && pkt != \null;
        assumes ctx == \null ||
                ((TcpContext *)ctx)->sim == \null ||
                ((TcpContext *)ctx)->table == \null;
        assigns iface->rx_errors;
        ensures \result == -1;

    behavior valid:
        assumes interface_basic_valid(iface);
        assumes pkt != \null;
        assumes ctx != \null;
        assumes ((TcpContext *)ctx)->sim != \null;
        assumes ((TcpContext *)ctx)->table != \null;
        assigns iface->rx_errors,
                iface->rx_dropped,
                ((TcpContext *)ctx)->table->tcbs[0 .. 63],
                ((TcpContext *)ctx)->table->count;
        ensures \result == 0 || \result == -1;

    complete behaviors;
    disjoint behaviors;
*/
int tcp_receive(Interface *iface, Packet *pkt, void *ctx);
```

### `tcp_close`

```c
/*@
    behavior bad_input:
        assumes sim == \null || tcb == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior invalid_tcb:
        assumes sim != \null && tcb != \null;
        assumes tcb->valid != 1 ||
                (tcb->state != TCP_ESTABLISHED &&
                 tcb->state != TCP_CLOSE_WAIT);
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes simulator_well_formed(sim);
        assumes tcp_tcb_basic_valid(tcb);
        assumes tcb->state == TCP_ESTABLISHED ||
                tcb->state == TCP_CLOSE_WAIT;
        assigns tcb->state,
                tcb->snd_nxt,
                tcb->sendq,
                tcb->retransmit_ts,
                sim->sched->eq->events,
                sim->sched->eq->events[0 .. sim->sched->eq->capacity - 1],
                sim->sched->eq->count;
        ensures \result == 0 || \result == -1;

    complete behaviors;
    disjoint behaviors;
*/
int tcp_close(Simulator *sim, Tcb *tcb);
```

### `tcp_retransmit_handler`

```c
/*@
    behavior null_event:
        assumes e == \null;
        assigns \nothing;

    behavior bad_ctx:
        assumes e != \null;
        assumes ctx == \null ||
                ((TcpContext *)ctx)->sim == \null ||
                ((TcpContext *)ctx)->table == \null;
        assigns \nothing;

    behavior valid:
        assumes e != \null;
        assumes ctx != \null;
        assumes ((TcpContext *)ctx)->sim != \null;
        assumes ((TcpContext *)ctx)->table != \null;
        assigns ((TcpContext *)ctx)->table->tcbs[0 .. 63];

    complete behaviors;
    disjoint behaviors;
*/
void tcp_retransmit_handler(const Event *e, void *ctx);
```

## KLEVA Verification Plan

Minimum KLEVA tests:

1. `tcp_init(NULL)` does not crash.
2. `tcp_init(valid)` zeroes count and clears all valid bits.
3. `tcp_listen` rejects NULL table and port zero.
4. `tcp_listen` rejects duplicate listener.
5. Successful listen creates `TCP_LISTEN` TCB.
6. `tcp_connect` rejects NULL simulator/table and zero tuple fields.
7. `tcp_connect` rejects duplicate four-tuple.
8. Successful connect creates `TCP_SYN_SENT` TCB.
9. Successful connect queues SYN clone.
10. Successful connect schedules retransmit when scheduler exists.
11. `tcp_send` rejects invalid TCB and non-established state.
12. `tcp_send` returns `0` for zero length.
13. `tcp_send` rejects NULL data when length is nonzero.
14. `tcp_send` rejects send when one unacknowledged segment exists.
15. Successful send queues one data clone and advances `snd_nxt`.
16. Send length larger than MSS sends only MSS bytes.
17. `tcp_receive` rejects NULL interface.
18. `tcp_receive` with NULL packet increments `rx_errors`.
19. Bad TCP context increments `rx_errors` and frees packet.
20. Too-short packet increments `rx_errors`.
21. Bad packet bounds increments `rx_errors`.
22. Non-TCP recovered IP protocol increments `rx_errors`.
23. TCP header words not equal to `5` increments `rx_errors`.
24. SYN to listener creates child in `TCP_SYN_RECEIVED`.
25. SYN to listener queues SYN-ACK clone.
26. SYN-ACK to active opener moves to `TCP_ESTABLISHED`.
27. Final ACK to passive opener moves to `TCP_ESTABLISHED`.
28. Established ACK clears acknowledged send queue entry.
29. Established in-order data advances `rcv_nxt`.
30. Established in-order data strips TCP header and sets layer `5`.
31. Established out-of-order data sends duplicate ACK and frees packet.
32. Established FIN moves to `TCP_CLOSE_WAIT`.
33. `tcp_close` from established sends FIN and moves to `TCP_FIN_WAIT_1`.
34. `tcp_close` from close-wait sends FIN and moves to `TCP_LAST_ACK`.
35. ACK in `TCP_FIN_WAIT_1` can move to `TCP_FIN_WAIT_2`.
36. FIN in `TCP_FIN_WAIT_2` moves to `TCP_TIME_WAIT`.
37. ACK in `TCP_LAST_ACK` releases the TCB when FIN is acknowledged.
38. `TCP_TIME_WAIT` receive consumes packet and returns `0`.
39. `tcp_retransmit_handler` retransmits queued unacknowledged clone.
40. Current behavior: `TCP_FIN_WAIT_1` does not implement simultaneous close.

## Common Mistakes

- Do not store TCP connections globally in `Simulator`.
- Do not make IP inspect `TcpTable` or `Tcb`.
- Do not treat listener TCBs as connected TCBs.
- Do not forget that SYN and FIN consume sequence numbers.
- Do not queue pure ACKs.
- Do not keep the same packet pointer after `ip_output` succeeds.
- Do not claim checksum validation exists.
- Do not claim TCP options are accepted.
- Do not claim data larger than MSS is fully sent; current code sends one MSS.
- Do not claim `TCP_FIN_WAIT_1` handles simultaneous close; current code does
  not.
