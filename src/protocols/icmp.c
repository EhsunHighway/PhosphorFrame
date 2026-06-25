#include <string.h>
#include "icmp.h"

static int icmp_send_error(Simulator *sim,
                           Interface *iface,
                           Packet    *orig_pkt,
                           uint8_t    type,
                           uint8_t    code,
                           uint16_t   next_hop_mtu) {
    if (!sim || !iface || !orig_pkt) {
        return -1;
    }

    if (!orig_pkt->head || !orig_pkt->data) {
        iface->tx_errors++;
        return -1;
    }

    if (orig_pkt->data < orig_pkt->head + IP_HDR_LEN) {
        iface->tx_errors++;
        return -1;
    }

    uint8_t *end = orig_pkt->head + PKT_HEADROOM + orig_pkt->capacity;
    if (orig_pkt->data >= end || orig_pkt->len > (size_t)(end - orig_pkt->data)) {
        iface->tx_errors++;
        return -1;
    }

    IpHeader *orig_ip = (IpHeader *)(orig_pkt->data - IP_HDR_LEN);
    if (orig_ip->protocol == IPPROTO_ICMP) {
        if (orig_pkt->len < ICMP_HDR_LEN) {
            return 0;
        }
        // Do not send an ICMP error in response to an ICMP error.
        IcmpHeader *orig_icmp = (IcmpHeader *)orig_pkt->data;
        if (orig_icmp->type == ICMP_DEST_UNREACH ||
            orig_icmp->type == ICMP_TIME_EXCEEDED) {
            return 0; // suppress: send nothing
        }
    }

    size_t payload_quote_len = orig_pkt->len < 8 ? orig_pkt->len : 8;
    size_t  quote_len         = IP_HDR_LEN + payload_quote_len;
    size_t  error_len         = ICMP_HDR_LEN + quote_len;
    Packet *err_pkt           = packet_create(error_len);
    if (!err_pkt) {
        iface->tx_errors++;
        return -1;
    }

    err_pkt->len   = error_len;
    err_pkt->layer = 4;

    IcmpHeader *icmp_hdr = (IcmpHeader *)err_pkt->data;
    icmp_hdr->type     = type;
    icmp_hdr->code     = code;
    icmp_hdr->checksum = 0;
    icmp_hdr->id       = 0;
    if (code == ICMP_CODE_FRAG_NEEDED) {
        icmp_hdr->seq  = ns_htons(next_hop_mtu);
    } else {
        icmp_hdr->seq  = 0;
    }

    uint8_t *quote = err_pkt->data + ICMP_HDR_LEN;
    memcpy(quote, orig_pkt->data - IP_HDR_LEN, IP_HDR_LEN);
    memcpy(quote + IP_HDR_LEN, orig_pkt->data, payload_quote_len);
    icmp_hdr->checksum = icmp_checksum(err_pkt->data, err_pkt->len);

    uint32_t src_ip = ns_ntohl(iface->ip_addr);
    uint32_t dst_ip = ns_ntohl(orig_ip->src_ip);
    int res = ip_output(sim, src_ip, dst_ip, IPPROTO_ICMP, err_pkt);
    if (res == -1) {
        iface->tx_errors++;
        packet_free(err_pkt);
        return -1;
    }
    return res;
}

