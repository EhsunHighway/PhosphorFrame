#include "udp.h"
#include "icmp.h"
#include <string.h>

void udp_init(UdpState *state) {
    if (!state) {
        return;
    }

    for (int i = 0; i < UDP_MAX_SOCKETS; i++) {
        state->sockets[i].valid = 0;
    }
    state->count = 0;
}

int  udp_bind(UdpState        *state,
              uint16_t         port,
              Udp_Recv_Handler recv_handler,
              void            *ctx) {
    if (!state || !recv_handler || port == 0) {
        return -1;
    }

    for (int i = 0; i < UDP_MAX_SOCKETS; i++) {
        if (state->sockets[i].valid && state->sockets[i].port == port) {
            return -1;
        }
    }

    for (int i = 0; i < UDP_MAX_SOCKETS; i++) {
        if (state->sockets[i].valid == 0) {
            state->sockets[i].valid        = 1;
            state->sockets[i].port         = port;
            state->sockets[i].recv_handler = recv_handler;
            state->sockets[i].ctx          = ctx;
            state->count++;
            return 0;
        }
    }
    return -1;
}

int  udp_unbind(UdpState *state, uint16_t port) {
    if (!state || port == 0) {
        return -1;
    }

    for (int i = 0; i < UDP_MAX_SOCKETS; i++) {
        if (state->sockets[i].valid == 1 && state->sockets[i].port == port) {
            state->sockets[i].valid = 0;
            state->sockets[i].port  = 0;
            state->count--;
            return 0;
        }
    }
    return -1;
}

int  udp_send(Simulator     *sim,
              uint32_t       src_ip,
              uint32_t       dst_ip,
              uint16_t       src_port,
              uint16_t       dst_port,
              const uint8_t *payload,
              size_t         payload_len) {
    if (!sim || dst_port == 0 || (payload_len > 0 && !payload)) {
        return -1;
    }

    if (payload_len > UINT16_MAX - UDP_HDR_LEN) {
        return -1;
    }

    Packet *pkt = packet_create(UDP_HDR_LEN + payload_len);
    if (!pkt) {
        return -1;
    }

    UdpHeader *hdr = (UdpHeader *)pkt->data;
    hdr->src_port  = ns_htons(src_port);
    hdr->dst_port  = ns_htons(dst_port);
    hdr->length    = ns_htons(UDP_HDR_LEN + payload_len);
    hdr->checksum  = 0;

    if (payload_len > 0) {
        memcpy(pkt->data + UDP_HDR_LEN, payload, payload_len);
    }
    pkt->len   = UDP_HDR_LEN + payload_len;
    pkt->layer = 4;
    int res = ip_output(sim, src_ip, dst_ip, IPPROTO_UDP, pkt);
    if (res ==  -1) {
        packet_free(pkt);
        return -1;
    }
    return 0;
}

int  udp_receive(Interface *iface,
                 Packet    *pkt,
                 void      *ctx) {
    if (!iface) {
        return -1;
    }

    if (!pkt) {
        iface->rx_errors++;
        return -1;
    }

    if (!ctx) {
        packet_free(pkt);
        iface->rx_errors++;
        return -1;
    }

    UdpContext *udp_ctx = (UdpContext *)ctx;
    if (udp_ctx->state == NULL) {
        packet_free(pkt);
        iface->rx_errors++;
        return -1;
    }
    if (pkt->len < UDP_HDR_LEN) {
        packet_free(pkt);
        iface->rx_errors++;
        return -1;
    }

    if (pkt->head == NULL || pkt->data == NULL) {
        packet_free(pkt);
        iface->rx_errors++;
        return -1;
    }

    uint8_t *end = pkt->head + PKT_HEADROOM + pkt->capacity;
    if ((pkt->data < pkt->head + IP_HDR_LEN) ||
        (pkt->data >= end) || (pkt->len > (size_t)(end - pkt->data))) {
        packet_free(pkt);
        iface->rx_errors++;
        return -1;
    }

    IpHeader *ip_hdr = (IpHeader *)(pkt->data - IP_HDR_LEN);
    if (ip_hdr->protocol != IPPROTO_UDP) {
        packet_free(pkt);
        iface->rx_errors++;
        return -1;
    }

    UdpHeader *udp_hdr   = (UdpHeader *)pkt->data;
    uint16_t   src_port  = ns_ntohs(udp_hdr->src_port);
    uint16_t   dst_port  = ns_ntohs(udp_hdr->dst_port);
    uint16_t   udp_len   = ns_ntohs(udp_hdr->length);
    uint32_t   src_ip    = ns_ntohl(ip_hdr->src_ip);

    if (udp_len < UDP_HDR_LEN) {
        packet_free(pkt);
        iface->rx_errors++;
        return -1;
    }

    if (udp_len > pkt->len) {
        packet_free(pkt);
        iface->rx_errors++;
        return -1;
    }

    for (int i = 0; i < UDP_MAX_SOCKETS; i++) {
        if (udp_ctx->state->sockets[i].valid == 1 &&
            udp_ctx->state->sockets[i].port == dst_port) {
            if (packet_strip(pkt, UDP_HDR_LEN) < 0) {
                packet_free(pkt);
                iface->rx_errors++;
                return -1;
            }
            pkt->layer = 5;
            udp_ctx->state->sockets[i].recv_handler(src_ip,
                                                    src_port,
                                                    pkt,
                                                    udp_ctx->state->sockets[i].ctx);
            return 0;
        }
    }
    if (udp_ctx->sim) {
        return icmp_send_unreach_port(udp_ctx->sim, iface, pkt);
    } else {
        packet_free(pkt);
        iface->rx_dropped++;
        return -1;
    }
}
