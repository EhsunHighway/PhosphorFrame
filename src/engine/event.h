#ifndef EVENT_H
#define EVENT_H

#include <stdint.h>
#include <stddef.h>

typedef struct Event Event;
typedef void (*EventCallback)(const Event *e, void *ctx);

typedef enum {
   EVT_PACKET_SEND,
    EVT_PACKET_RECEIVE,   // ethernet listens on this
    EVT_ARP_REQUEST,      // arp listens on this
    EVT_ARP_REPLY,        // arp listens on this
    EVT_ROUTING_UPDATE,
    EVT_ROUTE_ADDED,
    EVT_LINK_UP,
    EVT_LINK_DOWN,
    EVT_TIMER_EXPIRED,    // periodic protocols (RIP, OSPF hello)
    EVT_RENDER,
    EVT_RESET,
    // L2 / MAC
    EVT_MAC_AGE,
    // Transport
    EVT_TCP_RETRANSMIT,
    // RIP
    EVT_RIP_UPDATE,
    EVT_RIP_TIMEOUT,
    EVT_RIP_GC,
    // OSPF
    EVT_OSPF_HELLO,
    EVT_OSPF_DEAD,
    EVT_OSPF_SPF,
    // BGP
    EVT_BGP_KEEPALIVE,
    EVT_BGP_HOLD,
    EVT_BGP_CONNECT_RETRY,
    // EIGRP
    EVT_EIGRP_HELLO,
    EVT_EIGRP_HOLD,
    // IS-IS
    EVT_ISIS_HELLO,
    EVT_ISIS_HOLD,
    EVT_ISIS_SPF,
    EVT_ISIS_LSP_REGEN,
    // NAT
    EVT_NAT_GC,
    // sentinel — handler table size **must be last**
    EVT_TYPE_COUNT        
} EventType;

struct Event {
    EventType     type;
    uint64_t      timestamp;     // simulated microseconds
    void         *src_device;    // pointer to source device 
    void         *dst_device;    // pointer to destination device
    void         *packet;        // Packet* — void* to avoid circular include
    void         *data;          // protocol-specific extra payload
    EventCallback handler;       // optional per-event callback
    void         *handler_ctx;   // context passed to handler
};

typedef struct EventQueue {
    Event   **events;           // array of Event pointers
    size_t    count;            // current number of events
    size_t    capacity;         // allocated capacity
} EventQueue;


/*@
    requires capacity > 0;
    allocates \result;
    ensures \result != \null ==>
        (\result->count == 0 &&
         \result->capacity == capacity &&
         \valid(\result->events + (0 .. capacity-1)));
    ensures \result == \null || \valid(\result->events + (0 .. capacity-1));
*/
EventQueue *event_queue_create(size_t capacity);

void        event_queue_free(EventQueue *eq);

/*@ 
    requires \valid(eq);
    requires \valid(e);
    assigns eq->events, eq->count;
    ensures \result == 0 ==> eq->count == \old(eq->count) + 1;
    ensures \result == -1 ==> eq->count == \old(eq->count);
*/
int         event_queue_push(EventQueue *eq, Event *e);

/*@    
    requires \valid(eq);
    assigns eq->events, eq->count;
    ensures \result != \null ==> eq->count == \old(eq->count) - 1;
    ensures \result == \null ==> eq->count == \old(eq->count);
*/
Event      *event_queue_pop(EventQueue *eq);

/*@
    requires \valid_read(eq);
    assigns \nothing;
    ensures \result == \null || \valid_read(\result);
*/
Event      *event_queue_peek(const EventQueue *eq);

/*@
    requires \valid_read(eq);
    assigns \nothing;
    ensures \result == 1 <==> eq->count == 0;
*/
int         event_queue_is_empty(const EventQueue *eq);

/*@
    allocates \result;
    ensures \result != \null ==> \valid(\result);
    ensures \result != \null ==> \result->handler == \null;
    ensures \result != \null ==> \result->handler_ctx == \null;
*/
Event      *event_create(EventType type, 
                         uint64_t timestamp, 
                         void *src, 
                         void *dst, 
                         void *packet, 
                         void *data);

/*@
    allocates \result;
    ensures \result != \null ==> \valid(\result);
    ensures \result != \null ==> \result->handler == handler;
    ensures \result != \null ==> \result->handler_ctx == handler_ctx;
*/
Event      *event_create_callback(EventType type, 
                                  uint64_t timestamp,
                                  void *src, 
                                  void *dst, 
                                  void *packet, 
                                  void *data,
                                  EventCallback handler, 
                                  void *handler_ctx);

void        event_free(Event *e);

#endif // EVENT_H
