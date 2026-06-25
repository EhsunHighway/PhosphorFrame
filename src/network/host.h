#ifndef HOST_H
#define HOST_H

#include <stdint.h>

#include "device.h"
#include "interface.h"
#include "packet.h"
#include "../engine/simulator.h"
#include "../protocols/arp_cache.h"
#include "../protocols/ip.h"
#include "../protocols/udp.h"
#include "../protocols/tcp.h"

#define HOST_MAX_PORTS 8

typedef struct Host {
    Device      base;

    Simulator  *sim;

    ArpCache   *arp_cache;
    IpStack    *ip_stack;

    UdpState   *udp_state;
    UdpContext *udp_context;

    TcpTable   *tcp_table;
    TcpContext *tcp_context;

    uint32_t   gateway_ip;  // default gateway IP address (0 if none)
} Host;

/*@
    behavior bad_input:
        assumes name == \null || sim == \null;
        assigns \nothing;
        ensures \result == \null;

    behavior valid_input:
        assumes name != \null && sim != \null;
        allocates \result;
        ensures \result == \null || \valid(\result);
        ensures \result != \null ==> \result->sim == sim;
        ensures \result != \null ==> \result->base.iface_count == 0;
        ensures \result != \null ==> \result->base.iface_max == 8;
        ensures \result != \null ==> \result->base.interfaces != \null;
        ensures \result != \null ==> \result->gateway_ip == gateway_ip;

    complete behaviors;
    disjoint behaviors;
*/
Host *host_create(const char *name,
                  Simulator  *sim,
                  uint32_t    gateway_ip);

/*@
    behavior null:
        assumes host == \null;
        assigns \nothing;

    behavior valid:
        assumes \valid(host);
        frees host->base.interfaces[0 .. host->base.iface_count-1],
              host->base.interfaces,
              host;

    complete behaviors;
    disjoint behaviors;
*/
void  host_free(Host *host);

/*@
    behavior null_input:
        assumes host == \null || iface == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior full:
        assumes \valid(host) && \valid(iface);
        assumes host->base.iface_count >= host->base.iface_max;
        assigns \nothing;
        ensures \result == -1;

    behavior added:
        assumes \valid(host) && \valid(iface);
        assumes host->base.iface_count < host->base.iface_max;
        assigns host->base.interfaces[0 .. host->base.iface_count],
                host->base.iface_count,
                iface->device,
                iface->arp_cache,
                iface->rx_handler,
                iface->handler_ctx;
        ensures \result == 0 || \result == -1;
        ensures \result == 0 ==> host->base.iface_count == \old(host->base.iface_count) + 1;
        ensures \result == 0 ==> host->base.interfaces[\old(host->base.iface_count)] == iface;
        ensures \result == 0 ==> iface->device == &host->base;

    complete behaviors;
    disjoint behaviors;
*/
int   host_add_interface(Host *host, Interface *iface);

/*@
    behavior null_input:
        assumes host == \null || iface == \null || pkt == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior non_ipv4:
        assumes \valid(host) && \valid(iface) && \valid(pkt);
        assumes ethertype != 0x0800;
        assigns \nothing;
        frees pkt;
        ensures \result == -1;

    behavior ipv4:
        assumes \valid(host) && \valid(iface) && \valid(pkt);
        assumes ethertype == 0x0800;
        assigns iface->rx_bytes,
                iface->last_rx_time,
                iface->rx_errors,
                iface->rx_dropped,
                pkt->data,
                pkt->len,
                pkt->layer;
        ensures \result == 0 || \result == -1;

    complete behaviors;
    disjoint behaviors;
*/
int   host_receive(Host      *host,
                   Interface *iface,
                   Packet    *pkt,
                   uint16_t   ethertype);

/*@
    behavior null_input:
        assumes host == \null || payload == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior bad_address:
        assumes \valid(host) && \valid(payload);
        assumes src_ip == 0 || dst_ip == 0;
        assigns \nothing;
        ensures \result == -1;

    behavior no_simulator:
        assumes \valid(host) && \valid(payload);
        assumes src_ip != 0 && dst_ip != 0;
        assumes host->sim == \null ||
                (\valid(host->sim) && host->sim->topo == \null);
        assigns \nothing;
        ensures \result == -1;

    behavior send:
        assumes \valid(host) && \valid(payload);
        assumes src_ip != 0 && dst_ip != 0;
        assumes \valid(host->sim);
        assumes host->sim->topo != \null;
        assigns payload->data,
                payload->len,
                payload->capacity,
                payload->layer,
                host->sim->topo->devices[0 .. host->sim->topo->dev_count-1]->interfaces[0 ..]->tx_bytes,
                host->sim->topo->devices[0 .. host->sim->topo->dev_count-1]->interfaces[0 ..]->last_tx_time;
        ensures \result == 0 || \result == -1;

    complete behaviors;
    disjoint behaviors;
*/
int   host_send_ip(Host    *host,
                   uint32_t src_ip,
                   uint32_t dst_ip,
                   uint8_t  protocol,
                   Packet  *payload);

#endif /* HOST_H */