int        icmp_receive(Interface *iface,
                        Packet    *pkt,
                        void      *ctx) {
    Simulator *sim = (Simulator *)ctx;
    if (!iface) {
        return -1;
    }

    if (!pkt) {
        iface->rx_errors++;
        return -1;
    }
    if (pkt->len < ICMP_HDR_LEN) {
        iface->rx_errors++;
        packet_free(pkt);
        return -1;
    }

    if (!pkt->head || !pkt->data) {
        iface->rx_errors++;
        packet_free(pkt);
        return -1;
    }

    uint8_t *end       = pkt->head + PKT_HEADROOM + pkt->capacity;
    if (pkt->data < pkt->head + IP_HDR_LEN || pkt->data >= end) {
        iface->rx_errors++;
        packet_free(pkt);
        return -1;
    }

    size_t   remaining = (size_t)(end - pkt->data);
    if (pkt->len > remaining) {
        iface->rx_errors++;
        packet_free(pkt);
        return -1;
    }

    if (icmp_checksum(pkt->data, pkt->len) != 0) {
        iface->rx_errors++;
        packet_free(pkt);
        return -1;
    }

    IcmpHeader *icmp_hdr = (IcmpHeader *)pkt->data;

    switch (icmp_hdr->type) {
        case ICMP_ECHO_REQUEST:
            if (icmp_hdr->code == 0) {
                return icmp_send_echo_reply(sim, iface, pkt);
            }
            break;
        case ICMP_ECHO_REPLY:
            if (icmp_hdr->code == 0) {
                packet_free(pkt);
                return 0;
            }
            break;
        case ICMP_DEST_UNREACH:
            if (icmp_hdr->code == 0 || icmp_hdr->code == 1 ||
                icmp_hdr->code == 2 || icmp_hdr->code == 3 ||
                icmp_hdr->code == 4) {
                packet_free(pkt);
                return 0;
            }
            break;
        case ICMP_TIME_EXCEEDED:
            if (icmp_hdr->code == 0) {
                packet_free(pkt);
                return 0;
            }
            break;
        default:
            iface->rx_dropped++;
            packet_free(pkt);
            return -1;
    }

    iface->rx_dropped++;
    packet_free(pkt);
    return -1;
}

int        icmp_send_echo_request(Simulator      *sim,
                                  uint32_t       src_ip,
                                  uint32_t       dst_ip,
                                  uint16_t       id,
                                  uint16_t       seq,
                                  const uint8_t *payload,
                                  size_t         payload_len) {
    if (!sim) {
        return -1;
    }

    if (payload_len > 0 && !payload) {
        return -1;
    }

    Packet *pkt = packet_create(ICMP_HDR_LEN + payload_len);
    if (!pkt) {
        return -1;
    }

    IcmpHeader *icmp_hdr = (IcmpHeader *)pkt->data;
    icmp_hdr->type       = ICMP_ECHO_REQUEST;
    icmp_hdr->code       = 0;
    icmp_hdr->id         = ns_htons(id);
    icmp_hdr->seq        = ns_htons(seq);
    icmp_hdr->checksum   = 0;
    pkt->len             = ICMP_HDR_LEN + payload_len;
    pkt->layer           = 4;

    if (payload_len > 0) {
        memcpy(pkt->data + ICMP_HDR_LEN, payload, payload_len);
    }

    icmp_hdr->checksum = icmp_checksum(pkt->data, pkt->len);
    int res = ip_output(sim, src_ip, dst_ip, IPPROTO_ICMP, pkt);
    if (res == -1) {
        packet_free(pkt);
    }

    return res;
}

