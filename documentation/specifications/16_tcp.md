# Module 16 - TCP

**Files:** `src/protocols/tcp.c`, `src/protocols/tcp.h`  
**Status:** Ready for implementation  
**Depends on:** ip, packet, interface, scheduler, simulator

---

## Purpose

TCP provides a reliable, ordered, connection-oriented byte stream above IPv4.
This simulator implements a deliberately small TCP:

1. One connection table per protocol owner.
2. Three-way handshake.
3. Stop-and-wait data transfer: at most one unacknowledged segment per TCB.
4. A retransmit timer for the one unacknowledged segment.
5. FIN-based close.

The goal is not a production TCP stack. The goal is a deterministic transport
model that routing protocols and host applications can use inside the
simulator.

TCP depends on IP for output. IP must not include `tcp.h` and must not inspect
TCP state.

TCP is registered with IP like this:

```c
ip_stack_register_protocol(&ip_stack, IPPROTO_TCP, tcp_receive, &tcp_ctx);
```

All normal TCP sends use:

```c
ip_output(sim, src_ip, dst_ip, IPPROTO_TCP, pkt);
```

`IPPROTO_TCP` must be defined in `ip.h`:

```c
#define IPPROTO_TCP 6
```

---

## Implementation Target

Implement the whole small TCP module in this file:

1. Table initialization and listener creation.
2. Active open with SYN.
3. Passive open with SYN-ACK.
4. Final ACK transition to ESTABLISHED.
5. Stop-and-wait data send and in-order data receive.
6. ACK processing for the one unacknowledged segment.
7. Retransmit timer for the queued segment.
8. FIN-based close.

The phases later in this document are an implementation order, not a different
contract. The final module should satisfy all public API behavior described
below.

---

## Wire Format

TCP has a minimum 20-byte header. Options are not implemented, so this module
only accepts and emits data offset `5`.

```text
offset  size  field           byte order   meaning
------  ----  --------------  ----------   -------------------------------
0       2     src_port        network      sender port
2       2     dst_port        network      receiver port
4       4     seq_num         network      first byte sequence number
8       4     ack_num         network      next expected sequence, if ACK
12      2     data_off_flags  network      high 4 bits = header words
14      2     window          network      receive window
16      2     checksum        network      see checksum rule below
18      2     urgent_ptr      network      ignored here
20      ...   payload         unchanged    application bytes
```

Header type:

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

`sizeof(TcpHeader)` must be `TCP_HDR_LEN`.

Flag layout:

```text
data_off_flags, host order:

15          12 11       6 5 4 3 2 1 0
+------------+----------+-+-+-+-+-+-+
| data offset| reserved |U|A|P|R|S|F|
+------------+----------+-+-+-+-+-+-+
```

Helpers should parse it like this:

```c
uint16_t dof = ns_ntohs(tcp_hdr->data_off_flags);
uint8_t  data_offset_words = (uint8_t)(dof >> 12);
uint8_t  flags = (uint8_t)(dof & 0x3F);
```

To build it:

```c
tcp_hdr->data_off_flags = ns_htons((TCP_HDR_WORDS << 12) | flags);
```

---

## Header Constants

Add these constants in `tcp.h`:

```c
#define TCP_HDR_LEN          20
#define TCP_HDR_WORDS        5
#define TCP_FLAG_FIN         0x01
#define TCP_FLAG_SYN         0x02
#define TCP_FLAG_RST         0x04
#define TCP_FLAG_PSH         0x08
#define TCP_FLAG_ACK         0x10
#define TCP_FLAG_URG         0x20
#define TCP_MAX_CONNS        64
#define TCP_MAX_INFLIGHT     1
#define TCP_RETRANSMIT_US    1000000ULL
#define TCP_TIME_WAIT_US     (2 * TCP_RETRANSMIT_US)
#define TCP_MSS              1460
#define TCP_DEFAULT_WINDOW   65535
```

Use microseconds for timers because `Scheduler.now` and `Event.timestamp` are
microseconds.

---

## TCP Vocabulary For This Module

This section defines the TCP terms used by the implementation. The goal is to
make the state machine readable even if TCP is new to you.

### TCB

`Tcb` means Transmission Control Block. In this simulator, a TCB is one TCP
connection record. It is not a packet and it is not a wire header. It is the
memory TCP keeps so it can understand future packets for the same connection.

A TCB stores:

- local address and port: who this side is
- remote address and port: who the peer is
- current TCP state: `LISTEN`, `SYN_SENT`, `ESTABLISHED`, and so on
- send sequence state: what this side has sent and what is still unacknowledged
- receive sequence state: what byte this side expects next from the peer
- retransmission queue: outbound segments waiting for ACK
- application callbacks

There are three common TCB roles in this module:

| Role | State | Meaning |
| --- | --- | --- |
| Listener | `TCP_LISTEN` | Waits for incoming SYN packets on a local IP/port. |
| Active opener | `TCP_SYN_SENT` | Created by `tcp_connect`; waits for SYN-ACK. |
| Connected child | `TCP_SYN_RECEIVED` or later | Created from a listener when a SYN arrives. It has a full four-tuple. |

A listener TCB is not the connection itself. It is a template and lookup entry.
When a SYN arrives for a listener, TCP allocates a separate child TCB for that
peer. The child copies the listener callbacks, then stores the peer address,
peer port, sequence state, and retransmission queue for that one connection.

### TCP Table

`TcpTable` is the owner’s collection of TCBs. A host can have many TCP
connections at once, so TCP state lives in a table instead of one global
connection variable.

### Four-Tuple

A connected TCP flow is identified by four values:

```text
local_ip, local_port, remote_ip, remote_port
```

For a received packet:

- local values come from the packet destination: `dst_ip`, `dst_port`
- remote values come from the packet source: `src_ip`, `src_port`

Listeners are special: they match only local IP/port and have no fixed remote
peer yet. When a listener receives a SYN, TCP allocates a child TCB with the
full four-tuple.

### Sequence Numbers And ACK Numbers

TCP numbers bytes. The sequence number says which byte this segment starts
with. The ACK number says the next peer byte expected.

This module tracks three important numbers in each TCB:

| Field | Meaning |
| --- | --- |
| `snd_una` | Oldest sent sequence number not yet acknowledged. |
| `snd_nxt` | Next sequence number this side will send. |
| `rcv_nxt` | Next sequence number expected from the peer. |

When this side sends payload, `snd_nxt` advances by the payload length. When
this side receives in-order payload, `rcv_nxt` advances by the payload length.

ACK numbers acknowledge received peer bytes. Sending local payload does not
increase the outgoing ACK number. Outgoing ACK usually uses `ack = tcb->rcv_nxt`.

When this document writes an interval like `(tcb->snd_una, tcb->snd_nxt]`, it
means:

```text
ack > tcb->snd_una && ack <= tcb->snd_nxt
```

The left parenthesis means the lower value is not included. The right bracket
means the upper value is included.

### SYN And FIN Consume Sequence Numbers

TCP control flags can consume sequence numbers even when there is no payload:

