#ifndef TOPOLOGY_H
#define TOPOLOGY_H

#include <stdint.h>
#include <stddef.h>
#include "device.h"
#include "link.h"

typedef struct Topology {
    Device **devices;    // array of Device* pointers (heap, grows via realloc)
    int      dev_count;  // number of live devices
    int      dev_cap;    // current allocated capacity of devices array
    Link   **links;      // array of Link* pointers (heap, grows via realloc)
    int      link_count; // number of live links
    int      link_cap;   // current allocated capacity of links array
} Topology;

/*@
    allocates \result;
    ensures \result != \null ==>
        (\result->dev_count == 0 &&
         \result->dev_cap > 0 &&
         \result->link_count == 0 &&
         \result->link_cap > 0);
    ensures \result == \null || \valid(\result);
*/
Topology *topology_create(void);

/*@
    requires topo == \null || \valid(topo);
    frees topo->devices[0 .. topo->dev_count-1],
          topo->links[0 .. topo->link_count-1],
          topo->devices,
          topo->links,
          topo;
*/
void      topology_free(Topology *topo);

/*@
    behavior null:
        assumes topo == \null || dev == \null;
        assigns \nothing;
        ensures \result == -1;
    behavior valid:
        assumes \valid(topo) && \valid(dev);
        assigns topo->devices, topo->dev_count, topo->dev_cap;
        ensures \result == 0 || \result == -1;
        ensures \result == 0 ==> topo->dev_count == \old(topo->dev_count) + 1;
    complete behaviors;
    disjoint behaviors;
*/
int       topology_add_device(Topology *topo, Device *dev);

/*@
    behavior null:
        assumes topo == \null || link == \null;
        assigns \nothing;
        ensures \result == -1;
    behavior valid:
        assumes \valid(topo) && \valid(link);
        assigns topo->links, topo->link_count, topo->link_cap;
        ensures \result == 0 || \result == -1;
        ensures \result == 0 ==> topo->link_count == \old(topo->link_count) + 1;
    complete behaviors;
    disjoint behaviors;
*/
int       topology_add_link(Topology *topo, Link *link);

/*@
    behavior null:
        assumes topo == \null || name == \null;
        assigns \nothing;
        ensures \result == \null;
    behavior valid:
        assumes \valid(topo) && name != \null;
        assigns \nothing;
        ensures \result == \null || \valid(\result);
    complete behaviors;
    disjoint behaviors;
*/
Device   *topology_find_device_by_name(const Topology *topo, const char *name);

/*@
    behavior null:
        assumes topo == \null;
        assigns \nothing;
        ensures \result == \null;
    behavior valid:
        assumes \valid(topo);
        assigns \nothing;
        ensures \result == \null || \valid(\result);
    complete behaviors;
    disjoint behaviors;
*/
Device   *topology_find_device_by_ip(const Topology *topo, uint32_t ip_addr);

/*@
    behavior null:
        assumes topo == \null || iface_a == \null || iface_b == \null;
        assigns \nothing;
        ensures \result == \null;
    behavior valid:
        assumes \valid(topo) && \valid_read(iface_a) && \valid_read(iface_b);
        assigns \nothing;
        ensures \result == \null || \valid(\result);
    complete behaviors;
    disjoint behaviors;
*/
Link     *topology_get_link_between(const Topology  *topo,
                                    const Interface *iface_a,
                                    const Interface *iface_b);

/*@
    behavior null:
        assumes topo == \null;
        assigns \nothing;
        ensures \result == 0;
    behavior valid:
        assumes \valid(topo);
        assigns \nothing;
        ensures \result == topo->dev_count;
    complete behaviors;
    disjoint behaviors;
*/
int       topology_device_count(const Topology *topo);

/*@
    behavior null:
        assumes topo == \null;
        assigns \nothing;
        ensures \result == 0;
    behavior valid:
        assumes \valid(topo);
        assigns \nothing;
        ensures \result == topo->link_count;
    complete behaviors;
    disjoint behaviors;
*/
int       topology_link_count(const Topology *topo);


#endif /* TOPOLOGY_H */
