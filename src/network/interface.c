#include <stdlib.h>
#include <string.h>
#include "interface.h"

Interface     *interface_create(const char    *name,
                                const uint8_t  mac[6],
                                uint32_t       ip_addr,
                                uint8_t        prefix_len,
                                uint16_t       mtu) {
    Interface *iface = malloc(sizeof(Interface));
    if (!iface) {
        return NULL;
    }

    strncpy(iface->name, name, sizeof(iface->name) - 1);
    iface->name[sizeof(iface->name) - 1] = '\0';  // ensure null-termination
    memcpy(iface->mac, mac, sizeof(iface->mac));
    iface->ip_addr         = ip_addr;
    iface->prefix_len      = prefix_len;
    iface->mtu             = mtu;
    iface->up              = 0;  // default to down
    iface->link            = NULL;
    iface->device          = NULL;
    iface->tx_bytes        = 0;
    iface->rx_bytes        = 0;
    iface->rx_handler      = NULL;
    iface->handler_ctx     = NULL;
    iface->arp_cache       = NULL;
    iface->rx_dropped      = 0;
    iface->rx_errors       = 0;
    iface->tx_errors       = 0;
    iface->state           = IFACE_OK;
    iface->last_rx_time    = 0;
    iface->last_tx_time    = 0;
    iface->last_error_time = 0;
    return iface;
}


void           interface_free(Interface *iface) {
    if (iface) {
        free(iface);
    }
}

void           interface_set_up(Interface *iface, int up) {
    if (iface) {
        iface->up = up; 
    }
}

int            interface_is_up(const Interface *iface) {
    if (iface) {
        return iface->up != 0 ? 1 : 0;
    }
    return 0;
}


void           interface_set_link(Interface *iface, struct Link *link) {
    if (iface) {
        iface->link = link;
    }
}
struct Link   *interface_get_link(const Interface *iface) {
    if (iface) {
        return (struct Link *)iface->link;
    }
    return NULL;
}

const uint8_t *interface_get_mac(const Interface *iface) {
    return iface->mac;
}

uint32_t       interface_get_ip(const Interface *iface) {
    return iface->ip_addr;
}

void           interface_add_tx_bytes(Interface *iface, uint64_t n) {
    if (iface) {
        iface->tx_bytes += n;
    }


}

void           interface_add_rx_bytes(Interface *iface, uint64_t n) {
    if (iface) {
        iface->rx_bytes += n;
    }
}

void           interface_set_rx_handler(Interface *iface,
                                        RxHandler  fn,
                                        void      *ctx) {
    if (iface) {
        iface->rx_handler  = fn;
        iface->handler_ctx = ctx;
    }
}

void           interface_set_arp_cache(Interface *iface, struct ArpCache *cache) {
    if (iface) {
        iface->arp_cache = cache;
    }
}
