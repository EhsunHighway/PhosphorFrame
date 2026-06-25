#ifndef ARP_CACHE_H
#define ARP_CACHE_H

#include <stdint.h>

#define ARP_MAX_CACHE_SIZE        256
#define ARP_MAX_PENDING_PACKETS   32

typedef struct Simulator Simulator;
typedef struct Interface Interface;
typedef struct Packet Packet;

typedef struct ArpCacheEntry {
    uint32_t        ip_addr;              // IPv4 address (network byte order)
    uint8_t         mac_addr[6];          // MAC address
    uint64_t        timestamp;            // last updated time (sim time ms)
    int             valid;                // 1 if entry is valid, 0 if expired/invalid
} ArpCacheEntry;

typedef struct ArpPendingPacket {
    uint32_t          target_ip;   /* Host-order next-hop IP waiting for MAC. */
    uint32_t          src_ip;      /* Host-order IPv4 source for ip_send. */
    uint32_t          dst_ip;      /* Host-order IPv4 destination for ip_send. */
    uint8_t           protocol;    /* IPv4 protocol byte. */
    struct Interface *iface;       /* Borrowed outgoing interface. */
    struct Packet    *payload;     /* Owned queued L4 payload. */
    int               valid;
} ArpPendingPacket;

typedef struct ArpCache {
    ArpCacheEntry     entries[ARP_MAX_CACHE_SIZE];
    int               count;
    ArpPendingPacket  pending[ARP_MAX_PENDING_PACKETS];
    int               pending_count;
} ArpCache;

/*@
    behavior null:
        assumes cache == \null;
        assigns \nothing;

    behavior valid:
        assumes \valid(cache);
        assigns cache->entries[0 .. 256-1],
                cache->count,
                cache->pending[0 .. 32-1],
                cache->pending_count;
        ensures cache->count == 0;
        ensures \forall integer i; 0 <= i < 256 ==> cache->entries[i].valid == 0;
        ensures cache->pending_count == 0;
        ensures \forall integer i; 0 <= i < 32 ==> cache->pending[i].valid == 0;
        ensures \forall integer i; 0 <= i < 32 ==> cache->pending[i].iface == \null;
        ensures \forall integer i; 0 <= i < 32 ==> cache->pending[i].payload == \null;

    complete behaviors;
    disjoint behaviors;
*/
void arp_cache_init(ArpCache *cache);

/*@
    behavior null:
        assumes cache == \null || ip_addr == 0;
        assigns \nothing;
    behavior valid:
        assumes \valid(cache) && ip_addr != 0 && \valid_read(mac_addr+(0..5));
        assigns cache->entries[0..255];
    complete behaviors;
    disjoint behaviors;
*/
void arp_cache_add(ArpCache     *cache,
                   uint32_t      ip_addr,
                   const uint8_t mac_addr[6],
                   uint64_t      timestamp);

/*@
    behavior null:
        assumes cache == \null || ip_addr == 0 || out_mac == \null;
        assigns \nothing;
        ensures \result == -1;
    behavior valid:
        assumes \valid_read(cache) && ip_addr != 0 && \valid(out_mac+(0..5));
        assigns \nothing;
        ensures \result == 0 ==> \forall integer i; 0 <= i < 256 ==> (cache->entries[i].ip_addr == ip_addr ==> \forall integer j; 0 <= j < 6 ==> out_mac[j] == cache->entries[i].mac_addr[j]);
    complete behaviors;
    disjoint behaviors;
*/
int  arp_cache_lookup(const ArpCache *cache,
                      uint32_t        ip_addr,
                      uint8_t         out_mac[6]);

/*@
    behavior null:
        assumes cache == \null || iface == \null || payload == \null || target_ip == 0;
        assigns \nothing;
        ensures \result == -1;
    behavior valid:
        assumes \valid(cache) && \valid(iface) && \valid(payload) && target_ip != 0;
        assigns cache->pending[0..31], cache->pending_count;
        ensures \result == 0 || \result == -1;
*/
int  arp_pending_enqueue(ArpCache  *cache,
                         Interface *iface,
                         uint32_t   target_ip,
                         uint32_t   src_ip,
                         uint32_t   dst_ip,
                         uint8_t    protocol,
                         Packet    *payload);

/*@
    behavior null:
        assumes sim == \null || cache == \null || target_ip == 0 || mac_addr == \null;
        assigns \nothing;
        ensures \result == 0;
    behavior valid:
        assumes \valid(sim) && \valid(cache) && target_ip != 0 && \valid_read(mac_addr+(0..5));
        assigns cache->pending[0..31], cache->pending_count;
        ensures \result >= 0;
*/
int  arp_pending_flush(Simulator    *sim,
                       ArpCache     *cache,
                       uint32_t      target_ip,
                       const uint8_t mac_addr[6]);

/*@
    behavior null:
        assumes cache == \null;
        assigns \nothing;
    behavior valid:
        assumes \valid(cache);
        assigns cache->entries[0..255];
    complete behaviors;
    disjoint behaviors;
*/
void arp_cache_cleanup(ArpCache *cache, uint64_t current_time);

#endif /* ARP_CACHE_H */
