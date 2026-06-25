#ifndef SWITCH_H
#define SWITCH_H

#include <stdint.h>
#include "../protocols/ethernet.h"
#include "../network/device.h"
#include "mac_table.h"

#define SW_MAX_PORTS    48
#define SW_AGE_INTERVAL 30000 // ms

typedef struct Switch {
    /*
     * base MUST be first because 
     * switch_add_port(...) calls device_add_interface(...).
     * the cast is valid only if `base` is at offset 0.
     */
    Device     base;          // MUST be first
    MacTable   mac_tbl;
    int        port_count;
    Simulator *sim;
} Switch;

/*@
    behavior null:
        assumes name == \null || sim == \null;
        assigns \nothing;
        ensures \result == \null;
    behavior valid:
        assumes name != \null && \valid(sim);
        allocates \result;
        ensures \result == \null || (
            \valid(\result) &&
            (\forall integer i; 0 <= i < 48 ==> \result->base.interfaces[i] == \null) &&
            \result->mac_tbl.count == 0 &&
            \result->port_count == 0 &&
            \result->sim == sim);
    complete behaviors;
    disjoint behaviors;
*/
Switch    *switch_create(const char *name, Simulator *sim);

/*@
    behavior null:
        assumes sw == \null;
        assigns \nothing;
    behavior valid:
        assumes \valid(sw);
        frees sw->base.interfaces[0 .. sw->base.iface_count-1],
            sw->base.interfaces,
            sw;
*/
void       switch_free(Switch *sw);

/*@
    behavior null:
        assumes sw == \null || iface == \null ||
                (\valid(sw) && sw->port_count >= 48);
        assigns \nothing;
        ensures \result == -1;
    behavior valid:
        assumes \valid(sw) && \valid(iface) && sw->port_count < 48;
        assigns sw->base.interfaces[0 .. sw->base.iface_count],
                sw->base.iface_count,
                sw->port_count,
                iface->device,
                iface->rx_handler,
                iface->handler_ctx;
        ensures \result == 0 || \result == -1;
    complete behaviors;
    disjoint behaviors;
*/
int        switch_add_port(Switch *sw, Interface *iface);

/*@
    behavior null:
        assumes sw == \null || in_port == \null || frame == \null;
        assigns \nothing;
    behavior valid:
        assumes \valid(sw) && \valid(in_port) && \valid(frame);
        assigns sw->mac_tbl, in_port->rx_bytes, in_port->rx_dropped, in_port->rx_errors;
    complete behaviors;
    disjoint behaviors;
*/
void       switch_receive(Switch    *sw,
                          Interface *in_port,
                          Packet    *frame,
                          uint16_t   ethertype);

/*@
    behavior null:
        assumes sw == \null || port == \null;
        assigns \nothing;
    behavior valid:
        assumes \valid(sw) && \valid(port);
        assigns port->up, sw->mac_tbl;
    complete behaviors;
    disjoint behaviors;
*/
void       switch_port_down(Switch *sw, Interface *port);

/*@
    behavior null:
        assumes sw == \null || name == \null;
        assigns \nothing;
        ensures \result == \null;
    behavior valid:
        assumes \valid(sw) && name != \null;
        assigns \nothing;
        ensures \result == \null || \valid(\result);
    complete behaviors;
    disjoint behaviors;
*/
Interface *switch_get_port_by_name(Switch *sw, const char *name);


#endif /* SWITCH_H */