- `SYN` consumes one sequence number.
- `FIN` consumes one sequence number.
- pure `ACK` does not consume a sequence number.

That is why a SYN sent with `seq = 0` is tracked as sequence range `[0, 1)`,
and why sending a FIN advances `snd_nxt` by `1`.

### FIN And Close Direction

FIN means “this side will not send more bytes.” It closes one sending direction;
it does not automatically mean the whole connection record can be deleted.

Two common paths:

| Path | Meaning |
| --- | --- |
| Local close | This side calls `tcp_close`, sends FIN, and waits for the peer to ACK it. |
| Remote close | The peer sends FIN; this side ACKs it and moves toward close state. |

State names describe where we are in that close conversation:

- `TCP_FIN_WAIT_1`: we sent FIN and are waiting for ACK of our FIN.
- `TCP_FIN_WAIT_2`: our FIN was ACKed; we are waiting for the peer FIN.
- `TCP_CLOSE_WAIT`: peer sent FIN; application has not closed our side yet.
- `TCP_LAST_ACK`: we sent our FIN after peer FIN and are waiting for its ACK.
- `TCP_TIME_WAIT`: both FIN directions are done; keep the TCB briefly so late
  duplicate packets do not confuse a new connection with the same four-tuple.

`TCP_LAST_ACK` is entered by `tcp_close`, not directly by `tcp_receive`.
The sequence is:

1. `tcp_receive` accepts a peer FIN while the connection is established.
2. TCP ACKs the peer FIN and moves to `TCP_CLOSE_WAIT`.
3. Later, the local application calls `tcp_close`.
4. `tcp_close` sends the local FIN.
5. If that FIN is sent and queued successfully, the state becomes
   `TCP_LAST_ACK`.

### Send Queue

`sendq` stores clones of outbound segments that may need retransmission.
Segments are removed from the queue when an incoming ACK covers their
`seq_end`.

Pure ACKs are not queued. They do not consume sequence numbers and can be sent
again later if needed.

### Packet Clone For Retransmission

`ip_output` takes ownership of the packet it sends. After a successful
`ip_output`, TCP cannot keep using that same `Packet *`.

For segments that may need retransmission, TCP asks `tcp_send_segment` for a
clone. The original packet goes to IP. The clone stays in `tcb->sendq`.

Use a clone for:

- SYN
- SYN-ACK
- data
- FIN-ACK

Do not use a clone for pure ACK. A pure ACK carries no payload and consumes no
sequence number, so TCP can recreate it later instead of storing it.

### Duplicate ACK

A duplicate ACK is a normal ACK segment sent again with the same ACK number as
before. It does not mean TCP received duplicate data. It means:

```text
I am still waiting for byte tcb->rcv_nxt.
```

This module sends a duplicate ACK when payload arrives out of order. For
example, if `tcb->rcv_nxt == 1000` but a segment arrives with `seq == 1200`,
TCP cannot deliver byte `1200` yet because bytes `1000..1199` are missing.
The correct response is a pure ACK with:

- `seq = tcb->snd_nxt`
- `ack = tcb->rcv_nxt`
- flags `TCP_FLAG_ACK`
- no payload
- no retransmission clone

After sending that ACK, TCP frees the received out-of-order packet and returns
`0`. The packet was valid TCP but not acceptable for delivery yet, so this is
not an `rx_errors` path.

---

## State Model

TCP state belongs to a protocol owner, not globally to the entire simulator.
For the final stack, `Host` owns a `TcpTable`. Until `Host` exists, unit tests
may allocate a `TcpTable` directly and pass it through `TcpContext`.

Do not store one global TCP table in `Simulator`. In a network simulator,
multiple hosts must be able to listen on the same local port independently.

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

typedef struct Tcb Tcb;

typedef void (*TcpRecvHandler)(Tcb *tcb, Packet *payload, void *ctx);
typedef void (*TcpConnectHandler)(Tcb *tcb, void *ctx);

typedef struct TcpSegment {
    Packet   *pkt;          // clone of bytes handed to IP
    uint32_t  seq_start;    // first sequence number covered by this segment
    uint32_t  seq_end;      // first sequence number after this segment
    uint8_t   flags;        // SYN/FIN consume one sequence number
    uint64_t  sent_ts;
    int       retransmits;
    int       acked;
} TcpSegment;

typedef struct TcbSendQueue {
    TcpSegment entries[TCP_MAX_INFLIGHT];
    int        count;
} TcbSendQueue;

struct Tcb {
    uint32_t  local_ip;      // host order
    uint32_t  remote_ip;     // host order; 0 for LISTEN wildcard
    uint16_t  local_port;    // host order
    uint16_t  remote_port;   // host order; 0 for LISTEN wildcard

    TcpState  state;
    uint32_t  snd_una;       // oldest unacknowledged sequence
    uint32_t  snd_nxt;       // next sequence to send
    uint32_t  rcv_nxt;       // next expected sequence from peer
    uint16_t  snd_wnd;       // peer advertised window
    uint16_t  rcv_wnd;       // our advertised window

    TcbSendQueue sendq;
    uint64_t  retransmit_ts;

    TcpRecvHandler    recv_handler;
    TcpConnectHandler connect_handler;
    void             *handler_ctx;

    int       valid;
};

typedef struct TcpTable {
    Tcb tcbs[TCP_MAX_CONNS];
    int count;
} TcpTable;

typedef struct TcpContext {
    Simulator *sim;
    TcpTable  *table;
} TcpContext;
```

`TcpRecvHandler` receives ownership of `payload` when `payload != NULL`. The
callback must eventually free it or pass ownership onward. TCP must not free
the payload after handing it to the callback.

`TcpConnectHandler` is optional. TCP calls it when a connection becomes established.
This avoids overloading `recv_handler(tcb, NULL, handler_ctx)` with a second
meaning.

---

## Public API

```c
void tcp_init(TcpTable *table);

Tcb *tcp_listen(TcpTable *table,
                uint32_t local_ip,
                uint16_t local_port,
                TcpRecvHandler recv_fn,
                TcpConnectHandler connect_fn,
                void *ctx);

Tcb *tcp_connect(Simulator *sim,
                 TcpTable *table,
                 uint32_t local_ip,
                 uint32_t remote_ip,
                 uint16_t local_port,
                 uint16_t remote_port,
                 TcpRecvHandler recv_fn,
                 TcpConnectHandler connect_fn,
                 void *ctx);

int tcp_send(Simulator *sim,
             Tcb *tcb,
             const uint8_t *data,
             size_t len);

int tcp_receive(Interface *iface, Packet *pkt, void *ctx);

int tcp_close(Simulator *sim, Tcb *tcb);

