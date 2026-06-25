#include "simulator.h"
#include <stdlib.h>


Simulator *simulator_create(Topology *topo, Scheduler *sched) {
    if (!topo || !sched) {
        return NULL;
    }

    Simulator *sim = malloc(sizeof(Simulator));
    if (!sim) {
        return NULL;
    }

    sim->topo     = topo;
    sim->sched    = sched;
    sim->end_time = 0;
    
    return sim;
}

void       simulator_free(Simulator *sim) {
    if (!sim) {
        return;
    }

    scheduler_free(sim->sched);
    topology_free(sim->topo);
    free(sim);
}

int        simulator_run(Simulator *sim) {
    sim->sched->running = 1;
    
    while (sim->sched->running == 1) {
        int step_state = scheduler_step(sim->sched);
        if (step_state == 0) {
            // No more events to process; stop the simulator
            sim->sched->running = 0;
        }
        if (sim->end_time > 0 && sim->sched->now >= sim->end_time) {
            sim->sched->running = 0;
        }
    }
    
    return 0;
}

int        simulator_step(Simulator *sim) {
    if (!sim) {
        return -1;
    }

    return scheduler_step(sim->sched);
}

void       simulator_stop(Simulator *sim) {
    if (!sim) {
        return;
    }

    scheduler_stop(sim->sched);
}


void       simulator_set_end_time(Simulator *sim, uint64_t end_us) {
    if (!sim) {
        return;
    }

    sim->end_time = end_us;
}


uint64_t   simulator_now(const Simulator *sim) {
    if (!sim) {
        return 0;
    }

    return sim->sched->now;
}


int        simulator_inject_packet(Simulator *sim,
                                   Device    *src,
                                   Device    *dst,
                                   Packet    *pkt,
                                   uint64_t   delay_us) {
    if (!sim || !src || !dst || !pkt) {
        return -1;
    }

    Event *e = event_create(EVT_PACKET_SEND,
                            sim->sched->now + delay_us,
                            src,
                            dst,
                            pkt,
                            NULL);
    if (!e) {
        return -1;
    }

    return scheduler_schedule(sim->sched, e);
}


void       simulator_register_handler(Simulator   *sim,
                                      EventType    type,
                                      EventHandler fn,
                                      void        *ctx) {
    if (!sim || !fn || type < 0 || type >= EVT_TYPE_COUNT) {
        return;
    }

    scheduler_register(sim->sched,
                       type,
                       fn,
                       ctx);
}
