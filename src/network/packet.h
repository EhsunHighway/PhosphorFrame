#ifndef PACKET_H
#define PACKET_H

#include <stdint.h>
#include <stddef.h>

/*
 * PKT_HEADROOM: bytes reserved before data[] for protocol headers.
 * Ethernet(14) + IP(20) + TCP(20) = 54; 64 gives a safe margin.
 * packet_prepend() retreats data into this space — no memmove needed.
 * packet_strip()   advances data through this space — bytes remain accessible.
 *
 * Layout of the allocated buffer (head … head+PKT_HEADROOM+capacity):
 *   [  headroom (64 B)  |  payload / data area (capacity B)  ]
 *    ^                   ^
 *    head (never moves)  data (moves on strip/prepend)
 */
#define PKT_HEADROOM 64

typedef struct Packet {
    uint8_t *head;        // start of malloc'd buffer — NEVER moves
    uint8_t *data;        // start of current layer — retreats on prepend, advances on strip
    size_t   len;         // bytes of valid data starting at data
    size_t   capacity;    // usable payload bytes (excludes headroom)
    uint32_t id;          // unique packet ID
    int      layer;       // current OSI layer (1-4)
} Packet;


/*@
    requires capacity > 0;
    allocates \result;
    ensures \result != \null ==>
      (\result->len == 0 &&
       \result->capacity == capacity &&
       \result->layer == 0 &&
       \result->data == \result->head + PKT_HEADROOM);
    ensures \result == \null || \valid(\result->data + (0 .. capacity-1));
*/
Packet  *packet_create(size_t capacity);               

/*@ 
    requires \valid(p);
    requires header_len > 0;
    requires \valid_read((uint8_t *)header + (0 .. header_len-1));
    // succeeds only if there is sufficient headroom before data
    assigns p->data, p->len;
    behavior ok:
        assumes (size_t)(p->data - p->head) >= header_len;
        ensures \result == 0;
        ensures p->len == \old(p->len) + header_len;
        ensures p->data == \old(p->data) - header_len;
    behavior no_headroom:
        assumes (size_t)(p->data - p->head) < header_len;
        ensures \result == -1;
        ensures p->len == \old(p->len);
    complete behaviors ok, no_headroom;
    disjoint behaviors ok, no_headroom;
*/
int      packet_prepend(Packet     *p,
                        const void *header,
                        size_t      header_len);

/*@
  requires \valid(p);
  // advances data pointer; header bytes remain readable at old(data)
  assigns p->data, p->len;
  behavior valid_strip:
    assumes header_len <= p->len;
    ensures \result == 0;
    ensures p->len  == \old(p->len)  - header_len;
    ensures p->data == \old(p->data) + header_len;
  behavior overflow:
    assumes header_len > p->len;
    ensures \result == -1;
    ensures p->len  == \old(p->len);
    ensures p->data == \old(p->data);
  complete behaviors valid_strip, overflow;
  disjoint behaviors valid_strip, overflow;
*/
int      packet_strip(Packet *p, size_t header_len);          

/*@
  requires \valid_read(p);
  requires p->len > 0 ==> \valid_read(p->data + (0 .. p->len-1));
  allocates \result;
  ensures \result != \null ==>
    (\result->len == p->len &&
     \result->layer == p->layer &&
     \result->id != p->id);
*/
Packet  *packet_clone(const Packet *p);              

void     packet_free(Packet *p);                         

/*@
  requires len > 0;
  requires \valid((uint8_t *)data + (0 .. len-1));
  assigns \nothing;
*/
uint16_t packet_checksum(const void *data, size_t len); 

void     packet_dump(const Packet *p);


#endif // PACKET_H