void tcp_retransmit_handler(const Event *e, void *ctx);
```

`tcp_init` only initializes a table. Registration with IP is performed by the
owner that has both a table and context:

```c
TcpContext tcp_ctx = { .sim = sim, .table = &tcp_table };
ip_stack_register_protocol(&ip_stack, IPPROTO_TCP, tcp_receive, &tcp_ctx);
```

That keeps `tcp_init` usable in unit tests without needing a simulator.

---

## Ownership Rules

These rules are part of the implementation contract.

- `tcp_init`, `tcp_listen`, and `tcp_connect` do not take packet ownership from
  the caller.
- `tcp_connect` creates a SYN packet.
- If `tcp_connect` successfully calls `ip_output`, ownership of the SYN packet
  transfers to IP.
- If `tcp_connect` creates a packet but fails before ownership transfer, it
  frees that packet.
- `tcp_receive` consumes every non-null `pkt` passed to it.
- On receive validation failure, `tcp_receive` frees `pkt`.
- On successful data receive, TCP strips the TCP header and transfers the
  payload packet to `recv_handler`.
- On receive paths that send ACK, SYN-ACK, or FIN, any newly created packet is
  transferred to `ip_output` on success and freed on local failure.
- `sendq` owns clones of unacknowledged segments. `TCP_MAX_INFLIGHT` is `1`,
  so this behaves like stop-and-wait.
- Clear a queued segment by freeing `entry->pkt`, zeroing the entry, and
  decreasing `sendq.count`.

---

## Packet Layout On Receive

`tcp_receive` is called after IPv4 has stripped the IP header.

At function entry:

```c
pkt->data                 points to TcpHeader
pkt->len                  is TCP header + TCP payload bytes
pkt->data - IP_HDR_LEN    points to the stripped IPv4 header
```

The stripped IP header is still readable because `packet_strip` only advances
`data`; it does not erase bytes.

Use the stripped IP header for:

- source IP and destination IP used in TCB lookup
- validating that the packet came from protocol `IPPROTO_TCP`
- constructing replies with source/destination reversed

Do not strip the TCP header until after:

1. the packet is validated,
2. the matching TCB is found,
3. the sequence is accepted,
4. the callback path is selected.

---

## Checksum Rule

Real TCP requires a pseudo-header checksum. This simulator deliberately does
not implement TCP checksum validation yet.

For this TCP module:

- outgoing TCP headers set `checksum = 0`
- incoming TCP packets are accepted regardless of `checksum`

When checksum support is added, use
`tcp_checksum(src_ip, dst_ip, tcp_bytes, len)` with the IPv4 pseudo-header.
Do not mix that future work into this implementation.

---

## Helper Functions

These helpers may be `static` in `tcp.c`.

### `tcp_alloc_tcb`

```c
static Tcb *tcp_alloc_tcb(TcpTable *table);
```

Behavior:

1. If `table == NULL`, return `NULL`.
2. Search for the first slot where `valid == 0`.
3. Zero that slot.
4. Set `valid = 1`.
5. Increment `table->count`.
6. Return the slot.
7. If no slot is free, return `NULL`.

### `tcp_release_tcb`

```c
static void tcp_release_tcb(TcpTable *table, Tcb *tcb);
```

Behavior:

1. If either pointer is `NULL`, return.
2. For each `TcpSegment` in `tcb->sendq.entries`, free
   `entry->pkt` when it is non-NULL.
3. Zero the TCB.
4. Decrement `table->count` if it is greater than zero.

### `tcp_sendq_has_unacked`

```c
static int tcp_sendq_has_unacked(const Tcb *tcb);
```

Return `1` when any send-queue entry has `pkt != NULL && acked == 0`.

### `tcp_sendq_track`

```c
static int tcp_sendq_track(Tcb *tcb,
                           Packet *clone,
                           uint32_t seq_start,
                           uint32_t seq_end,
                           uint8_t flags,
                           uint64_t now);
```

Behavior:

1. If `tcb == NULL || clone == NULL`, return `-1`.
2. If `tcb->sendq.count >= TCP_MAX_INFLIGHT`, return `-1`.
3. Store the clone and sequence metadata in the first free entry.
4. Increment `tcb->sendq.count`.
5. Return `0`.

This queue tracks SYN, SYN-ACK, data, and FIN segments. Since
`TCP_MAX_INFLIGHT` is `1`, only one entry may be unacknowledged at a time.
Increasing `TCP_MAX_INFLIGHT` later makes the design a sliding-window sender
without replacing the TCB layout.

### `tcp_sendq_ack`

```c
static void tcp_sendq_ack(Tcb *tcb, uint32_t ack);
```

Behavior:

1. For each queued entry where `entry->pkt != NULL` and `ack >= entry->seq_end`,
   free `entry->pkt` and clear the entry.
2. Recompute `sendq.count`.
3. Leave partially acknowledged entries untouched. This implementation does not
   create partially acknowledged queued entries because each data send is at
   most one segment.

### `tcp_find_exact`

```c
static Tcb *tcp_find_exact(TcpTable *table,
                           uint32_t local_ip,
                           uint16_t local_port,
                           uint32_t remote_ip,
                           uint16_t remote_port);
```

Match only valid non-LISTEN TCBs with all four tuple fields equal.

### `tcp_find_listener`

```c
static Tcb *tcp_find_listener(TcpTable *table,
                              uint32_t local_ip,
                              uint16_t local_port);
```

Match valid TCBs in `TCP_LISTEN` state where:

- `local_port` matches, and
- `tcb->local_ip == local_ip` or `tcb->local_ip == 0`

`local_ip == 0` means wildcard listener.

## TCP Segment Kinds Used Here

This module uses a small set of TCP segment shapes. The names below describe
which flags and payload are used when calling `tcp_send_segment`.

| Name | Flags | Payload | Clone? | Why |
| --- | --- | --- | --- | --- |
| SYN | `TCP_FLAG_SYN` | none | yes | Active open; retransmit until SYN-ACK arrives. |
| SYN-ACK | `TCP_FLAG_SYN | TCP_FLAG_ACK` | none | yes | Passive open reply; retransmit until final ACK arrives. |
| Pure ACK | `TCP_FLAG_ACK` | none | no | Acknowledges received bytes; not stored for retransmission. |
| Data | `TCP_FLAG_ACK | TCP_FLAG_PSH` | application bytes | yes | Carries bytes that must be retransmitted until ACKed. |
| FIN-ACK | `TCP_FLAG_FIN | TCP_FLAG_ACK` | none | yes | Closes one direction; FIN consumes one sequence number. |

For a pure ACK:

- `seq` is the current local send sequence, usually `tcb->snd_nxt`.
- `ack` is the next peer byte expected, usually `tcb->rcv_nxt`.
- `payload == NULL`.
- `payload_len == 0`.
- `sent_clone == NULL`.

Pure ACKs are not queued because TCP can send another ACK later if needed. SYN,
SYN-ACK, data, and FIN are queued because they consume sequence numbers or carry
data and may need retransmission.

### `tcp_send_segment`

```c
static int tcp_send_segment(Simulator *sim,
                            uint32_t src_ip,
                            uint32_t dst_ip,
                            uint16_t src_port,
                            uint16_t dst_port,
                            uint32_t seq,
                            uint32_t ack,
                            uint8_t flags,
                            const uint8_t *payload,
                            size_t payload_len,
                            Packet **sent_clone);
