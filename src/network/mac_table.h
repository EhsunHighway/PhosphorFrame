#ifndef MAC_TABLE_H
#define MAC_TABLE_H

#include <stdint.h>
#include "interface.h"

#define MAC_TABLE_SIZE 1024
#define MAC_AGE_MS     300000           // 5 minutes; the standard Cisco CAM aging time
#define MAC_ADDR_LEN   6

typedef struct MacEntry {
    uint8_t    mac[MAC_ADDR_LEN];       // learned source MAC 
    uint8_t    _pad[2];                 // 2 bytes - align next field to 8
    Interface *port;                    // egress NIC (borrowed, not owned)
    uint64_t   timestamp;               // last time this MAC was seen/learned (ms)
    int        valid;         
    int        _pad2;                   // 4 bytes - padding to make struct size a multiple of 8 bytes
} MacEntry;                             // total: 32 bytes

typedef struct MacTable {
    MacEntry entries[MAC_TABLE_SIZE];   
    int      count;                     // number of valid entries   
    int      _pad;
} MacTable;


/*@
    behavior null:
        assumes table == \null;
        assigns \nothing;
    behavior valid:
        assumes \valid(table);
        assigns table->entries[0 .. 1023], table->count;
        ensures table->count == 0;
        ensures \forall integer i; 0 <= i < 1024 ==> table->entries[i].valid == 0;
    complete behaviors;
    disjoint behaviors;
*/
void        mac_table_init(MacTable *table);

/*@ 
    behavior null:
        assumes table == \null || mac == \null || port == \null;
        assigns \nothing;
        ensures \result == NULL;
    behavior valid:
        assumes \valid(table) && \valid_read(mac+(0..5)) && \valid(port);
        assigns table->entries[0 .. 1023], table->count;
        ensures \result == NULL || \valid(\result);
        ensures \result != NULL ==>
            (\result->port == port &&
             (\forall integer i; 0 <= i < 6 ==> \result->mac[i] == mac[i]) &&
             \result->timestamp == now &&
             \result->valid == 1);
    complete behaviors; 
    disjoint behaviors;
*/
MacEntry   *mac_table_learn(MacTable *table, 
                            const uint8_t mac[6], 
                            Interface *port, 
                            uint64_t now);

/*@ 
    behavior null:
        assumes table == \null || mac == \null;
        assigns \nothing;
        ensures \result == NULL;
    behavior valid:
        assumes \valid(table) && \valid_read(mac+(0..5));
        assigns \nothing;
        ensures \result == \null ||
            (\exists integer i; 0 <= i < 1024 &&
             table->entries[i].valid == 1 &&
             table->entries[i].port == \result &&
             (\forall integer j; 0 <= j < 6 ==> table->entries[i].mac[j] == mac[j]));
        ensures \result == \null ==>
            (\forall integer i; 0 <= i < 1024 ==>
             table->entries[i].valid == 0 ||
             (\exists integer j; 0 <= j < 6 && table->entries[i].mac[j] != mac[j]));
    complete behaviors; 
    disjoint behaviors;
*/
Interface  *mac_table_lookup(MacTable *table, const uint8_t mac[6]);
    
/*@
    behavior null:
        assumes table == \null;
        assigns \nothing;
    behavior valid:
        assumes \valid(table);
        assigns table->entries[0 .. 1023], table->count;
        ensures \forall integer i; 0 <= i < 1024 ==>
            (table->entries[i].valid == 0 ||
             now - table->entries[i].timestamp <= 300000);
    complete behaviors;
    disjoint behaviors;
*/
void        mac_table_age(MacTable *table, uint64_t now);

/*@
    behavior null:
        assumes table == \null || port == \null;
        assigns \nothing;
    behavior valid:
        assumes \valid(table) && \valid(port);
        assigns table->entries[0 .. 1023], table->count;
        ensures \valid(table);
        ensures (\forall integer i; 0 <= i < 1024 ==> 
                (table->entries[i].port == port ==> 
                table->entries[i].valid == 0));
        ensures \forall integer i; 0 <= i < 1024 ==>
                (table->entries[i].port != port ==>
                table->entries[i].valid == \old(table->entries[i].valid));
    complete behaviors;
    disjoint behaviors;
*/
void        mac_table_flush_port(MacTable *table, Interface *port);

#endif /* MAC_TABLE_H */
