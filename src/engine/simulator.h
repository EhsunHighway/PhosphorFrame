#ifndef SIMULATOR_H
#define SIMULATOR_H

#include <stdint.h>
#include <stddef.h>
#include "../network/topology.h"
#include "scheduler.h"

typedef struct Simulator {
    Topology  *topo;       // the network being simulated (owned)
    Scheduler *sched;      // event queue + dispatch loop  (owned)
    uint64_t   end_time;   // stop when sched->now >= end_time; 0 = no limit
} Simulator;

/*@
    behavior null:
        assumes topo == \null || sched == \null;
        assigns \nothing;
        ensures \result == \null;
    behavior ok:
        assumes topo != \null && sched != \null;
        allocates \result;
        ensures \result != \null ==>
            (\valid(\result) &&
             \result->topo == topo &&
             \result->sched == sched &&
             \result->end_time == 0);
    complete behaviors;
    disjoint behaviors;
*/
Simulator *simulator_create(Topology *topo, Scheduler *sched);

/*@
    requires sim == \null || \valid(sim);
    frees sim->topo, sim->sched, sim;
*/
void       simulator_free(Simulator *sim);

/*@
    requires sim != \null && \valid(sim);
    assigns  sim->sched->eq->events, sim->sched->eq->count, sim->sched->now, sim->sched->running;
    ensures  sim->sched->eq->count == 0 || 
             sim->sched->running == 0 ||
             (sim->end_time > 0 && sim->sched->now >= sim->end_time);
*/
int        simulator_run(Simulator *sim);

/*@
    behavior null:
        assumes sim == \null;
        assigns \nothing;
        ensures \result == -1;
    behavior valid:
        assumes \valid(sim);
        assigns  sim->sched->eq->events, sim->sched->eq->count, sim->sched->now;
        ensures  \result == 0 ==> sim->sched->eq->count == \old(sim->sched->eq->count);
        ensures  \result == 1 ==>
            (sim->sched->eq->count == \old(sim->sched->eq->count) - 1 &&
             sim->sched->now       >= \old(sim->sched->now));
    complete behaviors;
    disjoint behaviors;
*/
int        simulator_step(Simulator *sim);

/*@
    behavior null:
        assumes sim == \null;
        assigns \nothing;
    behavior valid:
        assumes \valid(sim);
        assigns  sim->sched->running;
        ensures  sim->sched->running == 0;
    complete behaviors;
    disjoint behaviors;
*/
void       simulator_stop(Simulator *sim);

/*@
    behavior null:
        assumes sim == \null;
        assigns \nothing;
    behavior valid:
        assumes \valid(sim);
        assigns  sim->end_time;
        ensures  sim->end_time == end_us;
    complete behaviors;
    disjoint behaviors;
*/
void       simulator_set_end_time(Simulator *sim, uint64_t end_us);

/*@
    behavior null:
        assumes sim == \null;
        assigns \nothing;
        ensures \result == 0;
    behavior valid:
        assumes \valid_read(sim);
        assigns \nothing;
        ensures \result == sim->sched->now;
    complete behaviors;
    disjoint behaviors;
*/
uint64_t   simulator_now(const Simulator *sim);

/*@
    behavior null:
        assumes sim == \null || src == \null || dst == \null || pkt == \null;
        assigns \nothing;
        ensures \result == -1;
    behavior valid:
        assumes \valid(sim) && \valid(src) && \valid(dst) && \valid(pkt);
        assigns  sim->sched->eq->events, sim->sched->eq->count;
        ensures  \result == 0 ==> sim->sched->eq->count == \old(sim->sched->eq->count) + 1;
        ensures  \result == -1 ==> sim->sched->eq->count == \old(sim->sched->eq->count);
    complete behaviors;
    disjoint behaviors;
*/
int        simulator_inject_packet(Simulator *sim,
                                   Device    *src,
                                   Device    *dst,
                                   Packet    *pkt,
                                   uint64_t   delay_us);

/*@
    behavior null:
        assumes sim == \null || type < 0 || type >= EVT_TYPE_COUNT || fn == \null;
        assigns \nothing;
    behavior valid:
        assumes \valid(sim) && type >= 0 && type < EVT_TYPE_COUNT && fn != \null;
        assigns sim->sched->handlers[type];
        ensures sim->sched->handlers[type].fn == fn;
        ensures sim->sched->handlers[type].ctx == ctx;
    complete behaviors;
    disjoint behaviors;
*/
void       simulator_register_handler(Simulator   *sim,
                                      EventType    type,
                                      EventHandler fn,
                                      void        *ctx);

#endif /* SIMULATOR_H */
