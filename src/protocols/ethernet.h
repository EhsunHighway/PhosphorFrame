#ifndef ETHERNET_H
#define ETHERNET_H

#include <stdint.h>
#include <stddef.h>
#include "../common/byte_order.h" /* htons() and ntohs() */
#include "../network/packet.h"
#include "../network/interface.h"
#include "../engine/simulator.h"
#include "../engine/event.h"

#define ETH_HDR_LEN      14
#define ETH_ALEN         6
#define ETHERTYPE_IPV4   0x0800
#define ETHERTYPE_ARP    0x0806
#define ETHERTYPE_IPV6   0x86DD
extern const uint8_t     ETH_BROADCAST[6];


/*
 * without __attribute__((packed)), the compiler may insert padding
 * and the struct won't be 14 bytes.
 */
typedef struct __attribute__((packed)) EthernetHeader {
    uint8_t    dst_mac[6];
    uint8_t    src_mac[6];
    uint16_t   ethertype;  // stored in network byte order on the wire
} EthernetHeader;

void ethernet_receive_event(const Event *e, void *ctx);

/*@
    behavior null:
        assumes sim == \null || iface == \null || dst_mac == \null || payload == \null;
        assigns \nothing;
        ensures \result == -1;
    behavior valid:
        assumes \valid(sim) && \valid(iface) && \valid_read(dst_mac+(0..5)) && \valid(payload);
        assigns payload->data, payload->len, payload->capacity, iface->tx_bytes;
        ensures \result >= 0 || \result == -1;
        ensures \result >= 0 ==> payload->len == \old(payload->len) + ETH_HDR_LEN;
    complete behaviors;
    disjoint behaviors;
*/
int  ethernet_send(Simulator    *sim,
                   Interface    *iface,
                   const uint8_t dst_mac[6],
                   uint16_t      ethertype,
                   Packet       *payload);

/*@
    behavior null:
        assumes iface == \null || frame == \null || out_ethertype == \null || frame->len < (size_t)ETH_HDR_LEN;
        assigns \nothing;
        ensures \result == -1;
    behavior drop:
        assumes \valid(iface) && \valid(frame) && \valid(out_ethertype) && frame->len >= (size_t)ETH_HDR_LEN &&
                ((EthernetHeader *)frame->data)->dst_mac[0] != iface->mac[0] &&
                ((EthernetHeader *)frame->data)->dst_mac[0] != ETH_BROADCAST[0];
        assigns \nothing;
        ensures \result == 1;
    behavior valid:
        assumes \valid(iface) && \valid(frame) && \valid(out_ethertype) && frame->len >= (size_t)ETH_HDR_LEN;
        assigns frame->data, frame->len, *out_ethertype, iface->rx_bytes;
        ensures \result == 0 ==> frame->len == \old(frame->len) - ETH_HDR_LEN;
        ensures \result == 0 ==> *out_ethertype == ((\old(frame->data[12]) << 8) | \old(frame->data[13]));
    complete behaviors;
    disjoint behaviors;
*/
int  ethernet_receive(Interface *iface,
                      Packet    *frame,
                      uint16_t  *out_ethertype);


#endif /* ETHERNET_H */
