#include <stdlib.h>
#include <string.h>
#include "device.h"


Device     *device_create(const char *name, int iface_max) {
    if (!name || iface_max <= 0) {
        return NULL;
    }

    Device *dev = malloc(sizeof(Device));
    if (!dev) {
        return NULL;
    }

    strncpy(dev->name, name, sizeof(dev->name) - 1);
    dev->name[sizeof(dev->name) - 1] = '\0'; // ensure null-termination
    dev->interfaces                  = malloc(sizeof(Interface *) * iface_max);
    if (!dev->interfaces) {
        free(dev);
        return NULL;
    }
    dev->iface_count = 0;
    dev->iface_max   = iface_max;
    return dev;
}

void        device_free(Device *dev) {
    if (!dev) {
        return;
    }

    /*
     * Device owns its interfaces — they are freed here.
     * Links borrow Interface pointers; always free links before the device.
     */
    for (int i = 0; i < dev->iface_count; i++) {
        interface_free(dev->interfaces[i]);
    }

    free(dev->interfaces);
    free(dev);
}

int         device_add_interface(Device *dev, Interface *iface) {
    if (!dev || !iface || dev->iface_count >= dev->iface_max) {
        return -1;
    }

    for (int i = 0; i < dev->iface_count; i++) {
        if (strcmp(dev->interfaces[i]->name, iface->name) == 0) {
            return -1; // duplicate interface name not allowed
        }
    }

    iface->device                       = dev;
    dev->interfaces[dev->iface_count++] = iface;
    return 0;
}

Interface *device_get_interface_by_name(const Device *dev, const char *iface_name) {
    if (!dev || !iface_name) {
        return NULL;
    }

    for (int i = 0; i < dev->iface_count; i++) {
        if (strcmp(dev->interfaces[i]->name, iface_name) == 0) {
            return dev->interfaces[i];
        }
    }
    return NULL;
}

Interface *device_get_interface_by_ip(const Device *dev, uint32_t ip_addr) {
    if (!dev) {
        return NULL;
    }

    for (int i = 0; i < dev->iface_count; i++) {
        if (dev->interfaces[i]->ip_addr == ip_addr) {
            return dev->interfaces[i];
        }
    }
    return NULL;
}

int        device_receive_packet(Device    *dev,
                                 Interface *in_iface,
                                 Packet    *pkt) {
    if (!dev || !in_iface || !pkt) return -1;
    return 0;  // stub: real logic added when protocols exist
}

int        device_send_packet(Device    *dev,
                              Interface *out_iface,
                              Packet    *pkt) {
    if (!dev || !out_iface || !pkt) return -1;
    return 0;  // stub: real logic added when protocols exist
}
