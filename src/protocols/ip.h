#ifndef IP_H
#define IP_H    

#include <stdint.h>
#include <stddef.h>
#include "../network/packet.h"
#include "../network/interface.h"
#include "../engine/simulator.h"
#include "../network/device.h"
#include "ethernet.h"

#define IP_HDR_LEN         20
#define IP_VERSION         4
#define IPPROTO_ICMP       1
#define IPPROTO_TCP        6
#define IPPROTO_UDP        17
#define IP_DEFAULT_TTL     64
#define IP_FLAG_DF         0x4000   // Don't Fragment flag
#define IP_MAX_PACKET_SIZE 65535    // Maximum size of an IP packet (header + payload)


typedef int (*IpProtocolHandler)(Interface *iface,
                                 Packet    *pkt,
                                 void      *ctx);

typedef struct IpProtocolEntry {
    IpProtocolHandler handler;
    void             *ctx;
} IpProtocolEntry;

typedef struct IpStack {
    Simulator       *sim;
    IpProtocolEntry  protocols[256];
} IpStack;

typedef struct __attribute__((packed)) IpHeader {
    uint8_t  version_ihl;           // Version (4 bits) + Internet Header Length (4 bits)
    uint8_t  dscp_ecn;              // Differentiated Services Code Point (DSCP) (6 bits) + Explicit Congestion Notification (ECN) (2 bits)
    uint16_t total_length;          // Total length of the IP packet (header + payload)
    uint16_t identification;        // Identification for fragmentation
    uint16_t flags_fragment_offset; // Flags (3 bits) + Fragment Offset (13 bits)
    uint8_t  ttl;                   // Time to Live - hop limit
    uint8_t  protocol;              // Protocol number (e.g., 6 for TCP, 17 for UDP)
    uint16_t header_checksum;       // Header checksum
    uint32_t src_ip;                // Source IP address (network byte order)
    uint32_t dst_ip;                // Destination IP address (network byte order)
} IpHeader;


/*@ 
    behavior null:
        assumes sim == \null || sim->topo == \null || stack == \null;
        assigns \nothing;
    behavior registers_handlers:
        assumes \valid(sim) && \valid(sim->topo) && \valid(stack);
        assigns sim->topo->devices[0 .. sim->topo->dev_count-1]->interfaces[0 ..]->rx_handler,
                sim->topo->devices[0 .. sim->topo->dev_count-1]->interfaces[0 ..]->handler_ctx,
                stack->sim,
                stack->protocols[0 .. 255];
    complete behaviors;
    disjoint behaviors;
*/
void ip_init(Simulator *sim, IpStack *stack);

/*@
    behavior null:
        assumes stack == \null;
        assigns \nothing;
    behavior valid:
        assumes \valid(stack);
        assigns stack->sim, stack->protocols[0 .. 255];
        ensures stack->sim == sim;
    complete behaviors;
    disjoint behaviors;
*/

void ip_stack_init(IpStack *stack, Simulator *sim);

/*@
    behavior null:
        assumes stack == \null || iface == \null;
        assigns \nothing;
        ensures \result == -1;
    behavior valid:
        assumes \valid(stack) && \valid(iface);
        assigns iface->rx_handler, iface->handler_ctx;
        ensures \result == 0;
    complete behaviors;
    disjoint behaviors;
*/
int  ip_stack_bind_interface(IpStack *stack, Interface *iface);

/*@
    behavior null:
        assumes stack == \null || handler == \null;
        assigns \nothing;
        ensures \result == -1;
    behavior valid:
        assumes \valid(stack) && handler != \null;
        assigns stack->protocols[protocol];
        ensures \result == 0;
    complete behaviors;
    disjoint behaviors;
*/
int  ip_stack_register_protocol(IpStack           *stack,
                                uint8_t            protocol,
                                IpProtocolHandler  handler,
                                void              *ctx);

/*@
    behavior null:
        assumes stack == \null;
        assigns \nothing;
        ensures \result == -1;
    behavior valid:
        assumes \valid(stack);
        assigns stack->protocols[protocol];
        ensures \result == 0;
    complete behaviors;
    disjoint behaviors;
*/
int  ip_stack_unregister_protocol(IpStack *stack, uint8_t protocol);