```

Inputs are host order except payload bytes.

Behavior:

1. Validate `sim != NULL`.
2. Validate `payload_len <= TCP_MSS`.
3. Validate `payload_len == 0 || payload != NULL`.
4. Create a packet with capacity `TCP_HDR_LEN + payload_len`.
5. Fill `TcpHeader` at `pkt->data`.
6. Store integer fields in network byte order.
7. Set:

   ```c
   data_off_flags = ns_htons((TCP_HDR_WORDS << 12) | flags);
   window = ns_htons(TCP_DEFAULT_WINDOW);
   checksum = 0;
   urgent_ptr = 0;
   ```

8. Copy payload bytes after the header when `payload_len > 0`.
9. Set `pkt->len = TCP_HDR_LEN + payload_len`.
10. Set `pkt->layer = 4`.
11. If `sent_clone != NULL`, clone `pkt` before handing it to IP. If clone
    allocation fails, free `pkt` and return `-1`.
12. Call `ip_output(sim, src_ip, dst_ip, IPPROTO_TCP, pkt)`.
13. If `ip_output` returns `-1`, free `pkt` and any clone, then return `-1`.
14. On success, ownership of `pkt` transfers to IP. If `sent_clone != NULL`,
    ownership of the clone transfers to the caller.
15. Return `0`.

Use `sent_clone` for SYN, SYN-ACK, data, and FIN-ACK segments that need queue
tracking. Pass `NULL` for pure ACK segments.

### `tcp_schedule_retransmit`

```c
static void tcp_schedule_retransmit(Simulator *sim,
                                    TcpContext *tcp_ctx,
                                    Tcb *tcb);
```

Behavior:

1. If `sim == NULL || sim->sched == NULL || tcb == NULL`, return.
2. The retransmit event represents a timer attached to one TCB. It is not a
   packet-send event and it is not associated with source or destination
   devices.
3. The event timestamp is the current scheduler time plus
   `TCP_RETRANSMIT_US`.
4. The event fields have this meaning:

   ```text
   type        = EVT_TCP_RETRANSMIT
   timestamp   = current scheduler time + TCP_RETRANSMIT_US
   src_device  = NULL
   dst_device  = NULL
   packet      = NULL
   data        = tcb
   ```

5. If `tcp_ctx` is available, attach `tcp_retransmit_handler` and `tcp_ctx` to
   the event as a per-event callback. This makes the event self-contained.
6. If `tcp_ctx` is not available, create the event without a per-event callback.
   In that case, the scheduler-level handler registered for
   `EVT_TCP_RETRANSMIT` supplies the TCP context.
7. If event allocation fails, return.
8. If scheduling fails, free the unscheduled event and return.
9. After the event is scheduled, record the same timestamp in
   `tcb->retransmit_ts`.

### `tcp_time_wait_handler`

```c
static void tcp_time_wait_handler(const Event *e, void *ctx);
```

Behavior:

1. If `e == NULL || ctx == NULL`, return.
2. Cast `TcpTable *table = (TcpTable *)ctx`.
3. Cast `Tcb *tcb = (Tcb *)e->data`.
4. If `table == NULL || tcb == NULL || tcb->valid != 1`, return.
5. If `tcb->state != TCP_TIME_WAIT`, return.
6. Release the TCB with `tcp_release_tcb(table, tcb)`.

---

## `tcp_init`

```c
void tcp_init(TcpTable *table);
```

Behavior:

1. If `table == NULL`, return immediately.
2. Zero the whole table.

ACSL shape:

```c
/*@
    behavior null:
        assumes table == \null;
        assigns \nothing;

    behavior valid:
        assumes \valid(table);
        assigns table->tcbs[0 .. 64-1], table->count;
        ensures table->count == 0;
        ensures \forall integer i; 0 <= i < 64 ==>
            table->tcbs[i].valid == 0;

    complete behaviors;
    disjoint behaviors;
*/
```

---

## `tcp_listen`

```c
Tcb *tcp_listen(TcpTable *table,
                uint32_t local_ip,
                uint16_t local_port,
                TcpRecvHandler recv_fn,
                TcpConnectHandler connect_fn,
                void *ctx);
```

Inputs:

- `local_ip` is host-order IPv4. `0` means wildcard local address.
- `local_port` is host order.
- `recv_fn` may be `NULL` for tests, but real applications should provide it.
- `connect_fn` may be `NULL`.

Validation order:

1. If `table == NULL`, return `NULL`.
2. If `local_port == 0`, return `NULL`.
3. If any valid LISTEN TCB already has the same `local_ip` and `local_port`,
   return `NULL`.
4. Allocate a TCB.
5. Fill:
   - `local_ip`
   - `local_port`
   - `remote_ip = 0`
   - `remote_port = 0`
   - `state = TCP_LISTEN`
   - `rcv_wnd = TCP_DEFAULT_WINDOW`
   - `recv_handler = recv_fn`
   - `connect_handler = connect_fn`
   - `handler_ctx = ctx`
6. Return the TCB.

ACSL shape:

```c
/*@
    behavior null_input:
        assumes table == \null || local_port == 0;
        assigns \nothing;
        ensures \result == \null;

    behavior duplicate:
        assumes \valid(table) && local_port != 0;
        assumes \exists integer i; 0 <= i < 64 &&
            table->tcbs[i].valid == 1 &&
            table->tcbs[i].state == TCP_LISTEN &&
            table->tcbs[i].local_ip == local_ip &&
            table->tcbs[i].local_port == local_port;
        assigns \nothing;
        ensures \result == \null;

    behavior valid:
        assumes \valid(table) && local_port != 0;
        assigns table->tcbs[0 .. 64-1], table->count;
        ensures \result == \null || \valid(\result);

    complete behaviors;
*/
```

---

## `tcp_connect`

```c
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

Inputs are host order.

Validation order:

1. If `sim == NULL`, return `NULL`.
2. If `table == NULL`, return `NULL`.
3. If `local_ip == 0 || remote_ip == 0`, return `NULL`.
4. If `local_port == 0 || remote_port == 0`, return `NULL`.
5. If an exact TCB already exists for the four-tuple, return `NULL`.
6. Allocate a TCB.
7. Initialize active-open state:
   - `local_ip = local_ip`
   - `remote_ip = remote_ip`
   - `local_port = local_port`
   - `remote_port = remote_port`
   - `state = TCP_SYN_SENT`
   - `snd_una = 0`
   - `snd_nxt = 1`
   - `rcv_nxt = 0`
   - `snd_wnd = TCP_DEFAULT_WINDOW`
   - `rcv_wnd = TCP_DEFAULT_WINDOW`
   - `recv_handler = recv_fn`
   - `connect_handler = connect_fn`
   - `handler_ctx = ctx`
8. Send a SYN segment from the local tuple to the remote tuple:
   - source IP/port are `local_ip`, `local_port`
   - destination IP/port are `remote_ip`, `remote_port`
   - `seq = 0`
   - `ack = 0`
   - flags are `TCP_FLAG_SYN`
   - payload is empty
9. Request a retransmission clone from `tcp_send_segment`. In C, this means
   keeping a local `Packet *` initialized to `NULL` and passing its address as
   the clone output parameter. On success, that local pointer receives the
   clone that belongs in `tcb->sendq`.
