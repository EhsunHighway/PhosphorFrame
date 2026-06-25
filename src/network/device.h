#ifndef DEVICE_H
#define DEVICE_H

#include <stdint.h>
#include <stddef.h>
#include "interface.h"
#include "packet.h"

typedef struct Device {
    char        name[32];       // device name (e.g., "Router-A")
    Interface **interfaces;     // array of pointers to interfaces
    int         iface_count;    // number of interfaces
    int         iface_max;      // allocated capacity of interfaces array
} Device;

/*@
    behavior null_or_bad:
        assumes name == \null || iface_max <= 0;
        assigns \nothing;
        ensures \result == \null;
    behavior ok:
        assumes name != \null && iface_max > 0;
        allocates \result;
        ensures \result != \null ==>
            (\valid(\result) &&
             \result->iface_count == 0 &&
             \result->iface_max == iface_max);
        ensures \result == \null || \valid(\result);
    complete behaviors;
    disjoint behaviors;
*/
Device     *device_create(const char *name, int iface_max);

/*@
    requires dev == \null || \valid(dev);
    frees dev->interfaces[0 .. dev->iface_count-1],
          dev->interfaces,
          dev;
*/
void        device_free(Device *dev);

/*@
    behavior null:
        assumes dev == \null || iface == \null;
        assigns \nothing;
        ensures \result == -1;
    behavior valid:
        assumes \valid(dev) && \valid(iface);
        assigns dev->interfaces[0 .. dev->iface_count], dev->iface_count, iface->device;
        ensures \result == 0 || \result == -1;
        ensures \result == 0 ==> dev->iface_count == \old(dev->iface_count) + 1;
        ensures \result == 0 ==> dev->interfaces[\old(dev->iface_count)] == iface;
        ensures \result == 0 ==> iface->device == dev;
        ensures \result == -1 ==> dev->iface_count == \old(dev->iface_count);
    // Ownership: on success (\result == 0), iface is owned by dev.
    // The caller must NOT free iface independently; device_free will free it.
*/
int         device_add_interface(Device *dev, Interface *iface);

/*@
    behavior null:
        assumes dev == \null || iface_name == \null;
        assigns \nothing;
        ensures \result == \null;
    behavior valid:
        assumes \valid(dev) && iface_name != \null;
        assigns \nothing;
        ensures \result == \null || \valid(\result);
    complete behaviors;
    disjoint behaviors;
*/
Interface *device_get_interface_by_name(const Device *dev, const char *iface_name);

/*@
    behavior null:
        assumes dev == \null;
        assigns \nothing;
        ensures \result == \null;
    behavior valid:
        assumes \valid(dev);
        assigns \nothing;
        ensures \result == \null || \valid(\result);
    complete behaviors;
    disjoint behaviors;
*/
Interface *device_get_interface_by_ip(const Device *dev, uint32_t ip_addr);

/*@
    behavior null:
        assumes dev == \null || in_iface == \null || pkt == \null;
        assigns \nothing;
        ensures \result == -1;
    behavior valid:
        assumes \valid(dev) && \valid(in_iface) && \valid(pkt);
        assigns \nothing;
        ensures \result == 0 || \result == -1;
    complete behaviors;
    disjoint behaviors;
*/
int        device_receive_packet(Device    *dev,
                                 Interface *in_iface,
                                 Packet    *pkt);

/*@
    behavior null:
        assumes dev == \null || out_iface == \null || pkt == \null;
        assigns \nothing;
        ensures \result == -1;
    behavior valid:
        assumes \valid(dev) && \valid(out_iface) && \valid(pkt);
        assigns \nothing;
        ensures \result == 0 || \result == -1;
    complete behaviors;
    disjoint behaviors;
*/
int        device_send_packet(Device    *dev,
                              Interface *out_iface,
                              Packet    *pkt);


#endif // DEVICE_H
