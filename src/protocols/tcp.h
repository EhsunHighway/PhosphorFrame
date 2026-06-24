#ifndef TCP_H
#define TCP_H

#include <stdint.h>
#include <stddef.h>
#include "ip.h"
#include "../engine/event.h"

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
    Packet  *pkt;          // clone of bytes handed to IP
    uint32_t seq_start;    // first sequence number covered by this segment
    uint32_t seq_end;      // first sequence number after this segment
    uint8_t  flags;        // SYN/FIN consume one sequence number
    uint64_t sent_ts;
    int      retransmits;
    int      acked;
} TcpSegment;

typedef struct TcbSendQueue {
    TcpSegment entries[TCP_MAX_INFLIGHT];
    int        count;
} TcbSendQueue;

struct Tcb {
    uint32_t           local_ip;
    uint32_t           remote_ip;
    uint16_t           local_port;
    uint16_t           remote_port;

    TcpState           state;
    uint32_t           snd_una;   // oldest unacknowledged sequence number
    uint32_t           snd_nxt;   // next sequence to send
    uint32_t           rcv_nxt;   // next expected sequence from peer
    uint16_t           rcv_wnd;   // peer advertised window
    uint16_t           snd_wnd;   // our advertised window

    TcbSendQueue       sendq;
    uint64_t           retransmit_ts;

    TcpRecvHandler     recv_handler;
    TcpConnectHandler  connect_handler;
    void              *handler_ctx;

    int                valid;
};

typedef struct TcpTable {
    Tcb tcbs[TCP_MAX_CONNS];
    int count;
} TcpTable;

typedef struct TcpContext {
    Simulator *sim;
    TcpTable  *table;
} TcpContext;


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
void  tcp_init(TcpTable *table);

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
Tcb  *tcp_listen(TcpTable         *table,
                 uint32_t          local_ip,
                 uint16_t          local_port,
                 TcpRecvHandler    recv_fn,
                 TcpConnectHandler connect_fn,
                 void             *ctx);

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
Tcb  *tcp_connect(Simulator        *sim,
                  TcpTable         *table,
                  uint32_t          local_ip,
                  uint32_t          remote_ip,
                  uint16_t          local_port,
                  uint16_t          remote_port,
                  TcpRecvHandler    recv_fn,
                  TcpConnectHandler connect_fn,
                  void             *ctx);

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
int  tcp_send(Simulator     *sim,
              Tcb           *tcb,
              const uint8_t *data,
              size_t         len);

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
int  tcp_receive(Interface *iface, Packet *pkt, void *ctx);

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
int  tcp_close(Simulator *sim, Tcb *tcb);

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
void tcp_retransmit_handler(const Event *e, void *ctx);

#endif // TCP_H