10. If SYN send fails:
   - release the TCB,
   - return `NULL`.
11. Track the SYN clone in `tcb->sendq` with:
    - `seq_start = 0`
    - `seq_end = 1`
    - `flags = TCP_FLAG_SYN`
12. If queue tracking fails, free the clone, release the TCB, and return
    `NULL`.
13. If `sim->sched != NULL`, schedule retransmit with
    `tcp_schedule_retransmit(sim, NULL, tcb)`. The owner should have registered
    `tcp_retransmit_handler` with scheduler context before using timers.
14. Return the TCB.

In this simplified TCP, the initial sequence number is always `0`. SYN consumes
one sequence number, so `snd_nxt` becomes `1`.

ACSL shape:

```c
/*@
    behavior null_input:
        assumes sim == \null || table == \null ||
                local_ip == 0 || remote_ip == 0 ||
                local_port == 0 || remote_port == 0;
        assigns \nothing;
        ensures \result == \null;

    behavior valid:
        assumes \valid(sim) && \valid(table);
        assumes local_ip != 0 && remote_ip != 0;
        assumes local_port != 0 && remote_port != 0;
        assigns table->tcbs[0 .. 64-1], table->count;
        ensures \result == \null || \valid(\result);
        ensures \result != \null ==> \result->state == TCP_SYN_SENT;
        ensures \result != \null ==> \result->snd_una == 0;
        ensures \result != \null ==> \result->snd_nxt == 1;

    complete behaviors;
*/
```

---

## `tcp_receive`

```c
int tcp_receive(Interface *iface, Packet *pkt, void *ctx);
```

`tcp_receive` consumes every non-null packet it receives, except when
`iface == NULL`. A consumed packet is either freed by TCP or transferred to the
application receive handler.

### Receive Pipeline

Process the packet in this order.

1. Validate the function inputs.
   - If `iface == NULL`, return `-1` without touching `pkt`.
   - If `pkt == NULL`, increment `iface->rx_errors` and return `-1`.
   - If `ctx == NULL`, free `pkt`, increment `iface->rx_errors`, and return
     `-1`.
2. Interpret `ctx` as `TcpContext *`. If the context has no simulator or no
   table, free `pkt`, increment `iface->rx_errors`, and return `-1`.
3. Validate packet storage before reading headers.
   - `pkt->len` must be at least `TCP_HDR_LEN`.
   - `pkt->head` and `pkt->data` must be non-null.
   - `pkt->data` must leave a readable stripped IPv4 header at
     `pkt->data - IP_HDR_LEN`.
   - `pkt->data + pkt->len` must stay inside the allocated packet buffer.
   On failure, free `pkt`, increment `iface->rx_errors`, and return `-1`.
4. Recover the stripped IPv4 header from `pkt->data - IP_HDR_LEN`. It provides
   the source IP, destination IP, and protocol.
5. Reject packets whose stripped IPv4 header does not say `IPPROTO_TCP`.
   This is a malformed dispatch path: free `pkt`, increment `iface->rx_errors`,
   and return `-1`.
6. Read the TCP header from `pkt->data`.
7. Convert all multi-byte header fields from network order to host order before
   using them for comparisons or TCB lookup. Convert each field once:
   - `src_ip`, `dst_ip`
   - `src_port`, `dst_port`
   - `seq`, `ack`
   - `data_off_flags`
8. Decode `data_off_flags`. In host order, the high 4 bits are the TCP header
   length in 32-bit words, and the low bits contain the flags:

   ```text
   hdr_words = data_off_flags >> 12
   flags     = data_off_flags & 0x3F
   ```

   `hdr_words == 5` means a 20-byte TCP header. Values greater than `5` mean
   the TCP segment has options after the fixed header. This module does not
   implement TCP options, so if `hdr_words != TCP_HDR_WORDS`, free `pkt`,
   increment `iface->rx_errors`, and return `-1`.
9. Compute `payload_len = pkt->len - TCP_HDR_LEN`. Because options are not
   supported, the TCP payload starts immediately after the 20-byte TCP header.
10. Find the matching connection:
    - first search for an exact four-tuple using local destination values
      `(dst_ip, dst_port)` and remote source values `(src_ip, src_port)`;
    - if there is no exact match and the segment is exactly `SYN`, search for a
      listener on `(dst_ip, dst_port)`.

After this pipeline, choose exactly one of the state branches below. Branches
that send a reply use `tcp_send_segment` with one of the segment kinds defined
above. Pure ACK replies pass `NULL` for the clone output because TCP does not
queue pure ACKs for retransmission.

### LISTEN Receives SYN

This branch handles a new passive-open request when no exact TCB exists and a
listener matched the destination address and port.

Accepted segment:

- `listener != NULL`
- `flags == TCP_FLAG_SYN`

Behavior:

1. Allocate a child TCB.
2. If allocation fails, free `pkt`, increment `iface->rx_dropped`, and return
   `-1`.
3. Initialize the child from the listener and the received segment:
   - child local IP/port: `dst_ip`, `dst_port`
   - child remote IP/port: `src_ip`, `src_port`
   - state: `TCP_SYN_RECEIVED`
   - `snd_una = 0`
   - `snd_nxt = 1`
   - `rcv_nxt = seq + 1`
   - default send/receive windows
   - callbacks and handler context copied from the listener
4. Send a `SYN | ACK` segment from the child local IP/port to the child remote
   IP/port. That means:
   - `sim` comes from `tcp_ctx->sim`,
   - source IP/port are `child->local_ip`, `child->local_port`,
   - destination IP/port are `child->remote_ip`, `child->remote_port`,
   - `seq = 0`,
   - `ack = child->rcv_nxt`.
5. Pass the address of a local `Packet *` clone variable to `tcp_send_segment`,
   because SYN-ACK must be tracked for retransmission.
6. Track the clone in the child send queue with sequence range `[0, 1)`.
   SYN consumes one sequence number.
7. If sending or queue tracking fails, release the child TCB, free `pkt`,
   increment `iface->rx_errors`, and return `-1`.
8. Free the received `pkt` and return `0`.

### SYN_SENT Receives SYN-ACK

This branch completes the active opener side of the handshake.

Accepted segment:

- exact TCB exists
- TCB state is `TCP_SYN_SENT`
- segment has both `SYN` and `ACK`
- `ack == tcb->snd_nxt`

Behavior:

1. Set `tcb->rcv_nxt = seq + 1`.
2. Set `tcb->snd_una = ack`.
3. Acknowledge the queued SYN with `tcp_sendq_ack(tcb, ack)`.
4. Move the TCB to `TCP_ESTABLISHED`.
5. Send a pure ACK using `seq = tcb->snd_nxt`, `ack = tcb->rcv_nxt`,
   no payload, and no clone.
6. If the ACK send fails, free `pkt`, increment `iface->rx_errors`, and return
   `-1`.
7. If a connect handler exists, call it after the state becomes established.
8. Free `pkt` and return `0`.

### SYN_RECEIVED Receives Final ACK

This branch completes the passive opener side of the handshake.

Accepted segment:

- exact TCB exists
- TCB state is `TCP_SYN_RECEIVED`
- segment has `ACK`
- `ack == tcb->snd_nxt`

Behavior:

1. Set `tcb->snd_una = ack`.
2. Acknowledge the queued SYN-ACK with `tcp_sendq_ack(tcb, ack)`.
3. Move the TCB to `TCP_ESTABLISHED`.
4. If a connect handler exists, call it.
5. Free `pkt` and return `0`.

### ESTABLISHED Receives ACK Or Data

This branch handles normal established traffic. Process the received segment in
this order:

1. ACK field
2. payload bytes
3. FIN flag

A segment may contain more than one of these. For example, a data segment from
the peer usually also has `ACK` set for our previously sent bytes.

ACK processing:

1. If the segment does not have `ACK`, leave `snd_una` and `sendq` unchanged.
2. If the segment has `ACK` and `ack` is in `(tcb->snd_una, tcb->snd_nxt]`,
   set `tcb->snd_una = ack` and clear fully acknowledged queued segments with
   `tcp_sendq_ack(tcb, ack)`.
3. If the segment has `ACK` but the ACK number is not new, leave `snd_una` and
   `sendq` unchanged.
4. ACK processing does not consume `pkt`. Continue to payload processing.

Payload processing:

1. If `payload_len == 0`, there is no application data to deliver. Continue to
   FIN processing.
2. If `payload_len > 0`, accept only `seq == tcb->rcv_nxt`.
3. If the payload is out of order, send a duplicate ACK with the current
   `tcb->rcv_nxt`, free `pkt`, and return `0`.
4. For in-order data, advance `tcb->rcv_nxt += payload_len`.
5. Send an ACK with `seq = tcb->snd_nxt` and `ack = tcb->rcv_nxt`.
6. Strip the 20-byte TCP header and set `pkt->layer = 5`.
7. If `recv_handler` exists, transfer ownership of the stripped packet to the
   handler and return `0`.
8. If no receive handler exists, free the stripped packet and return `0`.

FIN processing after ACK-only segments:

1. If `payload_len == 0` and the segment does not have `FIN`, free `pkt` and
   return `0`.
2. If the segment has `FIN`, do not free `pkt` here. Continue to the FIN branch
   below so the FIN consumes one sequence number and is acknowledged.

### FIN And Closing State Branches

FIN consumes one sequence number and must be acknowledged. The same incoming
segment may also carry an ACK for our previously sent FIN, so each close state
must handle ACK and FIN in a clear order.

The receive switch should be organized by TCP state. Use these state sections
as the mental map for the code:

- `TCP_ESTABLISHED`: process peer data and peer FIN.
- `TCP_FIN_WAIT_1`: process ACK of our FIN, peer FIN, or both.
- `TCP_FIN_WAIT_2`: process peer FIN after our FIN was acknowledged.
- `TCP_CLOSING`: process ACK of our FIN after both sides already sent FIN.
- `TCP_LAST_ACK`: process ACK of our FIN after passive close.
- `TCP_TIME_WAIT`: consume late duplicate segments.

When a branch below says “ACK covers our FIN,” it means the incoming segment
has `ACK` set and its `ack` value is greater than or equal to the `seq_end` of
the queued FIN segment. In this module, call `tcp_sendq_ack(tcb, ack)` to clear
fully acknowledged queued segments.

When a branch enters `TCP_TIME_WAIT`, schedule one TIME_WAIT expiration event
when `sim->sched != NULL`. The event data is `tcb`, the callback is
`tcp_time_wait_handler`, and the callback context is the owning `TcpTable *`.
If no scheduler exists, keep the TCB in `TCP_TIME_WAIT` for tests to inspect.

When a close branch sends a pure ACK and the send fails, free `pkt`, increment
`iface->rx_errors`, and return `-1`. Do not change the TCP state after a failed
ACK send.

#### ESTABLISHED Receives FIN

This is a remote close. The peer says it will not send more bytes.

Accepted segment:

- exact TCB exists
- TCB state is `TCP_ESTABLISHED`
- segment has `FIN`
- `seq == tcb->rcv_nxt`

Behavior:

1. Set `tcb->rcv_nxt = seq + 1`.
2. Send a pure ACK with `seq = tcb->snd_nxt` and `ack = tcb->rcv_nxt`.
3. Move to `TCP_CLOSE_WAIT`.
4. Free `pkt` and return `0`.

`TCP_CLOSE_WAIT` means the peer has closed its sending direction, but the local
application has not called `tcp_close` yet. When the application later calls
`tcp_close`, TCP sends its own FIN and moves to `TCP_LAST_ACK`.

#### FIN_WAIT_1 Receives ACK Or FIN

`TCP_FIN_WAIT_1` means local TCP already sent FIN and is waiting for the peer to
ACK that FIN. The peer may also send its own FIN while we are still waiting for
that ACK, so this state has to reason about two separate facts:

- did this segment ACK our FIN?
- did this segment contain an acceptable peer FIN?

In this branch, an acceptable peer FIN means:

```text
(flags & TCP_FLAG_FIN) != 0 && seq == tcb->rcv_nxt
```

If the FIN flag is absent, there is no peer FIN in this segment. If the FIN flag
is present but `seq != tcb->rcv_nxt`, the peer FIN is out of order and this
branch must not consume it.

Accepted segment:

- exact TCB exists
- TCB state is `TCP_FIN_WAIT_1`
- segment has `ACK`, `FIN`, or both

Behavior:

1. Remember whether this segment has a FIN with `seq == tcb->rcv_nxt`.
2. If the segment has `ACK` and `ack > tcb->snd_una`, update
   `tcb->snd_una = ack` and call `tcp_sendq_ack(tcb, ack)`.
3. Determine whether our queued FIN is now acknowledged by inspecting the send
   queue after `tcp_sendq_ack`:
   - scan `tcb->sendq.entries`
   - find any entry where `entry->pkt != NULL`, `entry->acked == 0`, and
     `entry->flags` contains `TCP_FLAG_FIN`
   - if no such entry exists, our FIN is acknowledged
   - if such an entry still exists, our FIN is not acknowledged yet
4. If the segment has an acceptable FIN:
   - set `tcb->rcv_nxt = seq + 1`
   - send a pure ACK with `seq = tcb->snd_nxt` and `ack = tcb->rcv_nxt`
5. If our FIN is acknowledged and this segment did not carry an acceptable peer
   FIN, move to `TCP_FIN_WAIT_2`, free `pkt`, and return `0`.
6. If the segment had an acceptable FIN and our FIN is not acknowledged yet,
   move to `TCP_CLOSING`, free `pkt`, and return `0`.
7. If the segment had an acceptable FIN and our FIN is acknowledged, move to
   `TCP_TIME_WAIT`, schedule TIME_WAIT expiration when possible, free `pkt`,
   and return `0`.
8. If neither the ACK nor FIN changed state, free `pkt` and return `0`.

#### FIN_WAIT_2 Receives FIN

`TCP_FIN_WAIT_2` means our FIN was acknowledged and we are waiting for the peer
FIN.

Accepted segment:

- exact TCB exists
- TCB state is `TCP_FIN_WAIT_2`
- segment has `FIN`
- `seq == tcb->rcv_nxt`

Behavior:

1. Set `tcb->rcv_nxt = seq + 1`.
2. Send a pure ACK with `seq = tcb->snd_nxt` and `ack = tcb->rcv_nxt`.
3. Move to `TCP_TIME_WAIT`.
4. Schedule TIME_WAIT expiration when possible.
5. Free `pkt` and return `0`.

#### CLOSING Receives ACK

`TCP_CLOSING` means both sides have sent FIN, but our FIN has not been
acknowledged yet.

Accepted segment:

- exact TCB exists
- TCB state is `TCP_CLOSING`
- segment has `ACK`
- ACK covers our queued FIN

Behavior:

1. Update `tcb->snd_una = ack` when `ack > tcb->snd_una`.
2. Clear the queued FIN with `tcp_sendq_ack(tcb, ack)`.
3. Move to `TCP_TIME_WAIT`.
4. Schedule TIME_WAIT expiration when possible.
5. Free `pkt` and return `0`.

#### LAST_ACK Receives ACK

`TCP_LAST_ACK` means the peer already sent FIN, local TCP already replied with
its own FIN, and local TCP is waiting for the peer to ACK that local FIN.

Accepted segment:

- exact TCB exists
- TCB state is `TCP_LAST_ACK`
- segment has `ACK`
- ACK covers our queued FIN

Behavior:

1. Update `tcb->snd_una = ack` when `ack > tcb->snd_una`.
2. Clear the queued FIN with `tcp_sendq_ack(tcb, ack)`.
3. Release the TCB with `tcp_release_tcb(table, tcb)`.
4. Free `pkt` and return `0`.

#### TIME_WAIT Receives Segment

`TCP_TIME_WAIT` means the close handshake is complete but the TCB is being kept
temporarily to absorb late duplicate packets.

Behavior:

1. Free `pkt`.
2. Return `0`.

### Unhandled Segment

If no branch accepts the segment, TCP does not respond in this simplified
module. Free `pkt`, increment `iface->rx_dropped`, and return `-1`.

ACSL shape:

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

    behavior null_ctx:
        assumes \valid(iface) && \valid(pkt) && ctx == \null;
        assigns iface->rx_errors;
        ensures \result == -1;

    behavior bad_ctx:
        assumes \valid(iface) && \valid(pkt) && ctx != \null;
        assumes ((TcpContext *)ctx)->sim == \null ||
                ((TcpContext *)ctx)->table == \null;
        assigns iface->rx_errors;
        ensures \result == -1;

    behavior too_short:
        assumes \valid(iface) && \valid(pkt) && ctx != \null;
        assumes ((TcpContext *)ctx)->sim != \null;
        assumes ((TcpContext *)ctx)->table != \null;
        assumes pkt->len < 20;
        assigns iface->rx_errors;
        ensures \result == -1;

    behavior readable_tcp:
        assumes \valid(iface) && \valid(pkt) && ctx != \null;
        assumes ((TcpContext *)ctx)->sim != \null;
        assumes ((TcpContext *)ctx)->table != \null;
        assumes pkt->len >= 20;
        assumes pkt->data >= pkt->head + 20;
        assumes \valid_read(pkt->data + (0 .. pkt->len-1));
        assigns iface->rx_errors, iface->rx_dropped;
        ensures \result == 0 || \result == -1;

    complete behaviors;
*/
```

---

## `tcp_send`

```c
int tcp_send(Simulator *sim, Tcb *tcb, const uint8_t *data, size_t len);
```

Validation order:

1. If `sim == NULL || tcb == NULL`, return `-1`.
2. If `tcb->valid != 1`, return `-1`.
3. If `tcb->state != TCP_ESTABLISHED`, return `-1`.
4. If `len > 0 && data == NULL`, return `-1`.
5. If `len == 0`, return `0`.
6. If `len > TCP_MSS`, send only the first `TCP_MSS` bytes. The caller may
   call again for remaining bytes.
7. If `tcp_sendq_has_unacked(tcb)`, return `-1`. With
   `TCP_MAX_INFLIGHT == 1`, stop-and-wait allows one unacknowledged segment.
8. Let `send_len` be the number of payload bytes sent by this call. It is
   `len` when `len <= TCP_MSS`; otherwise it is `TCP_MSS`.
9. Save the current send sequence before creating the segment. This saved value
   is the first sequence number covered by the outgoing payload.
10. Send one data segment with flags `TCP_FLAG_ACK | TCP_FLAG_PSH`.
11. The outgoing segment uses:
    - source/destination IP and ports from the TCB,
    - `seq = old snd_nxt`,
    - `ack = rcv_nxt`,
    - payload bytes `data[0 .. send_len-1]`,
    - `payload_len = send_len`,
    - a requested clone for retransmission tracking.
12. The `seq` field describes bytes this side is sending. The `ack` field
    describes bytes already received from the peer. Sending new payload advances
    `snd_nxt`; it does not change `rcv_nxt`, so it does not add `send_len` to
    the outgoing ACK number.
13. Track the returned clone in `tcb->sendq` with:
    - `seq_start = old snd_nxt`
    - `seq_end = old snd_nxt + send_len`
    - `flags = TCP_FLAG_ACK | TCP_FLAG_PSH`
    - timestamp = current scheduler time when a scheduler exists, otherwise `0`
14. If queue tracking fails, free the clone and return `-1`.
15. Advance `tcb->snd_nxt += send_len`.
16. Schedule retransmit with `tcp_schedule_retransmit(sim, NULL, tcb)` when
    `sim->sched != NULL`.
17. Return `0`.

ACSL shape:

```c
/*@
    behavior null_input:
        assumes sim == \null || tcb == \null || data == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior invalid_tcb:
        assumes \valid(sim) && \valid(tcb);
        assumes tcb->valid != 1 || tcb->state != TCP_ESTABLISHED;
        assigns \nothing;
        ensures \result == -1;

    behavior empty:
        assumes \valid(sim) && \valid(tcb);
        assumes tcb->valid == 1;
        assumes tcb->state == TCP_ESTABLISHED;
        assumes data != \null;
        assumes len == 0;
        assigns \nothing;
        ensures \result == 0;

    behavior valid:
        assumes \valid(sim) && \valid(tcb);
        assumes tcb->valid == 1;
        assumes tcb->state == TCP_ESTABLISHED;
        assumes data != \null;
        assumes len > 0;
        assumes \valid_read(data + (0 .. len-1));
        assigns tcb->snd_nxt, tcb->sendq, tcb->retransmit_ts;
        ensures \result == 0 || \result == -1;

    complete behaviors;
*/
```

---

## Retransmit Timer

The scheduler event type already exists:

```c
EVT_TCP_RETRANSMIT
```

The owner may register a scheduler-level handler:

```c
scheduler_register(sim->sched, EVT_TCP_RETRANSMIT,
                   tcp_retransmit_handler, tcp_ctx);
