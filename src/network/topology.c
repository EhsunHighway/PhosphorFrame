#include "topology.h"
#include <stdlib.h>
#include <string.h>

Topology *topology_create(void) {
    Topology *topo = malloc (sizeof(Topology));
    if (!topo) {
        return NULL;
    }
    topo->dev_count  = 0;
    topo->dev_cap    = 8; // initial capacity for devices array
    topo->link_count = 0;
    topo->link_cap   = 8; // initial capacity for links array
    topo->devices    = malloc(sizeof(Device *) * topo->dev_cap);
    topo->links      = malloc(sizeof(Link *) * topo->link_cap);
    if (!topo->devices || !topo->links) {
        free(topo->devices);
        free(topo->links);
        free(topo); 
        return NULL;
    }
    return topo;
}

void      topology_free(Topology *topo) {
    if (!topo) {
        return;
    }

    for (int i = 0; i < topo->dev_count; i++) {
        device_free(topo->devices[i]);
    }
    for (int i = 0; i < topo->link_count; i++) {
        link_free(topo->links[i]);
    }

    free(topo->devices);
    free(topo->links);
    free(topo);
}

int       topology_add_device(Topology *topo, Device *dev) {
    if (!topo || !dev) {
        return -1;
    }

    if (topo->dev_count == topo->dev_cap) {
        int      new_cap   = topo->dev_cap * 2;
        Device **new_array = realloc(topo->devices, sizeof(Device *) * new_cap);
        if (!new_array) {
            return -1;
        }
        topo->devices = new_array;
        topo->dev_cap = new_cap;
    }

    topo->devices[topo->dev_count++] = dev;
    return 0;
}

int       topology_add_link(Topology *topo, Link *link) {
    if (!topo || !link) {
        return -1;
    }
    
    if (topo->link_count == topo->link_cap) {
        int    new_cap   = topo->link_cap * 2;
        Link **new_array = realloc(topo->links, sizeof(Link *) * new_cap);
        if (!new_array) {
            return -1;
        }
        topo->links    = new_array;
        topo->link_cap = new_cap;
    }

    topo->links[topo->link_count++] = link;
    return 0;
}

Device   *topology_find_device_by_name(const Topology *topo, const char *name) {
    if (!topo || !name) {
        return NULL;
    }

    for (int i = 0;i < topo->dev_count;i++) {
        if (strcmp(topo->devices[i]->name, name)  == 0) {
            return topo->devices[i];
        }
    }
    return NULL;    
}

Device   *topology_find_device_by_ip(const Topology *topo, uint32_t ip_addr) {
    if (!topo) {
        return NULL;
    }

    for (int i = 0;i < topo->dev_count;i++) {
        for (int j = 0;j < topo->devices[i]->iface_count;j++) {
            if (topo->devices[i]->interfaces[j]->ip_addr == ip_addr) {
                return topo->devices[i];
            }
        }
    }

    return NULL;
}

Link     *topology_get_link_between(const Topology  *topo,
                                    const Interface *iface_a,
                                    const Interface *iface_b) {
    if (!topo || !iface_a || !iface_b) {
        return NULL;
    }

    for (int i = 0;i < topo->link_count;i++) {
        if ((topo->links[i]->end_a == iface_a && topo->links[i]->end_b == iface_b) ||
            (topo->links[i]->end_a == iface_b && topo->links[i]->end_b == iface_a)) {
            return topo->links[i];
        } 
    }

    return NULL;
}

int       topology_device_count(const Topology *topo) {
    if (!topo) {
        return 0;
    }
    
    return topo->dev_count;
}


int       topology_link_count(const Topology *topo) {
    if (!topo) {
        return 0;
    }

    return topo->link_count;
}
