#include <string.h>
#include "../network/packet.h"
#include "ip.h"
#include "arp_cache.h"

#define ARP_CACHE_TIMEOUT_MS 300000
#define ARP_HARDWARE_ADDR_LEN 6

void arp_cache_init(ArpCache *cache) {
    if (!cache) {
        return;
    }

    memset(cache, 0, sizeof(*cache));
}

void arp_cache_add(ArpCache     *cache,
                   uint32_t      ip_addr,
                   const uint8_t mac_addr[6],
                   uint64_t      timestamp) {
    if (!cache || ip_addr == 0) {
        return;
    }

    for (int i = 0; i < ARP_MAX_CACHE_SIZE; i++) {
        if ((cache->entries[i].valid == 1) &&
            (cache->entries[i].ip_addr == ip_addr)) {
            memcpy(cache->entries[i].mac_addr, mac_addr, ARP_HARDWARE_ADDR_LEN);
            cache->entries[i].timestamp = timestamp;
            return;
        }
    }

    for (int i = 0; i < ARP_MAX_CACHE_SIZE; i++) {
        if (cache->entries[i].valid == 0) {
            cache->entries[i].ip_addr   = ip_addr;
            memcpy(cache->entries[i].mac_addr, mac_addr, ARP_HARDWARE_ADDR_LEN);
            cache->entries[i].timestamp = timestamp;
            cache->entries[i].valid     = 1;
            cache->count++;
            return;
        }
    }
}

int arp_cache_lookup(const ArpCache *cache,
                     uint32_t        ip_addr,
                     uint8_t         out_mac[6]) {
    if (!cache || !ip_addr || !out_mac) {
        return -1;
    }

    for (int i = 0; i < ARP_MAX_CACHE_SIZE; i++) {
        if ((cache->entries[i].valid == 1) && (cache->entries[i].ip_addr == ip_addr)) {
            memcpy(out_mac, cache->entries[i].mac_addr, ARP_HARDWARE_ADDR_LEN);
            return 0;
        }
    }

    return -1;
}

int arp_pending_enqueue(ArpCache  *cache,
                        Interface *iface,
                        uint32_t   target_ip,
                        uint32_t   src_ip,
                        uint32_t   dst_ip,
                        uint8_t    protocol,
                        Packet    *payload) {
    if (!cache || !iface || !target_ip || !payload) {
        return -1;
    }

    for (int i = 0; i < ARP_MAX_PENDING_PACKETS; i++) {
        if (cache->pending[i].valid == 0) {
            cache->pending[i].target_ip = target_ip;
            cache->pending[i].src_ip    = src_ip;
            cache->pending[i].dst_ip    = dst_ip;
            cache->pending[i].protocol  = protocol;
            cache->pending[i].iface     = iface;
            cache->pending[i].payload   = payload;
            cache->pending[i].valid     = 1;
            cache->pending_count++;
            return 0;
        }
    }

    return -1;
}

int arp_pending_flush(Simulator    *sim,
                      ArpCache     *cache,
                      uint32_t      target_ip,
                      const uint8_t mac_addr[6]) {
    if (!sim || !cache || !target_ip || !mac_addr) {
        return 0;
    }

    int sent = 0;
    for (int i = 0; i < ARP_MAX_PENDING_PACKETS; i++) {
        ArpPendingPacket *pending = &cache->pending[i];
        if (!pending->valid || pending->target_ip != target_ip) {
            continue;
        }

        Packet    *payload  = pending->payload;
        Interface *iface    = pending->iface;
        uint32_t   src_ip   = pending->src_ip;
        uint32_t   dst_ip   = pending->dst_ip;
        uint8_t    protocol = pending->protocol;

        pending->valid   = 0;
        pending->payload = NULL;
        pending->iface   = NULL;
        if (cache->pending_count > 0) {
            cache->pending_count--;
        }

        if (iface && payload &&
            ip_send(sim,
                    iface,
                    (uint8_t *)mac_addr,
                    src_ip,
                    dst_ip,
                    protocol,
                    payload) >= 0) {
            sent++;
        } else {
            packet_free(payload);
        }
    }

    return sent;
}

void arp_cache_cleanup(ArpCache *cache, uint64_t current_time) {
    if (!cache) {
        return;
    }

    for (int i = 0; i < ARP_MAX_CACHE_SIZE; i++) {
        if ((cache->entries[i].valid == 1) &&
            (current_time - cache->entries[i].timestamp >= ARP_CACHE_TIMEOUT_MS)) {
            cache->entries[i].valid = 0;
            cache->count--;
        }
    }
}
