#ifndef UDP_H
#define UDP_H

#include <stdint.h>
#include <stddef.h>
#include "ip.h"

#define UDP_HDR_LEN       8
#define UDP_MAX_SOCKETS   32
#define UDP_PORT_DNS      53
#define UDP_PORT_DHCP_SRV 67
#define UDP_PORT_DHCP_CLI 68
#define UDP_PORT_RIP      520

typedef struct UdpSocket UdpSocket;
typedef void (*Udp_Recv_Handler)(uint32_t src_ip,
                                  uint16_t src_port,
                                  Packet *payload,
                                  void *ctx);

struct UdpSocket {
    uint16_t         port;            // host order
    int              valid;           // 1 when this slot is bound
    Udp_Recv_Handler recv_handler;    // called on receive hit
    void            *ctx;             // callback-owned context
};

typedef struct UdpState {
    UdpSocket sockets[UDP_MAX_SOCKETS];
    int       count;
} UdpState;

typedef struct UdpContext {
    Simulator *sim;
    UdpState  *state;
} UdpContext;

typedef struct __attribute__((packed)) UdpHeader {
    uint16_t src_port;                // Sender port number
    uint16_t dst_port;                // Receiver port number
    uint16_t length;                  // UDP header + payload length
    uint16_t checksum;                // 0 means no checksum
} UdpHeader;


/*@
    behavior null:
        assumes state == \null;
        assigns \nothing;
    
    behavior valid:
        assumes \valid(state);
        assigns state->sockets[0..32-1], state->count;
        ensures state->count == 0;
        ensures \forall integer i; 0 <= i < 32 ==> state->sockets[i].valid == 0;

    complete behaviors;
    disjoint behaviors;
*/
void udp_init(UdpState *state);


/*@
    behavior null_input:
        assumes state == \null || recv_handler == \null || port == 0;
        assigns \nothing;
        ensures \result == -1;

    behavior duplicate:
        assumes \valid(state) && recv_handler != \null && port != 0;
        assumes \exists integer i; 0 <= i < 32 &&
                state->sockets[i].valid == 1 &&
                state->sockets[i].port == port;
        assigns \nothing;
        ensures \result == -1;
    
    behavior full:
        assumes \valid(state) && recv_handler != \null && port != 0;
        assumes \forall integer i; 0 <= i < 32 ==>
                state->sockets[i].valid == 1;
        assigns \nothing;
        ensures \result == -1;

    behavior bound:
        assumes \valid(state) && recv_handler != \null && port != 0;
        assumes \forall integer i; 0 <= i < 32 ==>
                state->sockets[i].valid == 0 ||
                state->sockets[i].port != port;
        assumes \exists integer i; 0 <= i < 32 &&
                state->sockets[i].valid == 0;
        assigns state->sockets[0..32-1], state->count;
        ensures \result == 0;
        ensures state->count == \old(state->count) + 1;

    complete behaviors;
*/
int  udp_bind(UdpState        *state,
              uint16_t         port,
              Udp_Recv_Handler recv_handler,
              void            *ctx);


/*@
    behavior null_input:
        assumes state == \null || port == 0;
        assigns \nothing;
        ensures \result == -1;

    behavior not_found:
        assumes \valid(state) && port != 0;
        assumes \forall integer i; 0 <= i < 32 ==>
                state->sockets[i].valid == 0 ||
                state->sockets[i].port != port;
        assigns \nothing;
        ensures \result == -1;

    behavior remove:
        assumes \valid(state) && port != 0;
        assumes \exists integer i; 0 <= i < 32 &&
                state->sockets[i].valid == 1 &&
                state->sockets[i].port == port;
        assigns state->sockets[0..32-1], state->count;
        ensures \result == 0;

    complete behaviors;
*/
int  udp_unbind(UdpState *state, uint16_t port);


/*@
    behavior null_input:
        assumes sim == \null ||
                dst_port == 0 ||
                (payload_len > 0 && payload == \null);
        assigns \nothing;
        ensures \result == -1;

    behavior too_large:
        assumes \valid(sim);
        assumes dst_port != 0;
        assumes payload_len > 0 ==> payload != \null;
        assumes payload_len > 65535 - 8;
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes \valid(sim);
        assumes dst_port != 0;
        assumes payload_len <= 65535 - 8;
        assumes payload_len == 0 || \valid_read(payload + (0..payload_len-1));
        assigns \nothing;
        ensures \result == 0 || \result == -1;
    
    complete behaviors;
*/
int  udp_send(Simulator     *sim,
              uint32_t       src_ip,
              uint32_t       dst_ip,
              uint16_t       src_port,
              uint16_t       dst_port,
              const uint8_t *payload,
              size_t         payload_len);


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

    behavior too_short:
        assumes \valid(iface) && \valid(pkt) && ctx != \null;
        assumes pkt->len < 8;
        assigns iface->rx_errors;
        ensures \result == -1;

    behavior readable_udp:
        assumes \valid(iface) && \valid(pkt) && ctx != \null;
        assumes pkt->len >= 8;
        assumes pkt->data >= pkt->head + 20;
        assumes \valid_read(pkt->data + (0 .. pkt->len-1));
        assigns pkt->data, pkt->len, pkt->layer;
        ensures \result == 0 || \result == -1;

    complete behaviors;
*/
int  udp_receive(Interface *iface, 
                 Packet    *pkt,
                 void      *ctx);

#endif /* UDP_H */
