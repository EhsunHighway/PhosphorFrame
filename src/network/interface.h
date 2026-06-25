#ifndef INTERFACE_H
#define INTERFACE_H

#include <stdint.h>
#include <stddef.h>

/* 
 * Forward declarations for opaque types to break circular dependencies.
 * The actual definitions are in their respective header files.
 */
struct Link;
struct Packet;
struct Interface;
struct Device;
struct ArpCache;

/*
 * Upward declarations of protocol functions that need to call back into the interface layer.
 */ 
typedef void (*RxHandler)(struct Interface *iface,
                          struct Packet    *pkt,
                          uint16_t          ethertype,
                          void             *ctx);

typedef enum {
    IFACE_OK,
    IFACE_ERR_DISABLED,  // too many errors → stop forwarding
} InterfaceState;


typedef struct Interface {
    char            name[16];        // e.g.: "eth0", "ge0/0"
    uint8_t         mac[6];          // 48-bit MAC, network byte order
    uint32_t        ip_addr;         // IPv4, network byte order
    uint8_t         prefix_len;      // 0-32 CIDR prefix length (subnet mask)
    uint16_t        mtu;             // max transmission unit default 1500
    int             up;              // 1=up, 0=down
    struct Link    *link;            // NULL if not connected
    struct Device  *device;          // back-pointer to owning device (set by device_add_interface)
    uint64_t        tx_bytes;        // total bytes transmitted (for stats)
    uint64_t        rx_bytes;        // total bytes received (for stats)
    RxHandler       rx_handler;      // callback for received packets
    void           *handler_ctx;     // opaque context passed to rx_handler
    struct ArpCache *arp_cache;      // borrowed L3 neighbor cache, owned by host/router
    uint64_t        rx_dropped;      // wrong MAC, etc.
    uint64_t        rx_errors;       // malformed, truncated
    uint64_t        tx_errors;       // link down, alloc failed
    InterfaceState  state;           // interface operational state
    uint64_t        last_rx_time;    // last successful RX (sim time µs)
    uint64_t        last_tx_time;    // last successful TX (sim time µs)
    uint64_t        last_error_time; // last error event (sim time µs)
} Interface;




/*@ 
    requires name != \null;
    requires mac != \null;
    requires prefix_len <= 32;
    requires mtu > 0;
    allocates \result;
    ensures \result != \null ==>
        (\result->ip_addr == ip_addr &&
         \result->prefix_len == prefix_len &&
         \result->mtu == mtu &&
         \result->up == 0 &&
         \result->link == \null &&
         \result->device == \null &&
         \result->tx_bytes == 0 &&
         \result->rx_bytes == 0 &&
         \result->rx_handler == \null &&
         \result->handler_ctx == \null &&
         \result->arp_cache == \null &&
         \result->rx_dropped == 0 &&
         \result->rx_errors == 0 &&
         \result->tx_errors == 0 &&
         \result->state == IFACE_OK &&
         \result->last_rx_time == 0 &&
         \result->last_tx_time == 0 &&
         \result->last_error_time == 0);
*/
Interface     *interface_create(const char    *name,
                                const uint8_t  mac[6],
                                uint32_t       ip_addr,
                                uint8_t        prefix_len,
                                uint16_t       mtu);

/*@ 
    requires iface == \null || \valid(iface);
    assigns \nothing; 
*/
void           interface_free(Interface *iface);

/*@
    behavior null:
        assumes iface == \null;
        assigns \nothing;
    behavior valid:
        assumes iface != \null;
        assigns  iface->up;
        ensures  iface->up == up;
    complete behaviors;
    disjoint behaviors;
*/
void           interface_set_up(Interface *iface, int up);

/*@
    behavior null:
        assumes iface == \null;
        assigns \nothing;
        ensures \result == 0;
    behavior valid:
        assumes iface != \null;
        assigns \nothing;
        ensures \result == 1 || \result == 0;
        ensures iface->up != 0 ==> \result == 1;
        ensures iface->up == 0 ==> \result == 0;
    complete behaviors;
    disjoint behaviors;
*/
int            interface_is_up(const Interface *iface);

/*@
    behavior null:
        assumes iface == \null;
        assigns \nothing;
    behavior valid:
        assumes iface != \null;
        assigns iface->link;
        ensures iface->link == link;
    complete behaviors;
    disjoint behaviors;
*/
void           interface_set_link(Interface *iface, struct Link *link);

/*@
    behavior null:
        assumes iface == \null;
        assigns \nothing;
        ensures \result == \null;
    behavior valid:
        assumes iface != \null;
        assigns \nothing;
        ensures \result == iface->link;
    complete behaviors;
    disjoint behaviors;
*/
struct Link   *interface_get_link(const Interface *iface);

/*@
    requires iface != \null;
    assigns \nothing;
    ensures \result == iface->mac;
*/
const uint8_t *interface_get_mac(const Interface *iface);

/*@
    requires iface != \null;
    assigns \nothing;
    ensures \result == iface->ip_addr;
*/
uint32_t       interface_get_ip(const Interface *iface);

/*@
    behavior null:
        assumes iface == \null;
        assigns \nothing;
    behavior valid:
        assumes iface != \null;
        assigns iface->tx_bytes;
        ensures iface->tx_bytes == \old(iface->tx_bytes) + n;
    complete behaviors;
    disjoint behaviors;
*/
void           interface_add_tx_bytes(Interface *iface, uint64_t n);

/*@
    behavior null:
        assumes iface == \null;
        assigns \nothing;
    behavior valid:
        assumes iface != \null;
        assigns iface->rx_bytes;
        ensures iface->rx_bytes == \old(iface->rx_bytes) + n;
    complete behaviors;
    disjoint behaviors;
*/
void           interface_add_rx_bytes(Interface *iface, uint64_t n);

/*@
    behavior null:
        assumes iface == \null;
        assigns \nothing;
    behavior valid:
        assumes iface != \null;
        assigns iface->rx_handler, iface->handler_ctx;
        ensures iface->rx_handler == fn;
        ensures iface->handler_ctx == ctx;
    complete behaviors;
    disjoint behaviors;
*/
void           interface_set_rx_handler(Interface *iface,
                                        RxHandler  fn,
                                        void      *ctx);

/*@
    behavior null:
        assumes iface == \null;
        assigns \nothing;
    behavior valid:
        assumes iface != \null;
        assigns iface->arp_cache;
        ensures iface->arp_cache == cache;
    complete behaviors;
    disjoint behaviors;
*/
void           interface_set_arp_cache(Interface *iface, struct ArpCache *cache);

#endif /* INTERFACE_H */
