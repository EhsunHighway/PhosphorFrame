#include <stdlib.h>
#include "link.h"
#include "../engine/scheduler.h"

Link      *link_create(Interface *end_a, 
                       Interface *end_b, 
                       uint32_t bw, 
                       uint32_t delay, 
                       float loss_rate) {
    if (!end_a || !end_b || bw == 0) {
        return NULL;
    }

    Link *link = malloc(sizeof(Link));
    if (!link) {
        return NULL;
    }
    link->end_a          = end_a;
    link->end_b          = end_b;
    link->bandwidth_mbps = bw;
    link->delay_ms       = delay;
    link->loss_rate      = loss_rate;
    link->up             = 1;
    return link; 
}

void       link_free(Link *link) {
    if (link) {
        free(link);
    }
}

void       link_set_up(Link *link, int up) {
    if (link) {
        link->up = up;
    }
    return;
}

int        link_is_up(const Link *link) {
    if (link) {
        return link->up != 0 ? 1 : 0;
    }
    return 0;
}

Interface *link_get_other_interface(const Link *link, const Interface *src) {
    if (!link || !src) {
        return NULL;
    }

    if (src == link->end_a) {
        return link->end_b;
    } else if (src == link->end_b) {
        return link->end_a;
    } else {
        return NULL; // src is not part of this link
    }
}

int        link_transmit(Link *link, 
                         const Packet *pkt, 
                         const Interface *src,
                         struct Scheduler *sched, 
                         uint64_t now,
                         EventCallback rx_handler, 
                         void *rx_ctx) {
    if (!link || !pkt || !src || !sched) {
        return -1;
    }

    if (!link->up) {
        return -1;
    }
    Interface *dst = link_get_other_interface(link, src);
    if (!dst || !interface_is_up(dst)) {
        return -1;
    }

    Packet *pkt_clone = packet_clone(pkt);
    if (!pkt_clone) {
        return -1;
    }

    Event *e = event_create_callback(EVT_PACKET_RECEIVE,
                                     now + link->delay_ms,
                                     (void *)src,
                                     (void *)dst,
                                     pkt_clone,
                                     NULL,
                                     rx_handler,
                                     rx_ctx);
    if (!e) {
        packet_free(pkt_clone);
        return -1;
    }

    int scheduled = scheduler_schedule(sched, e);
    if (scheduled != 0) {
        packet_free(pkt_clone);
        event_free(e);
    }
    return scheduled == 0 ? 1 : -1;
}
