#include "ethernet.h"
#include <string.h>
#include <stdlib.h>

const uint8_t ETH_BROADCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

static int mac_equal(const uint8_t a[6], const uint8_t b[6]) {
    return memcmp(a, b, 6) == 0;
}

void ethernet_receive_event(const Event *e, void *ctx) {
    (void)ctx;

    Interface *iface  = (Interface *)e->dst_device;
    Packet    *frame  = (Packet *)e->packet;

    if (!iface || !frame) {
        return;
    }

    uint16_t ethertype = 0;
    int      result    = ethernet_receive(iface, frame, &ethertype);

    if (result == 0) {
        iface->last_rx_time = e->timestamp;
        if (iface->rx_handler) {
            iface->rx_handler(iface, frame, ethertype, iface->handler_ctx);
        }
    } else if (result == 1) {
        iface->rx_dropped++;  // Frame silently dropped (wrong MAC)
    } else {
        iface->rx_errors++;   // Error (result == -1)
        iface->last_error_time = e->timestamp;
    }
}

int  ethernet_send(Simulator    *sim,
                   Interface    *iface,
                   const uint8_t dst_mac[6],
                   uint16_t      ethertype,
                   Packet       *payload) {
    if (!sim || !iface || !dst_mac || !payload) {
        return -1;
    }

    if (iface->state == IFACE_ERR_DISABLED) {
        return -1;  // Interface is error-disabled
    }

    EthernetHeader *eth_hdr = malloc(sizeof(EthernetHeader));
    if (!eth_hdr) {
        iface->tx_errors++;
        iface->last_error_time = simulator_now(sim);
        return -1;
    }

    memcpy(eth_hdr->dst_mac, dst_mac, 6);
    memcpy(eth_hdr->src_mac, iface->mac, 6);
    eth_hdr->ethertype = ns_htons(ethertype);

    if (packet_prepend(payload, eth_hdr, ETH_HDR_LEN) == -1) {
        free(eth_hdr);
        iface->tx_errors++;
        iface->last_error_time = simulator_now(sim);
        return -1;
    }

    free(eth_hdr);

    iface->tx_bytes += payload->len;
    iface->last_tx_time = simulator_now(sim);
    payload->layer = 2;

    return link_transmit(iface->link, payload, iface, sim->sched, simulator_now(sim),
                         ethernet_receive_event, sim);
}

int  ethernet_receive(Interface *iface,
                      Packet    *frame,
                      uint16_t  *out_ethertype) {
    if (!iface || !frame || !out_ethertype || frame->len < ETH_HDR_LEN) {
        return -1;
    }

    if (iface->state == IFACE_ERR_DISABLED) {
        return -1;  // Interface is error-disabled
    }

    EthernetHeader *eth_hdr = (EthernetHeader *)frame->data;
    if (!mac_equal(eth_hdr->dst_mac, iface->mac) && !mac_equal(eth_hdr->dst_mac, ETH_BROADCAST)) {
        return 1;  // Not for this interface - drop
    }
    /*
     * Extract the EtherType from the Ethernet header and convert it from network byte order to host byte order.
     * Network protocols use big-endian by convention; This is called “network byte order”.
     */
    *out_ethertype = ns_ntohs(eth_hdr->ethertype);

    if (packet_strip(frame, ETH_HDR_LEN) == -1 ) {
        return -1;
    }

    iface->rx_bytes += frame->len;
    frame->layer = 3;

    return 0;
}
