#ifndef ARP_H
#define ARP_H

#include <stdint.h>
#include <stddef.h>
#include "../network/packet.h"
#include "../network/interface.h"
#include "../engine/simulator.h"
#include "../network/device.h"
#include "arp_cache.h"

#define HARDWARE_TYPE_ETHERNET    1
#define PROTOCOL_TYPE_IPV4        0x0800
#define HARDWARE_ADDR_LEN         6
#define PROTOCOL_ADDR_LEN         4
#define ARP_OPCODE_REQUEST        1
#define ARP_OPCODE_REPLY          2
#define ARP_CACHE_TIMEOUT_MS      300000 /* 5 minutes; An ARP cache entry would expire almost immediately. */ 


typedef struct __attribute__((packed)) ArpPacket {
    uint16_t        hardware_type;       // e.g., 1 for Ethernet
    uint16_t        protocol_type;       // e.g., 0x0800 for IPv4
    uint8_t         hardware_addr_len;   // e.g., 6 for Ethernet MAC
    uint8_t         protocol_addr_len;   // e.g., 4 for IPv4
    uint16_t        opcode;              // 1=request, 2=reply
    uint8_t         sender_hardware_addr[HARDWARE_ADDR_LEN];
    uint32_t        sender_protocol_addr;
    uint8_t         target_hardware_addr[HARDWARE_ADDR_LEN];
    uint32_t        target_protocol_addr;
} ArpPacket;

/*@
    behavior null:
        assumes sim == \null;
        assigns \nothing;
    behavior valid:
        assumes sim != \null;
        assigns sim->sched->handlers[EVT_ARP_REQUEST], sim->sched->handlers[EVT_ARP_REPLY];
    complete behaviors;
    disjoint behaviors;
*/
void arp_init(Simulator *sim);

/*@ 
    behavior null:
        assumes sim == \null || iface == \null || target_ip == 0;
        assigns \nothing;
        ensures \result == -1;
    behavior valid:
        assumes \valid(sim) && \valid(iface) && target_ip != 0;
        assigns sim->sched->eq->events, sim->sched->eq->count;
        ensures \result == 0 ==> sim->sched->eq->count == \old(sim->sched->eq->count) + 1;
        ensures \result == -1 ==> sim->sched->eq->count == \old(sim->sched->eq->count);
    complete behaviors;
    disjoint behaviors;
*/
int  arp_send_request(Simulator *sim,
                      Interface *iface,
                      uint32_t   target_ip);


/*@
    behavior null:
        assumes sim == \null || iface == \null || req_pkt == \null;
        assigns \nothing;
        ensures \result == -1;
    behavior valid:
        assumes \valid(sim) && \valid(iface) && \valid(req_pkt);
        assumes req_pkt->len >= 28;
        assumes \valid_read(req_pkt->data + (0 .. req_pkt->len - 1));
        assigns sim->sched->eq->events, sim->sched->eq->count;
        ensures \result == 0 ==> sim->sched->eq->count == \old(sim->sched->eq->count) + 1;
        ensures \result == -1 ==> sim->sched->eq->count == \old(sim->sched->eq->count);
    complete behaviors;
    disjoint behaviors;
*/
int  arp_send_reply(Simulator *sim,
                    Interface *iface,
                    Packet    *req_pkt);

#endif /* ARP_H */
