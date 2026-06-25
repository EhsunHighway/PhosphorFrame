#include "ip.h"
#include "arp.h"
#include "arp_cache.h"
#include <stdlib.h>
#include <string.h>

static void ip_rx_shim(Interface *iface,
                       Packet    *pkt,
                       uint16_t   ethertype,
                       void      *ctx) {
    ip_receive(iface,
               pkt,
               ethertype,
               ctx);
}

static uint32_t ip_prefix_mask(uint8_t prefix_len) {
    if (prefix_len == 0) {
        return 0;
    }
    if (prefix_len >= 32) {
        return 0xFFFFFFFFu;
    }
    return 0xFFFFFFFFu << (32 - prefix_len);
}

static int ip_same_subnet(uint32_t a_host,
                          uint32_t b_host,
                          uint8_t  prefix_len) {
    uint32_t mask = ip_prefix_mask(prefix_len);
    return (a_host & mask) == (b_host & mask);
}

static Interface *ip_find_source_iface(const Simulator *sim, uint32_t src_ip) {
    if (!sim || !sim->topo) {
        return NULL;
    }

    uint32_t src_net = ns_htonl(src_ip);
    for (int i = 0; i < sim->topo->dev_count; i++) {
        Device *dev = sim->topo->devices[i];
        if (!dev) {
            continue;
        }

        for (int j = 0; j < dev->iface_count; j++) {
            Interface *iface = dev->interfaces[j];
            if (iface && iface->ip_addr == src_net) {
                return iface;
            }
        }
    }

    return NULL;
}


void ip_init(Simulator *sim, IpStack *stack) {
    if (!sim || !sim->topo || !stack) {
        return;
    }

    ip_stack_init(stack, sim);

    for (int i = 0; i < sim->topo->dev_count; i++) {
        Device *dev = sim->topo->devices[i];
        for (int j = 0; j < dev->iface_count; j++) {
            ip_stack_bind_interface(stack, dev->interfaces[j]);
        }
    }
    
}

void ip_stack_init(IpStack *stack, Simulator *sim) {
    if (!stack) {
        return;
    }

    memset(stack, 0, sizeof(*stack));
    stack->sim = sim;
}

int ip_stack_bind_interface(IpStack *stack, Interface *iface) {
    if (!stack || !iface) {
        return -1;
    }

    interface_set_rx_handler(iface, ip_rx_shim, stack);
    return 0;
}

int ip_stack_register_protocol(IpStack           *stack,
                               uint8_t            protocol,
                               IpProtocolHandler  handler,
                               void              *ctx) {
    if (!stack || !handler) {
        return -1;
    }

    stack->protocols[protocol].handler = handler;
    stack->protocols[protocol].ctx     = ctx;
    return 0;
}

int ip_stack_unregister_protocol(IpStack *stack, uint8_t protocol) {
    if (!stack) {
        return -1;
    }

    stack->protocols[protocol].handler = NULL;
    stack->protocols[protocol].ctx     = NULL;
    return 0;
}

int  ip_receive(Interface *iface,
                 Packet   *frame,
                 uint16_t  ethertype,
                 void     *ctx) {
    (void)ethertype;

    if (!iface || !frame || frame->len < IP_HDR_LEN) {
        return -1;
    }

    IpHeader *ip_hdr = (IpHeader *)frame->data;
    if (ip_hdr->version_ihl >> 4 != 4) {
        iface->rx_errors++;
        packet_free(frame);
        return -1; // Not IPv4
    }

    if (ip_hdr->ttl == 0) {
        iface->rx_dropped++;
        packet_free(frame);
        return -1; // TTL expired
    }

    if (ip_checksum(ip_hdr) != 0) { 
        iface->rx_errors++; 
        packet_free(frame); 
        return -1; 
    }

     // Verify checksum
    uint8_t protocol    = ip_hdr->protocol;
    iface->rx_bytes += frame->len;
    IpStack   *stack    = (IpStack *)ctx;
    Simulator *sim = stack ? stack->sim : NULL;
    iface->last_rx_time = sim ? simulator_now(sim) : 0;
    frame->data        += IP_HDR_LEN;
    frame->len         -= IP_HDR_LEN;
    frame->layer        = 4;

    if (!stack) {
        return 0;
    }

    IpProtocolEntry *entry = &stack->protocols[protocol];
    if (entry->handler) {
        return entry->handler(iface, frame, entry->ctx);
    }

    return 0;
}


