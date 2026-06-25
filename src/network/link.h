#ifndef LINK_H
#define LINK_H

#include <stdint.h>
#include "interface.h"
#include "packet.h"
#include "../engine/event.h"

struct Scheduler;              // forward declaration to avoid circular include

typedef struct Link {
    Interface *end_a;             // one end of the link
    Interface *end_b;             // other end of the link
    uint32_t   bandwidth_mbps;    // link bandwidth in megabits per second
    uint32_t   delay_ms;          // link latency in milliseconds
    float      loss_rate;         // packet loss rate (0.0 to 1.0)
    int        up;                // 1=up, 0=down
} Link;


/*@
    behavior null_or_bad:
        assumes end_a == \null || end_b == \null || bw == 0;
        assigns \nothing;
        ensures \result == \null;
    behavior ok:
        assumes end_a != \null && end_b != \null && bw > 0;
        allocates \result;
        ensures \result == \null || \valid(\result);
        ensures \result != \null ==>
            (\result->end_a == end_a &&
             \result->end_b == end_b &&
             \result->bandwidth_mbps == bw &&
             \result->delay_ms == delay &&
             \result->up == 1);
    complete behaviors;
    disjoint behaviors;
*/
Link      *link_create(Interface *end_a,
                       Interface *end_b,
                       uint32_t   bw,
                       uint32_t   delay,
                       float      loss_rate);

/*@
    requires link == \null || \valid(link);
    assigns \nothing;
*/
void       link_free(Link *link);

/*@
    behavior null:
        assumes link == \null;
        assigns \nothing;
    behavior valid:
        assumes \valid(link);
        assigns link->up;
        ensures link->up == up;
    complete behaviors;
    disjoint behaviors;
*/
void       link_set_up(Link *link, int up);

/*@
    behavior null:
        assumes link == \null;
        assigns \nothing;
        ensures \result == 0;
    behavior valid:
        assumes \valid(link);
        assigns \nothing;
        ensures \result == (link->up != 0 ? 1 : 0);
    complete behaviors;
    disjoint behaviors;
*/
int        link_is_up(const Link *link);

/*@
    behavior null:
        assumes link == \null || src == \null;
        assigns \nothing;
        ensures \result == \null;
    behavior mismatch:
        assumes \valid(link) && src != \null &&
                src != link->end_a && src != link->end_b;
        assigns \nothing;
        ensures \result == \null;
    behavior valid:
        assumes \valid(link) && src != \null &&
                (src == link->end_a || src == link->end_b);
        assigns \nothing;
        ensures \result == (src == link->end_a ? link->end_b : link->end_a);
    complete behaviors;
    disjoint behaviors;
*/
Interface *link_get_other_interface(const Link *link, const Interface *src);

/*@
    behavior null:
        assumes link == \null || pkt == \null || src == \null || sched == \null;
        assigns \nothing;
        ensures \result == -1;
    behavior valid:
        assumes \valid(link) && \valid(pkt) && \valid(src) && \valid(sched);
        assigns *sched;
        ensures \result == 1 || \result == -1;
    behavior scheduled:
        assumes \valid(link) && \valid(pkt) && \valid(src) && \valid(sched);
        assumes src == link->end_a;
        assumes link->up != 0;
        assumes link->end_a->up != 0;
        assumes link->end_b->up != 0;
        assigns *sched;
        ensures \result == 1 || \result == -1;
    complete behaviors;
*/
int        link_transmit(Link             *link,
                         const Packet     *pkt,
                         const Interface  *src,
                         struct Scheduler *sched,
                         uint64_t          now,
                         EventCallback     rx_handler,
                         void             *rx_ctx);


#endif // LINK_H
