#include "host.h"
#include "../protocols/arp.h"
#include "../protocols/icmp.h"
#include <stdlib.h>
#include <string.h>

Host *host_create(const char *name,
                  Simulator  *sim,
                  uint32_t    gateway_ip) {
    if (name == NULL || sim == NULL) {
        return NULL;
    }

    Host *host = (Host *)malloc(sizeof(Host));
    if (!host) {
        return NULL;
    }
    memset(host, 0, sizeof(Host));
    memcpy(host->base.name, name, sizeof(host->base.name) - 1);
    host->base.iface_max   = HOST_MAX_PORTS;
    host->base.interfaces  = (Interface **)malloc(sizeof(Interface *) * HOST_MAX_PORTS);
    if (!host->base.interfaces) {
        host_free(host);
        return NULL;
    }
    host->base.iface_count = 0;

    host->arp_cache = malloc(sizeof(ArpCache));
    arp_cache_init(host->arp_cache);

    host->ip_stack  = ip_stack_create(sim, host->arp_cache);
    if (!host->ip_stack) {
        arp_cache_free(host->arp_cache);
        host_free(host);
        return NULL;
    }

    host->udp_state   = malloc(sizeof(UdpState));
    if (!host->udp_state) {
        ip_stack_free(host->ip_stack);
        arp_cache_free(host->arp_cache);
        host_free(host);
        return NULL;
    }
    udp_init(host->udp_state);

    host->tcp_table   = malloc(sizeof(TcpTable));
    if (!host->tcp_table) {
        free(host->udp_state);
        ip_stack_free(host->ip_stack);
        arp_cache_free(host->arp_cache);
        host_free(host);
        return NULL;
    }
    tcp_init(host->tcp_table);

    host->udp_context = malloc(sizeof(UdpContext));
    if (!host->udp_context) {
        free(host->tcp_table);
        free(host->udp_state);
        ip_stack_free(host->ip_stack);
        arp_cache_free(host->arp_cache);
        host_free(host);
        return NULL;
    }
    host->udp_context->sim   = sim;
    host->udp_context->state = host->udp_state;

    host->tcp_context = malloc(sizeof(TcpContext));
    if (!host->tcp_context) {
        free(host->udp_context);
        free(host->tcp_table);
        free(host->udp_state);
        ip_stack_free(host->ip_stack);
        arp_cache_free(host->arp_cache);
        host_free(host);
        return NULL;
    }
    host->tcp_context->sim   = sim;
    host->tcp_context->table = host->tcp_table;

    ip_stack_register_protocol(host->ip_stack,
                               IPPROTO_ICMP,
                               icmp_receive,
                               sim);

    ip_stack_register_protocol(host->ip_stack,
                               IPPROTO_UDP,
                               udp_receive,
                               host->udp_context);

    ip_stack_register_protocol(host->ip_stack,
                               IPPROTO_TCP,
                               tcp_receive,
                               host->tcp_context);

    host->gateway_ip       = gateway_ip;
    return host;
}

void  host_free(Host *host);

int   host_add_interface(Host *host, Interface *iface);

int   host_receive(Host      *host,
                   Interface *iface,
                   Packet    *pkt,
                   uint16_t   ethertype);

int   host_send_ip(Host    *host,
                   uint32_t src_ip,
                   uint32_t dst_ip,
                   uint8_t  protocol,
                   Packet  *payload);