/*@ 
    behavior null:
        assumes iface == \null || frame == \null;
        assigns \nothing;
        ensures \result == -1;
    behavior too_short:
        assumes \valid(iface) && \valid(frame) && frame->len < IP_HDR_LEN;
        assigns \nothing;
        ensures \result == -1;  
    behavior bad_version:
        assumes \valid(iface) && \valid(frame) && frame->len >= IP_HDR_LEN;
        assumes ((IpHeader *)frame->data)->version_ihl >> 4 != IP_VERSION;
        assigns iface->rx_errors;
        ensures \result == -1;
    behavior ttl_zero:
        assumes \valid(iface) && \valid(frame) && frame->len >= IP_HDR_LEN;
        assumes ((IpHeader *)frame->data)->version_ihl >> 4 == IP_VERSION;
        assumes ((IpHeader *)frame->data)->ttl == 0;
        assigns iface->rx_dropped;
        ensures \result == -1;
    behavior bad_checksum:
        assumes \valid(iface) && \valid(frame) && frame->len >= IP_HDR_LEN;
        assumes ((IpHeader *)frame->data)->version_ihl >> 4 == IP_VERSION;
        assumes ((IpHeader *)frame->data)->ttl != 0;
        assumes ((IpHeader *)frame->data)->header_checksum == 0xFFFF;
        assigns iface->rx_errors;
        ensures \result == -1;
    behavior valid:
        assumes \valid(iface) && \valid(frame) && frame->len >= 20;
        assumes ((IpHeader *)frame->data)->version_ihl >> 4 == IP_VERSION;
        assumes ((IpHeader *)frame->data)->ttl != 0;
        assumes ((IpHeader *)frame->data)->header_checksum != 0xFFFF;
        assigns iface->rx_bytes, iface->last_rx_time, frame->data, frame->len, frame->layer;
        ensures \result == 0;
    complete behaviors;
    disjoint behaviors;
*/

int  ip_receive(Interface *iface,
                 Packet   *frame,
                 uint16_t  ethertype,
                 void     *ctx);

/*@
    behavior null:
        assumes sim == \null || iface == \null || dst_mac == \null || payload == \null;
        assigns \nothing;
        ensures \result == -1;
    behavior valid:
        assumes \valid(sim) && \valid(iface) && \valid_read(dst_mac+(0..5)) && \valid(payload);
        assigns payload->data, payload->len, payload->capacity, iface->tx_bytes;
        ensures \result >= 0 || \result == -1;
        ensures \result >= 0 ==> payload->layer == 2;
        ensures \result >= 0 ==> payload->len == \old(payload->len) + 20 + ETH_HDR_LEN;
    behavior prepend_failed:
        assumes \valid(sim) && \valid(iface) && \valid_read(dst_mac+(0..5)) && \valid(payload);
        assumes (size_t)(payload->data - payload->head) < sizeof(IpHeader);
        assigns \nothing;
        ensures \result == -1;
    complete behaviors;
    disjoint behaviors;
*/
int  ip_send(Simulator *sim,
             Interface *iface,
             uint8_t    dst_mac[6],
             uint32_t   src_ip,
             uint32_t   dst_ip,
             uint8_t    protocol,
             Packet    *payload);

/*@
    behavior null:
        assumes sim == \null || payload == \null;
        assigns \nothing;
        ensures \result == -1;
    behavior no_topology:
        assumes \valid(sim) && sim->topo == \null && \valid(payload);
        assigns \nothing;
        ensures \result == -1;
    behavior direct_output:
        assumes \valid(sim) && sim->topo != \null && \valid(payload);
        assigns payload->data, payload->len, payload->capacity, payload->layer,
                sim->topo->devices[0 .. sim->topo->dev_count-1]->interfaces[0 ..]->tx_bytes,
                sim->topo->devices[0 .. sim->topo->dev_count-1]->interfaces[0 ..]->last_tx_time;
        ensures \result >= 0 || \result == -1;
        ensures \result > 0 ==> payload->layer == 2;
        ensures \result > 0 ==> payload->len == \old(payload->len) + IP_HDR_LEN + ETH_HDR_LEN;
        ensures \result == 0 ==> payload->layer == \old(payload->layer);
        ensures \result == 0 ==> payload->len == \old(payload->len);
    complete behaviors;
    disjoint behaviors;
*/
int  ip_output(Simulator *sim,
                uint32_t  src_ip,
                uint32_t  dst_ip,
                uint8_t   protocol,
                Packet   *payload);

/*@
    behavior null:
        assumes ip_hdr == \null;
        assigns \nothing;
        ensures \result == 0xFFFF; 
    behavior valid:
        assumes \valid(ip_hdr);
        assigns \nothing;
        ensures \result == 0 || \result != 0;
        ensures ip_hdr->version_ihl == \old(ip_hdr->version_ihl);
        ensures ip_hdr->ttl == \old(ip_hdr->ttl);
    complete behaviors;
    disjoint behaviors;
*/
uint16_t  ip_checksum(IpHeader *ip_hdr);

/*@
    behavior null:
        assumes dev == \null;
        assigns \nothing;
        ensures \result == 0;
    behavior hit:
        assumes \valid(dev);
        assumes \exists integer i; 0 <= i < dev->iface_count &&
                dev->interfaces[i]->ip_addr == dst_ip;
        assigns \nothing;
        ensures \result == 1;
    behavior miss:
        assumes \valid(dev);
        assumes \forall integer i; 0 <= i < dev->iface_count ==>
                dev->interfaces[i]->ip_addr != dst_ip;
        assigns \nothing;
        ensures \result == 0;
    complete behaviors;
    disjoint behaviors;
*/
int  ip_is_local(Device *dev, uint32_t dst_ip);

#endif /* IP_H */