int  ip_send(Simulator *sim,
             Interface *iface,
             uint8_t    dst_mac[6],
             uint32_t   src_ip,
             uint32_t   dst_ip,
             uint8_t    protocol,
             Packet    *payload) {
    if (!sim || !iface || !dst_mac || !payload) {
        return -1;
    }

    IpHeader ip_hdr;
    ip_hdr.version_ihl           = (4 << 4) | (IP_HDR_LEN / 4);
    ip_hdr.dscp_ecn              = 0;
    ip_hdr.total_length          = ns_htons(IP_HDR_LEN + payload->len);
    ip_hdr.identification        = 0;
    ip_hdr.flags_fragment_offset = 0;
    ip_hdr.ttl                   = IP_DEFAULT_TTL;
    ip_hdr.protocol              = protocol;
    ip_hdr.header_checksum       = 0;
    ip_hdr.src_ip                = ns_htonl(src_ip);
    ip_hdr.dst_ip                = ns_htonl(dst_ip);
    ip_hdr.header_checksum       = ip_checksum(&ip_hdr);

    if (packet_prepend(payload, &ip_hdr, sizeof(IpHeader)) == -1) {
        return -1;
    }
    payload->layer      = 3;
    iface->tx_bytes    += payload->len;
    iface->last_tx_time = simulator_now(sim);
    return ethernet_send(sim,
                         iface,
                         dst_mac,
                         ETHERTYPE_IPV4,
                         payload);
}

int ip_output(Simulator *sim,
              uint32_t  src_ip,
              uint32_t  dst_ip,
              uint8_t   protocol,
              Packet   *payload) {
    if (!sim || !payload) {
        return -1;
    }

    Interface *iface = ip_find_source_iface(sim, src_ip);
    if (!iface || !interface_is_up(iface) || !iface->arp_cache) {
        return -1;
    }

    uint32_t iface_ip = ns_ntohl(iface->ip_addr);
    if (!ip_same_subnet(iface_ip, dst_ip, iface->prefix_len)) {
        return -1;
    }

    uint8_t dst_mac[ETH_ALEN];
    if (arp_cache_lookup(iface->arp_cache, dst_ip, dst_mac) != 0) {
        if (arp_send_request(sim, iface, ns_htonl(dst_ip)) != 0) {
            return -1;
        }
        if (arp_pending_enqueue(iface->arp_cache,
                                iface,
                                dst_ip,
                                src_ip,
                                dst_ip,
                                protocol,
                                payload) != 0) {
            return -1;
        }
        return 0;
    }

    return ip_send(sim,
                   iface,
                   dst_mac,
                   src_ip,
                   dst_ip,
                   protocol,
                   payload);
}

/*
 * The checksum is calculated by treating the header as a sequence of 16-bit words, 
 * summing them up, and then taking the one's complement of the sum. 
 * The checksum field itself is set to 0 during the calculation. 
 */
uint16_t  ip_checksum(IpHeader *ip_hdr) {
    if (!ip_hdr) {
        return 0xFFFF; 
    }

    uint32_t sum = 0;
    uint16_t *ptr = (uint16_t *)ip_hdr;
    for (int i = 0; i < IP_HDR_LEN / 2; i++) {
        sum += ns_ntohs(ptr[i]);
    }

    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return ns_htons(~sum);
}

int  ip_is_local(Device *dev, uint32_t dst_ip) {
    if (!dev) {
        return 0;
    }

    for (int i = 0; i < dev->iface_count; i++) {
        if (dev->interfaces[i]->ip_addr == dst_ip) {
            return 1;
        }
    }
    return 0;
}