```

Prefer per-event callback scheduling when creating TCP timers, because the
event can carry the correct TCP context directly:

```c
Event *e = event_create_callback(EVT_TCP_RETRANSMIT,
                                 scheduler_now(sim->sched) + TCP_RETRANSMIT_US,
                                 NULL, NULL, NULL, tcb,
                                 tcp_retransmit_handler, tcp_ctx);
scheduler_schedule(sim->sched, e);
```

There is no cancel API. When an ACK clears the send queue before the timer
fires, the later retransmit event should become a harmless no-op because no
unacknowledged entry remains.

`tcp_retransmit_handler` behavior:

1. If `e == NULL`, return.
2. Cast `TcpContext *tcp_ctx = (TcpContext *)ctx`.
3. If `tcp_ctx == NULL || tcp_ctx->sim == NULL || tcp_ctx->table == NULL`,
   return.
4. Cast `Tcb *tcb = (Tcb *)e->data`.
5. If `tcb == NULL || tcb->valid != 1`, return.
6. Find the first send-queue entry where `pkt != NULL && acked == 0`.
7. If no such entry exists, return.
8. Clone that entry's `pkt`.
9. Send the clone through
   `ip_output(tcp_ctx->sim, tcb->local_ip, tcb->remote_ip, IPPROTO_TCP, clone)`.
10. If `ip_output` fails, free the clone and return.
11. Increment `entry->retransmits`.
12. Update `entry->sent_ts = scheduler_now(tcp_ctx->sim->sched)` when a
    scheduler exists.
13. Reschedule another retransmit event with `tcp_schedule_retransmit` when
    `tcp_ctx->sim->sched != NULL`.

ACSL shape:

```c
/*@
    behavior null_event:
        assumes e == \null;
        assigns \nothing;

    behavior null_ctx:
        assumes e != \null && ctx == \null;
        assigns \nothing;

    behavior bad_ctx:
        assumes e != \null && ctx != \null;
        assumes ((TcpContext *)ctx)->sim == \null ||
                ((TcpContext *)ctx)->table == \null;
        assigns \nothing;

    behavior valid:
        assumes e != \null && ctx != \null;
        assumes ((TcpContext *)ctx)->sim != \null;
        assumes ((TcpContext *)ctx)->table != \null;
        assigns \nothing;

    complete behaviors;
*/
```

---

## `tcp_close`

`tcp_close(sim, tcb)` behavior:

1. If `sim == NULL || tcb == NULL`, return `-1`.
2. If `tcb->valid != 1`, return `-1`.
3. If `tcb->state` is neither `TCP_ESTABLISHED` nor `TCP_CLOSE_WAIT`, return
   `-1`.
4. If `tcp_sendq_has_unacked(tcb)`, return `-1`; stop-and-wait does not send
   FIN while another segment is unacknowledged.
5. Save the current state as `old_state`.
6. Send `TCP_FLAG_FIN | TCP_FLAG_ACK` with:
   - `seq = tcb->snd_nxt`
   - `ack = tcb->rcv_nxt`
   - no payload
   - `sent_clone != NULL`
7. If sending the FIN fails, return `-1` and leave the state unchanged.
8. Track the FIN clone with:
   - `seq_start = old snd_nxt`
   - `seq_end = old snd_nxt + 1`
   - `flags = TCP_FLAG_FIN | TCP_FLAG_ACK`
9. If queue tracking fails, free the FIN clone, return `-1`, and leave the
   state unchanged.
10. Advance `tcb->snd_nxt += 1` because FIN consumes one sequence number.
11. If `old_state == TCP_ESTABLISHED`, set `state = TCP_FIN_WAIT_1`.
12. If `old_state == TCP_CLOSE_WAIT`, set `state = TCP_LAST_ACK`.
13. Schedule retransmit with `tcp_schedule_retransmit(sim, NULL, tcb)` when
    `sim->sched != NULL`.
14. Return `0`.

ACSL shape:

```c
/*@
    behavior null_input:
        assumes sim == \null || tcb == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior invalid_tcb:
        assumes \valid(sim) && \valid(tcb);
        assumes tcb->valid != 1;
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes \valid(sim) && \valid(tcb);
        assumes tcb->valid == 1;
        assigns tcb->state, tcb->snd_nxt, tcb->sendq, tcb->retransmit_ts;
        ensures \result == 0 || \result == -1;

    complete behaviors;
*/
```

---

## Implementation Checklist

Implement in this order. Each step builds on the earlier one but the final
module includes all of them.

1. Fix `tcp.h` include guard and includes:
   - `<stdint.h>`
   - `<stddef.h>`
   - `ip.h`
   - `event.h`
2. Confirm `IPPROTO_TCP 6` exists in `ip.h`.
3. Add TCP constants.
4. Add `TcpHeader`, `TcpState`, `Tcb`, `TcpTable`, `TcpContext`, callback
   typedefs, and public prototypes.
5. Implement `tcp_init`.
6. Implement static table helpers.
7. Implement `tcp_send_segment` with checksum `0`.
8. Implement `tcp_listen`.
9. Implement `tcp_connect`.
10. Implement `tcp_receive` validation and handshake:
    - validation failure paths,
    - LISTEN + SYN,
    - SYN_SENT + SYN|ACK,
    - SYN_RECEIVED + ACK,
    - unhandled drop.
11. Implement `tcp_send`.
12. Extend `tcp_receive` for ESTABLISHED ACK and data paths.
13. Implement retransmit scheduling and `tcp_retransmit_handler`.
14. Implement `tcp_close` and close-state receive paths.
15. Add focused unit tests for all public functions and important state
    transitions.

---

## Test Plan

Connection setup tests:

- `tcp_init_null`
- `tcp_init_valid`
- `tcp_listen_null_table`
- `tcp_listen_zero_port`
- `tcp_listen_duplicate`
- `tcp_listen_valid`
- `tcp_connect_null_input`
- `tcp_connect_zero_tuple`
- `tcp_connect_sends_syn`
- `tcp_receive_null_iface`
- `tcp_receive_null_pkt`
- `tcp_receive_null_ctx`
- `tcp_receive_too_short`
- `tcp_receive_bad_protocol`
- `tcp_receive_listen_syn_sends_syn_ack`
- `tcp_receive_syn_sent_syn_ack_establishes`
- `tcp_receive_syn_received_ack_establishes`
- `tcp_receive_unhandled_drops`

Data and ACK tests:

- `tcp_send_null_input`
- `tcp_send_not_established`
- `tcp_send_one_segment`
- `tcp_receive_established_ack_clears_retransmit`
- `tcp_receive_established_data_calls_recv`
- `tcp_receive_out_of_order_sends_duplicate_ack`

Retransmit tests:

- `tcp_retransmit_null_event`
- `tcp_retransmit_no_buffer_noop`
- `tcp_retransmit_clones_and_reschedules`

Close tests:

- `tcp_close_established_sends_fin`
- `tcp_close_close_wait_sends_fin`
- `tcp_receive_fin_established_moves_close_wait`
- `tcp_receive_fin_wait_2_fin_moves_time_wait`
- `tcp_receive_last_ack_ack_releases_tcb`
