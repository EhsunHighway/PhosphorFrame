#include <string.h>
#include "mac_table.h"
#include "../engine/simulator.h"

void        mac_table_init(MacTable *table) {
    if (!table) {
        return;
    }
    memset(table, 0, sizeof(MacTable));
    table->count = 0;
}

MacEntry   *mac_table_learn(MacTable *table, 
                            const uint8_t mac[6], 
                            Interface *port, 
                            uint64_t now) {
    if (!table || !mac ||!port) {
        return NULL;
    }

    for (int i = 0; i < MAC_TABLE_SIZE; i++) {
        if (table->entries[i].valid && memcmp(table->entries[i].mac, mac, 6) == 0) {
            table->entries[i].port      = port;
            table->entries[i].timestamp = now;
            return &table->entries[i];
        }
    }

    for (int i = 0; i < MAC_TABLE_SIZE; i++) {
        if (!table->entries[i].valid) {
            memcpy(table->entries[i].mac, mac, 6);
            table->entries[i].port      = port;
            table->entries[i].timestamp = now;
            table->entries[i].valid     = 1;
            table->count++;
            return &table->entries[i];
        }
    }

    return NULL;
}

Interface  *mac_table_lookup(MacTable *table, const uint8_t mac[6]) {
    if (!table || !mac) {
        return NULL;
    }

    for (int i = 0;i < MAC_TABLE_SIZE;i++) {
        if (table->entries[i].valid && memcmp(table->entries[i].mac, mac, 6) == 0) {
            return table->entries[i].port;
        }
    }

    return NULL;
}

void        mac_table_age(MacTable *table, uint64_t now) {
    if (!table) {
        return;
    }

    for (int i = 0;i < MAC_TABLE_SIZE;i++) {
        if (table->entries[i].valid && now - table->entries[i].timestamp > MAC_AGE_MS) {
            table->entries[i].valid = 0;
            table->count--;
        }
    }
}

void        mac_table_flush_port(MacTable *table, Interface *port) {
    if (!table || !port) {
        return;
    }

    for (int i = 0;i < MAC_TABLE_SIZE;i++) {
        if (table->entries[i].valid && table->entries[i].port == port) {
            table->entries[i].valid = 0;
            table->count--;        
        }
    }
    return;
}