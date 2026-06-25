#include "switch.h"
#include <string.h>
#include <stdlib.h>

static void mac_age_handler(const Event *e, void *ctx) {
    (void)e;
    Switch *sw = (Switch *)ctx;
    if (!sw || !sw->sim || !sw->sim->sched) {
        return;
    }        

    mac_table_age(&sw->mac_tbl, sw->sim->sched->now);

    Event *next = event_create_callback(EVT_MAC_AGE,
                                        sw->sim->sched->now + SW_AGE_INTERVAL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        mac_age_handler,
                                        sw);
    if (next) {
        if (scheduler_schedule(sw->sim->sched, next) != 0) {
            event_free(next);
        }
    }
}

static void switch_rx_shim(Interface *iface,
                           Packet    *pkt,
                           uint16_t   ethertype,
                           void      *ctx) {
    Switch *sw = (Switch *)ctx;
    if (!sw || !iface || !pkt) {
        return;
    }
    
    switch_receive(sw,
                   iface,
                   pkt,
                   ethertype);
}

Switch    *switch_create(const char *name, Simulator *sim) {
    if (!name || !sim) {
        return NULL;
    }

    Switch *sw = malloc(sizeof(Switch));
    if (!sw) {
        return NULL;
    }

    memset(sw, 0, sizeof(Switch));
    Device *dev = device_create(name, SW_MAX_PORTS);
    if (!dev) {
        free(sw);
        return NULL;
    }
    
    sw->base       = *dev;
    free(dev);
    sw->port_count = 0;
    sw->sim        = sim;
    mac_table_init(&sw->mac_tbl);
    Event *age_event = event_create_callback(EVT_MAC_AGE,
                                             sim->sched->now + SW_AGE_INTERVAL,
                                             NULL,
                                             NULL,
                                             NULL,
                                             NULL,
                                             mac_age_handler,
                                             sw);
    if (!age_event || scheduler_schedule(sim->sched, age_event) != 0) {
        event_free(age_event);
        device_free((Device *)sw);
        return NULL;
    }
    return sw;
}

void       switch_free(Switch *sw) {
    if (!sw) {
        return;
    }
    device_free((Device *)sw);
}

int        switch_add_port(Switch *sw, Interface *iface) {
    if (!sw || !iface || sw->port_count >= SW_MAX_PORTS) {
        return -1;
    }
    int res = device_add_interface((Device *)sw, iface);
    if (res == 0) {
        interface_set_rx_handler(iface, switch_rx_shim, sw);
        interface_set_up(iface, 1);
        sw->port_count = sw->base.iface_count;
    }
    
    return res;
}

void       switch_receive(Switch    *sw,
                          Interface *in_port,
                          Packet    *frame,
                          uint16_t   ethertype) {
    if (!sw || !in_port || !frame) {
        return;
    }

    if (!interface_is_up(in_port)) {
        packet_free(frame);
        return;
    }
    
    if (!(frame->data >= frame->head + ETH_HDR_LEN)) {
        packet_free(frame);
        in_port->rx_errors++;
        return;
    }

    EthernetHeader *eth_hdr = (EthernetHeader *)(frame->data - ETH_HDR_LEN);
    uint8_t src_mac[ETH_ALEN];
    memcpy(src_mac, eth_hdr->src_mac, ETH_ALEN);
    uint8_t dst_mac[ETH_ALEN];
    memcpy(dst_mac, eth_hdr->dst_mac, ETH_ALEN);
    
    MacEntry *entry = mac_table_learn(&sw->mac_tbl, src_mac, in_port, simulator_now(sw->sim));
    if (!entry) {
        // MAC table full, can't learn new entry
        in_port->rx_dropped++;
        packet_free(frame);
        return;
    }

    if (memcmp(dst_mac, ETH_BROADCAST, ETH_ALEN) == 0 ||
        mac_table_lookup(&sw->mac_tbl, dst_mac) == NULL) {
        /*
         * Broadcast or unknown destination MAC, flood the frame to all ports except the incoming port.
         */
        for (int i = 0; i < sw->port_count; i++) {
            Interface *out_port = sw->base.interfaces[i];
            if (out_port != in_port && interface_is_up(out_port) && out_port->link) {
                Packet *pkt_clone = packet_clone(frame);
                if (pkt_clone) {
                    ethernet_send(sw->sim,
                                  out_port,
                                  dst_mac,
                                  ethertype,
                                  pkt_clone);
                } else {
                    in_port->rx_errors++;
                    break; 
                }
            }
        }
        packet_free(frame);
    } else {
        /*
         * Unicast frame with known destination MAC, send it to the appropriate port.
         */
        Interface *egress = mac_table_lookup(&sw->mac_tbl, dst_mac);
        if (egress && egress != in_port && interface_is_up(egress)) {
            ethernet_send(sw->sim,
                          egress,
                          dst_mac,
                          ethertype,
                          frame);
        } else {
            in_port->rx_dropped++;
            packet_free(frame);
        }
    }
}

void       switch_port_down(Switch *sw, Interface *port) {
    if (!sw || !port) {
        return;
    }
    interface_set_up(port, 0);
    mac_table_flush_port(&sw->mac_tbl, port);
}

Interface *switch_get_port_by_name(Switch *sw, const char *name) {
    if (!sw || !name) {
        return NULL;
    }
    return device_get_interface_by_name((Device *)sw, name);
}