int        icmp_send_echo_reply(Simulator *sim,
                                Interface *iface,
                                Packet    *req_pkt) {
    if (!sim || !iface || !req_pkt) {
        return -1;
    }

    if (req_pkt->len < ICMP_HDR_LEN) {
        iface->tx_errors++;
        packet_free(req_pkt);
        return -1;
    }

    if (!req_pkt->head || !req_pkt->data) {
        iface->tx_errors++;
        packet_free(req_pkt);
        return -1;
    }

    if (req_pkt->data < req_pkt->head + IP_HDR_LEN) {
        iface->tx_errors++;
        packet_free(req_pkt);
        return -1;
    }

    uint8_t *end = req_pkt->head + PKT_HEADROOM + req_pkt->capacity;
    if (req_pkt->data >= end) {
        iface->tx_errors++;
        packet_free(req_pkt);
        return -1;
    }

    size_t remaining = (size_t)(end - req_pkt->data);
    if (req_pkt->len > remaining) {
        iface->tx_errors++;
        packet_free(req_pkt);
        return -1;
    }

    IcmpHeader *req_icmp  = (IcmpHeader *)req_pkt->data;
    if (req_icmp->type != ICMP_ECHO_REQUEST || req_icmp->code != 0) {
        iface->tx_errors++;
        packet_free(req_pkt);
        return -1;
    }

    IpHeader   *req_ip    = (IpHeader *)(req_pkt->data - IP_HDR_LEN);
    Packet     *reply_pkt = packet_create(req_pkt->len);
    if (!reply_pkt) {
        iface->tx_errors++;
        packet_free(req_pkt);
        return -1;
    }

    reply_pkt->len   = req_pkt->len;
    reply_pkt->layer = 4;
    memcpy(reply_pkt->data, req_pkt->data, req_pkt->len);

    IcmpHeader *reply_icmp = (IcmpHeader *)reply_pkt->data;
    reply_icmp->type       = ICMP_ECHO_REPLY;
    reply_icmp->checksum   = 0;
    reply_icmp->checksum   = icmp_checksum(reply_pkt->data, reply_pkt->len);

    uint32_t src_ip = ns_ntohl(req_ip->dst_ip);
    uint32_t dst_ip = ns_ntohl(req_ip->src_ip);

    int res = ip_output(sim, src_ip, dst_ip, IPPROTO_ICMP, reply_pkt);
    if (res == -1) {
        iface->tx_errors++;
        packet_free(reply_pkt);
        packet_free(req_pkt);
        return -1;
    }
    packet_free(req_pkt);
    return res;
}

uint16_t   icmp_checksum(const void *data, size_t len) {
    if (!data || len == 0) {
        return 0xFFFF;
    }

    uint32_t sum = 0;
    const uint16_t *words = (const uint16_t *)data;
    for (size_t i = 0; i < len / 2; i++) {
        sum += ns_ntohs(words[i]);
    }

    if (len % 2 == 1) {
        sum += ((const uint8_t *)data)[len - 1] << 8;
    }

    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return ns_htons((uint16_t)~sum);
}

int      icmp_send_time_exceeded(Simulator *sim,
                                 Interface *iface,
                                 Packet    *orig_pkt) {
    return icmp_send_error(sim,
                           iface,
                           orig_pkt,
                           ICMP_TIME_EXCEEDED,
                           ICMP_CODE_TTL_EXCEEDED,
                           0);
}

int      icmp_send_unreach_net(Simulator *sim,
                               Interface *iface,
                               Packet    *orig_pkt) {
    return icmp_send_error(sim,
                           iface,
                           orig_pkt,
                           ICMP_DEST_UNREACH,
                           ICMP_CODE_NET_UNREACH,
                           0);
}

int      icmp_send_unreach_host(Simulator *sim,
                                Interface *iface,
                                Packet    *orig_pkt) {
    return icmp_send_error(sim,
                           iface,
                           orig_pkt,
                           ICMP_DEST_UNREACH,
                           ICMP_CODE_HOST_UNREACH,
                           0);
}

int      icmp_send_unreach_proto(Simulator *sim,
                                 Interface *iface,
                                 Packet    *orig_pkt) {
    return icmp_send_error(sim,
                           iface,
                           orig_pkt,
                           ICMP_DEST_UNREACH,
                           ICMP_CODE_PROTO_UNREACH,
                           0);
}

int     icmp_send_unreach_port(Simulator *sim,
                               Interface *iface,
                               Packet    *orig_pkt) {
    return icmp_send_error(sim,
                           iface,
                           orig_pkt,
                           ICMP_DEST_UNREACH,
                           ICMP_CODE_PORT_UNREACH,
                           0);
}

int     icmp_send_frag_needed(Simulator *sim,
                              Interface *iface,
                              Packet    *orig_pkt,
                              uint16_t   next_hop_mtu) {
    return icmp_send_error(sim,
                           iface,
                           orig_pkt,
                           ICMP_DEST_UNREACH,
                           ICMP_CODE_FRAG_NEEDED,
                           next_hop_mtu);
}
