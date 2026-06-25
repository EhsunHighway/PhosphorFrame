#include "tcp.h"
#include <string.h>


static Tcb *tcp_alloc_tcb(TcpTable *table) {
    if (!table) {
        return NULL;
    }

    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        if (!table->tcbs[i].valid) {
            memset(&table->tcbs[i], 0, sizeof(table->tcbs[i]));
            table->tcbs[i].valid = 1;
            table->count++;
            return &table->tcbs[i];
        }
    }
    return NULL;
}

static void tcp_release_tcb(TcpTable *table, Tcb *tcb) {
    if (!table || !tcb) {
        return;
    }

    for (int i = 0; i < TCP_MAX_INFLIGHT; i++) {
        if (tcb->sendq.entries[i].pkt) {
            packet_free(tcb->sendq.entries[i].pkt);
        }
    }

    memset(tcb, 0, sizeof(*tcb));
    if (table->count > 0) {
        table->count--;
    }
}

static int tcp_sendq_has_unacked(const Tcb *tcb) {
    if (!tcb) {
        return 0;
    }

    for (int i = 0; i < TCP_MAX_INFLIGHT; i++) {
        if (tcb->sendq.entries[i].pkt && tcb->sendq.entries[i].acked == 0) {
            return 1;
        }
    }
    return 0;
}

/*@
    behavior null_input:
        assumes tcb == \null || clone == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior full:
        assumes \valid(tcb) && \valid(clone);
        assumes tcb->sendq.count >= 1;
        assigns \nothing;
        ensures \result == -1;

    behavior first_free:
        assumes \valid(tcb) && \valid(clone);
        assumes tcb->sendq.count < 1;
        assumes tcb->sendq.entries[0].pkt == \null;
        assigns tcb->sendq;
        ensures \result == 0;
        ensures tcb->sendq.count == \old(tcb->sendq.count) + 1;
        ensures tcb->sendq.entries[0].pkt == clone;
        ensures tcb->sendq.entries[0].seq_start == seq_start;
        ensures tcb->sendq.entries[0].seq_end == seq_end;
        ensures tcb->sendq.entries[0].flags == flags;
        ensures tcb->sendq.entries[0].sent_ts == now;
        ensures tcb->sendq.entries[0].retransmits == 0;
        ensures tcb->sendq.entries[0].acked == 0;

    complete behaviors;
*/
static int tcp_sendq_track(Tcb     *tcb,
                           Packet  *clone,
                           uint32_t seq_start,
                           uint32_t seq_end,
                           uint8_t  flags,
                           uint64_t now) {
    if (!tcb || !clone) {
        return -1;
    }

    if (tcb->sendq.count >= TCP_MAX_INFLIGHT) {
        return -1;
    }
    for (int i = 0; i < TCP_MAX_INFLIGHT; i++) {
        TcpSegment *seg  = &tcb->sendq.entries[i];

        if (seg->pkt == NULL) {
            seg->pkt         =  clone;
            seg->seq_start   =  seq_start;
            seg->seq_end     =  seq_end;
            seg->flags       =  flags;
            seg->sent_ts     =  now;
            seg->retransmits =  0;
            seg->acked       =  0;
            tcb->sendq.count++;
            return 0;
        }
    }

    return -1;
}

static void tcp_sendq_ack(Tcb *tcb, uint32_t ack) {
    if (!tcb) {
        return;
    }

    for (int i = 0; i < TCP_MAX_INFLIGHT; i++) {
        TcpSegment *seg = &tcb->sendq.entries[i];
        if (seg->pkt && ack >= seg->seq_end) {
            seg->acked = 1;
            packet_free(seg->pkt);
            seg->pkt = NULL;
            tcb->sendq.count--;
        }
    }   
}

static Tcb *tcp_find_exact(TcpTable *table,
                           uint32_t  local_ip,
                           uint16_t  local_port,
                           uint32_t  remote_ip,
                           uint16_t  remote_port) {
    if (!table) {
        return NULL;
    }

    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        Tcb *tcb = &table->tcbs[i];
        if (tcb->valid                     &&
            tcb->state       != TCP_LISTEN &&
            tcb->local_ip    == local_ip   &&
            tcb->local_port  == local_port &&
            tcb->remote_ip   == remote_ip  &&
            tcb->remote_port == remote_port) {
            return tcb;
        }
    }
    return NULL;
}

static Tcb *tcp_find_listener(TcpTable *table,
                              uint32_t  local_ip,
                              uint16_t  local_port) {
    if (!table) {
        return NULL;
    }

    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        Tcb *tcb = &table->tcbs[i];
        if (tcb->valid                     &&
            tcb->state       == TCP_LISTEN &&
           (tcb->local_ip    == local_ip   ||
            tcb->local_ip    == 0)         &&
            tcb->local_port  == local_port) {
            return tcb;
        }
    }
    return NULL;    
}

static int tcp_send_segment(Simulator     *sim,
                            uint32_t       src_ip,
                            uint32_t       dst_ip,
                            uint16_t       src_port,
                            uint16_t       dst_port,
                            uint32_t       seq,
                            uint32_t       ack,
                            uint8_t        flags,
                            const uint8_t *payload,
                            size_t         payload_len,
                            Packet       **sent_clone) {
    if (!sim) {
        return -1;
    }

    if (payload_len > TCP_MSS) {
        return -1;
    }

    if (payload_len > 0 && !payload) {
        return -1;
    }

    if (sent_clone) {
        *sent_clone = NULL;
    }

    Packet *pkt = packet_create(TCP_HDR_LEN + payload_len);
    if (!pkt) {
        return -1;
    }

    TcpHeader *tcp_hdr      = (TcpHeader *)pkt->data;
    tcp_hdr->src_port       = ns_htons(src_port);
    tcp_hdr->dst_port       = ns_htons(dst_port);
    tcp_hdr->seq_num        = ns_htonl(seq);
    tcp_hdr->ack_num        = ns_htonl(ack);
    tcp_hdr->data_off_flags = ns_htons((TCP_HDR_WORDS << 12) | flags);
    tcp_hdr->window         = ns_htons(TCP_DEFAULT_WINDOW);
    tcp_hdr->checksum       = 0;
    tcp_hdr->urgent_ptr     = 0;

    if (payload_len > 0) {
        memcpy(pkt->data + TCP_HDR_LEN, payload, payload_len);
    }

    pkt->len   = TCP_HDR_LEN + payload_len;
    pkt->layer = 4;

    Packet *track_clone = NULL;
    if (sent_clone) {
        track_clone = packet_clone(pkt);
        if (!track_clone) {
            packet_free(pkt);
            return -1;
        }
    }

    int res = ip_output(sim, src_ip, dst_ip, IPPROTO_TCP, pkt);
    if (res == -1) {
        packet_free(pkt);
        packet_free(track_clone);
        return -1;
    }

    if (sent_clone) {
        *sent_clone = track_clone;
    }
    return 0;
}

/*@
    behavior invalid:
        assumes sim == \null || tcb == \null ||
                (sim != \null && sim->sched == \null);
        assigns \nothing;

    behavior valid:
        assumes \valid(sim) && sim->sched != \null && \valid(sim->sched);
        assumes \valid(tcb);
        assigns sim->sched->eq->events, sim->sched->eq->count,
                tcb->retransmit_ts;
        ensures tcb->retransmit_ts == \old(tcb->retransmit_ts) ||
                tcb->retransmit_ts == \old(sim->sched->now) + 1000000ULL;

    complete behaviors;
*/
static void tcp_schedule_retransmit(Simulator *sim,
                                    TcpContext *tcp_ctx,
                                    Tcb *tcb) {
    if (!sim || !sim->sched || !tcb) {
        return;
    }

    uint64_t now = scheduler_now(sim->sched) + TCP_RETRANSMIT_US;

    if (tcp_ctx) {
        Event *e = event_create_callback(EVT_TCP_RETRANSMIT, 
                                         now, 
                                         NULL, 
                                         NULL, 
                                         NULL, 
                                         tcb, 
                                         tcp_retransmit_handler, 
                                         tcp_ctx);
        if (!e) {
            return;
        }
        int res = scheduler_schedule(sim->sched, e);
        if (res == -1) {
            event_free(e);
            return;
        }
    } else {
        Event *e = event_create(EVT_TCP_RETRANSMIT, now, NULL, NULL, NULL, tcb);
        if (!e) {
            return;
        }
        int res = scheduler_schedule(sim->sched, e);
        if (res == -1) {
            event_free(e);
            return;
        }
    }
    tcb->retransmit_ts = now;
}

/*@
    behavior null_input:
        assumes e == \null || ctx == \null;
        assigns \nothing;

    behavior no_tcb:
        assumes e != \null && ctx != \null;
        assumes ((Event *)e)->data == \null;
        assigns \nothing;

    behavior not_releasable:
        assumes e != \null && ctx != \null;
        assumes ((Event *)e)->data != \null;
        assumes \valid((Tcb *)((Event *)e)->data);
        assumes ((Tcb *)((Event *)e)->data)->valid != 1 ||
                ((Tcb *)((Event *)e)->data)->state != TCP_TIME_WAIT;
        assigns \nothing;

    behavior release_time_wait:
        assumes e != \null && ctx != \null;
        assumes \valid((TcpTable *)ctx);
        assumes ((Event *)e)->data != \null;
        assumes \valid((Tcb *)((Event *)e)->data);
        assumes ((Tcb *)((Event *)e)->data)->valid == 1;
        assumes ((Tcb *)((Event *)e)->data)->state == TCP_TIME_WAIT;
        assigns ((TcpTable *)ctx)->count, *((Tcb *)((Event *)e)->data);
        ensures ((Tcb *)((Event *)e)->data)->valid == 0;

    complete behaviors;
*/
static void tcp_time_wait_handler(const Event *e, void *ctx) {
    if (!e || !ctx) {
        return;
    }

    TcpTable *table = (TcpTable *)ctx;
    Tcb      *tcb   = (Tcb *)e->data;

    if (!tcb || !table || tcb->valid != 1) {
        return;
    }

    if (tcb->state != TCP_TIME_WAIT) {
        return;
    }
    tcp_release_tcb(table, tcb);
}

void  tcp_init(TcpTable *table) {
    if (!table) {
        return;
    }

    memset(table, 0, sizeof(*table));
}


Tcb  *tcp_listen(TcpTable         *table,
                 uint32_t          local_ip,
                 uint16_t          local_port,
                 TcpRecvHandler    recv_fn,
                 TcpConnectHandler connect_fn,
                 void             *ctx) {
    if (!table) {
        return NULL;
    }

    if (local_port == 0) {
        return NULL;
    }

    if (tcp_find_listener(table, local_ip, local_port)) {
        return NULL;
    }

    Tcb *tcb = tcp_alloc_tcb(table);
    if (!tcb) {
        return NULL;
    }

    tcb->local_ip        = local_ip;
    tcb->local_port      = local_port;
    tcb->remote_ip       = 0;
    tcb->remote_port     = 0;
    tcb->state           = TCP_LISTEN;
    tcb->rcv_wnd         = TCP_DEFAULT_WINDOW;
    tcb->recv_handler    = recv_fn;
    tcb->connect_handler = connect_fn;
    tcb->handler_ctx     = ctx;

    return tcb;
}


Tcb  *tcp_connect(Simulator        *sim,
                  TcpTable         *table,
                  uint32_t          local_ip,
                  uint32_t          remote_ip,
                  uint16_t          local_port,
                  uint16_t          remote_port,
                  TcpRecvHandler    recv_fn,
                  TcpConnectHandler connect_fn,
                  void             *ctx) {
    if (!sim || !table) {
        return NULL;
    }

    if (local_ip == 0 || remote_ip == 0 || local_port == 0 || remote_port == 0) {
        return NULL;
    }

    if (tcp_find_exact(table, local_ip, local_port, remote_ip, remote_port)) {
        return NULL;
    }

    Tcb *tcb = tcp_alloc_tcb(table);
    if (!tcb) {
        return NULL;
    }

    tcb->local_ip        = local_ip;
    tcb->local_port      = local_port;
    tcb->remote_ip       = remote_ip;
    tcb->remote_port     = remote_port;
    tcb->state           = TCP_SYN_SENT;
    tcb->snd_una         = 0;
    tcb->snd_nxt         = 1;
    tcb->rcv_nxt         = 0;
    tcb->snd_wnd         = TCP_DEFAULT_WINDOW;
    tcb->rcv_wnd         = TCP_DEFAULT_WINDOW;
    tcb->recv_handler    = recv_fn;
    tcb->connect_handler = connect_fn;
    tcb->handler_ctx     = ctx;

    Packet *syn_clone = NULL;
    int res = tcp_send_segment(sim, local_ip, 
                               remote_ip, 
                               local_port, 
                               remote_port, 
                               0, 
                               0, 
                               TCP_FLAG_SYN, 
                               NULL, 
                               0, 
                               &syn_clone);
    if (res == -1) {
        tcp_release_tcb(table, tcb);
        return NULL;
    }

    uint64_t now = sim->sched ? scheduler_now(sim->sched) : 0;
    if (tcp_sendq_track(tcb, syn_clone, 0, 1, TCP_FLAG_SYN, now) == -1) {
        packet_free(syn_clone);
        tcp_release_tcb(table, tcb);
        return NULL;
    }

    if (sim->sched) {
        tcp_schedule_retransmit(sim, NULL, tcb);
    }
    return tcb;
}

int  tcp_receive(Interface *iface,
                 Packet    *pkt,
                 void      *ctx) {
    if (!iface) {
        return -1;
    }

    if (!pkt) {
        iface->rx_errors++;
        return -1;
    }

    if (!ctx) {
        packet_free(pkt);
        iface->rx_errors++;
        return -1;
    }

    TcpContext *tcp_ctx = (TcpContext *)ctx;
    if (!tcp_ctx->sim || !tcp_ctx->table) {
        packet_free(pkt);
        iface->rx_errors++;
        return -1;
    }

    if (pkt->len < TCP_HDR_LEN) {
        packet_free(pkt);
        iface->rx_errors++;
        return -1;
    }

    if (!pkt->head || !pkt->data) {
        packet_free(pkt);
        iface->rx_errors++;
        return -1;
    }

    if (pkt->data < pkt->head + IP_HDR_LEN) {
        packet_free(pkt);
        iface->rx_errors++;
        return -1;
    }

    if (pkt->data + pkt->len > pkt->head + PKT_HEADROOM + pkt->capacity) {
        packet_free(pkt);
        iface->rx_errors++;
        return -1;
    }

    IpHeader *ip_hdr   = (IpHeader *)(pkt->data - IP_HDR_LEN);
    uint32_t  src_addr = ns_ntohl(ip_hdr->src_ip);
    uint32_t  dst_addr = ns_ntohl(ip_hdr->dst_ip);
    uint8_t   protocol = ip_hdr->protocol;

    if (protocol != IPPROTO_TCP) {
        packet_free(pkt);
        iface->rx_errors++;
        return -1;
    }

    TcpHeader *tcp_hdr = (TcpHeader *)pkt->data;
    uint32_t src_ip    = src_addr;
    uint16_t src_port  = ns_ntohs(tcp_hdr->src_port);
    uint32_t dst_ip    = dst_addr;
    uint16_t dst_port  = ns_ntohs(tcp_hdr->dst_port);
    uint32_t seq_num   = ns_ntohl(tcp_hdr->seq_num);
    uint32_t ack_num   = ns_ntohl(tcp_hdr->ack_num);
    uint16_t data_off  = ns_ntohs(tcp_hdr->data_off_flags);
    uint16_t hdr_words = data_off >> 12;
    uint8_t  flags     = data_off & 0x3F;
    if (hdr_words != TCP_HDR_WORDS) {
        packet_free(pkt);
        iface->rx_errors++;
        return -1;
    }
    size_t payload_len = pkt->len - TCP_HDR_LEN;
    
    Tcb *tcb      = tcp_find_exact(tcp_ctx->table, dst_ip, dst_port, src_ip, src_port);
    Tcb *listener = NULL;
    if (!tcb && flags == TCP_FLAG_SYN) {
        /*
         * No exact match, check for listener (SYN packets only)
         */
        listener = tcp_find_listener(tcp_ctx->table, dst_ip, dst_port);
    }

    /*
     * LISTEN Receives SYN
     */
    if (listener) {
        Tcb *child = tcp_alloc_tcb(tcp_ctx->table);
        if (!child) {
            packet_free(pkt);
            iface->rx_dropped++;
            return -1;
        }

        child->local_ip        = dst_ip;
        child->local_port      = dst_port;
        child->remote_ip       = src_ip;
        child->remote_port     = src_port;
        child->state           = TCP_SYN_RECEIVED;
        child->snd_una         = 0;
        child->snd_nxt         = 1;
        child->rcv_nxt         = seq_num + 1;
        child->snd_wnd         = TCP_DEFAULT_WINDOW;
        child->rcv_wnd         = TCP_DEFAULT_WINDOW;
        child->recv_handler    = listener->recv_handler;
        child->connect_handler = listener->connect_handler;
        child->handler_ctx     = listener->handler_ctx;
        
        Simulator *sim           = tcp_ctx->sim;
        Packet    *syn_ack_clone = NULL;
        uint64_t   now           = sim->sched ? scheduler_now(sim->sched) : 0;
        if (tcp_send_segment(sim, 
                             child->local_ip, 
                             child->remote_ip, 
                             child->local_port, 
                             child->remote_port, 
                             0, 
                             child->rcv_nxt, 
                             TCP_FLAG_SYN | TCP_FLAG_ACK, 
                             NULL, 
                             0, 
                             &syn_ack_clone) == -1) {
            tcp_release_tcb(tcp_ctx->table, child);
            packet_free(pkt);
            iface->rx_errors++;
            return -1;
        }

        if ((tcp_sendq_track(child, 
                             syn_ack_clone, 
                             0, 
                             1, 
                             TCP_FLAG_SYN | TCP_FLAG_ACK, 
                             now)) == -1) {
            packet_free(syn_ack_clone);
            tcp_release_tcb(tcp_ctx->table, child);
            packet_free(pkt);
            iface->rx_errors++;
            return -1;
        }
        packet_free(pkt);
        return 0;
    }

    if (!tcb) {
        packet_free(pkt);
        iface->rx_dropped++;
        return -1;
    }

    switch (tcb->state)
    {
        /*
         * SYN_SENT Receives SYN-ACK
         */
        case TCP_SYN_SENT:
            if ((flags & TCP_FLAG_SYN)  && 
                (flags & TCP_FLAG_ACK)  && 
                ack_num == tcb->snd_nxt) {
                tcb->rcv_nxt = seq_num + 1;
                tcb->snd_una = ack_num;
                tcp_sendq_ack(tcb, ack_num);
                tcb->state  = TCP_ESTABLISHED;
                int res = tcp_send_segment(tcp_ctx->sim, 
                                           tcb->local_ip, 
                                           tcb->remote_ip, 
                                           tcb->local_port, 
                                           tcb->remote_port, 
                                           tcb->snd_nxt, 
                                           tcb->rcv_nxt, 
                                           TCP_FLAG_ACK, 
                                           NULL, 
                                           0, 
                                           NULL);
                if (res == -1) {
                    packet_free(pkt);
                    iface->rx_errors++;
                    return -1;
                }

                if (tcb->connect_handler) {
                    tcb->connect_handler(tcb, tcb->handler_ctx);
                }

                packet_free(pkt);
                return 0;
            }
            break;

        /*
         * SYN_RECEIVED Receives Final ACK
         */
        case TCP_SYN_RECEIVED:
            if ((flags & TCP_FLAG_ACK) && 
                 ack_num == tcb->snd_nxt) {
                tcb->snd_una = ack_num;
                tcp_sendq_ack(tcb, ack_num);
                tcb->state = TCP_ESTABLISHED;
                if (tcb->connect_handler) {
                    tcb->connect_handler(tcb, tcb->handler_ctx);
                }
                packet_free(pkt);
                return 0;
            }
            break;

        /*
         * ESTABLISHED Receives ACK Or Data
         */
        case TCP_ESTABLISHED:
            /*        
             * 1. Update send queue based on ACK field
             */
            if (flags & TCP_FLAG_ACK) {
                if (ack_num > tcb->snd_una && ack_num <= tcb->snd_nxt) {
                    tcp_sendq_ack(tcb, ack_num);
                    tcb->snd_una = ack_num;
                }
            }
            /*
             * 2. Update receive queue based on payload
             */
            if (payload_len > 0) {
                if (seq_num == tcb->rcv_nxt) {
                    tcb->rcv_nxt += payload_len;
                    int res = tcp_send_segment(tcp_ctx->sim, 
                                               tcb->local_ip, 
                                               tcb->remote_ip, 
                                               tcb->local_port, 
                                               tcb->remote_port, 
                                               tcb->snd_nxt, 
                                               tcb->rcv_nxt, 
                                               TCP_FLAG_ACK, 
                                               NULL, 
                                               0, 
                                               NULL);
                    if (res == -1) {
                        packet_free(pkt);
                        iface->rx_errors++;
                        return -1;
                    }

                    if (packet_strip(pkt, TCP_HDR_LEN) == -1) {
                        packet_free(pkt);
                        iface->rx_errors++;
                        return -1;
                    }

                    pkt->layer = 5;

                    if (tcb->recv_handler) {
                        tcb->recv_handler(tcb, pkt, tcb->handler_ctx);
                        return 0;
                    } else {
                        packet_free(pkt);
                        return 0;
                    }
                } else {
                    int res = tcp_send_segment(tcp_ctx->sim, 
                                               tcb->local_ip, 
                                               tcb->remote_ip, 
                                               tcb->local_port, 
                                               tcb->remote_port, 
                                               tcb->snd_nxt, 
                                               tcb->rcv_nxt, 
                                               TCP_FLAG_ACK, 
                                               NULL, 
                                               0, 
                                               NULL);
                    if (res == -1) {
                        packet_free(pkt);
                        iface->rx_errors++;
                        return -1;
                    }
                    packet_free(pkt);
                    return 0;
                }
            }
            /*
             * 3. Handle FIN
             */
            if (payload_len == 0) {
                if ((flags & TCP_FLAG_FIN) && seq_num == tcb->rcv_nxt) {
                    tcb->rcv_nxt = seq_num + 1;
                    int res = tcp_send_segment(tcp_ctx->sim,
                                               tcb->local_ip,
                                               tcb->remote_ip,
                                               tcb->local_port,
                                               tcb->remote_port,
                                               tcb->snd_nxt,
                                               tcb->rcv_nxt,
                                               TCP_FLAG_ACK,
                                               NULL,
                                               0,
                                               NULL);
                    if (res == -1) {
                        packet_free(pkt);
                        iface->rx_errors++;
                        return -1;
                    }

                    tcb->state = TCP_CLOSE_WAIT;
                    packet_free(pkt);
                    return 0;
                }

                if (!(flags & TCP_FLAG_FIN)) {
                    packet_free(pkt);
                    return 0;
                }
            }
            break;

        /*
         * FIN_WAIT_1 Receives ACK
         */
        case TCP_FIN_WAIT_1:
            if ((flags & TCP_FLAG_ACK) && ack_num > tcb->snd_una) {
                tcb->snd_una = ack_num;
                tcp_sendq_ack(tcb, ack_num);
                int fin_still_unacked = 0;
                for (int i = 0; i < TCP_MAX_INFLIGHT; i++) {
                    TcpSegment *seg = &tcb->sendq.entries[i];
                    if (seg->pkt && (seg->flags & TCP_FLAG_FIN)) {
                        fin_still_unacked = 1;
                        break;
                    }
                }
                int fin_is_acked = !fin_still_unacked;
                
                if (fin_is_acked) {
                    tcb->state = TCP_FIN_WAIT_2;
                }

                packet_free(pkt);
                return 0;
            }
            break;

        /*
         * FIN_WAIT_2 Receives FIN
         */
        case TCP_FIN_WAIT_2:
            if ((flags & TCP_FLAG_FIN) && seq_num == tcb->rcv_nxt) {
                tcb->rcv_nxt = seq_num + 1;
                int res = tcp_send_segment(tcp_ctx->sim, 
                                           tcb->local_ip, 
                                           tcb->remote_ip, 
                                           tcb->local_port, 
                                           tcb->remote_port, 
                                           tcb->snd_nxt, 
                                           tcb->rcv_nxt, 
                                           TCP_FLAG_ACK, 
                                           NULL, 
                                           0, 
                                           NULL);
                if (res == -1) { 
                    packet_free(pkt);
                    iface->rx_errors++;
                    return -1;
                }
                tcb->state = TCP_TIME_WAIT;
                if (tcp_ctx->sim->sched != NULL) {
                    uint64_t now = scheduler_now(tcp_ctx->sim->sched);
                    Event   *e   = event_create_callback(EVT_TIMER_EXPIRED, 
                                                         now + TCP_TIME_WAIT_US, 
                                                         NULL, 
                                                         NULL, 
                                                         NULL, 
                                                         tcb, 
                                                         tcp_time_wait_handler, 
                                                        (TcpTable *)tcp_ctx->table);
                    if (e) {
                        int res = scheduler_schedule(tcp_ctx->sim->sched, e);
                        if (res == -1) {
                            event_free(e);
                        }
                    }
                }

                packet_free(pkt);
                return 0;
            } else {
                packet_free(pkt);
                return 0;
            }
            break;
    
        /*
         * CLOSING Receives ACK
         */
        case TCP_CLOSING:
            if (ack_num > tcb->snd_una) {
                tcb->snd_una = ack_num;
                tcp_sendq_ack(tcb, ack_num);

                int fin_still_unacked = 0;
                for (int i = 0; i < TCP_MAX_INFLIGHT; i++) {
                    TcpSegment *seg = &tcb->sendq.entries[i];
                    if (seg->pkt && (seg->flags & TCP_FLAG_FIN)) {
                        fin_still_unacked = 1;
                        break;
                    }
                }
                int fin_is_acked = !fin_still_unacked;
                
                if (fin_is_acked) {            
                    tcb->state = TCP_TIME_WAIT;
                    if (tcp_ctx->sim->sched != NULL) {
                        uint64_t now = scheduler_now(tcp_ctx->sim->sched);
                        Event   *e   = event_create_callback(EVT_TIMER_EXPIRED, 
                                                             now + TCP_TIME_WAIT_US, 
                                                             NULL, 
                                                             NULL, 
                                                             NULL, 
                                                             tcb, 
                                                             tcp_time_wait_handler, 
                                                            (TcpTable *)tcp_ctx->table);
                        if (e) {
                            int res = scheduler_schedule(tcp_ctx->sim->sched, e);
                            if (res == -1) {
                                event_free(e);
                            }
                        }
                    }
                    
                    packet_free(pkt);
                    return 0;
                }
                
                packet_free(pkt);
                return 0;
            }
            break;
    
        /*
         * LAST_ACK Receives ACK
         */
        case TCP_LAST_ACK:
            if ((flags & TCP_FLAG_ACK) && ack_num > tcb->snd_una) {
                tcb->snd_una = ack_num;
                tcp_sendq_ack(tcb, ack_num);
                int fin_still_unacked = 0;
                for (int i = 0; i < TCP_MAX_INFLIGHT; i++) {
                    TcpSegment *seg = &tcb->sendq.entries[i];
                    if (seg->pkt && (seg->flags & TCP_FLAG_FIN)) {
                        fin_still_unacked = 1;
                        break;
                    }
                }
                int fin_is_acked = !fin_still_unacked;
                if (fin_is_acked) {
                    tcp_release_tcb(tcp_ctx->table, tcb);
                    packet_free(pkt);
                    return 0;
                }

                packet_free(pkt);
                return 0;
            }
            break;
 
        /*
         * TIME_WAIT Consumes Late Segment
         */
        case TCP_TIME_WAIT:
            packet_free(pkt);
            return 0;

        /*
         * Unhandled State
         */
        default:
            packet_free(pkt);  
            iface->rx_dropped++;
            return -1;
    }

    packet_free(pkt);
    iface->rx_dropped++;
    return -1;
}

int  tcp_send(Simulator     *sim,
              Tcb           *tcb,
              const uint8_t *data,
              size_t         len) {
    if (!sim || !tcb) {
        return -1;
    }

    if (tcb->valid != 1) {
        return -1;
    }

    if (tcb->state != TCP_ESTABLISHED) {
        return -1;
    }

    if (len > 0 && !data) {
        return -1;
    }

    if (len == 0) {
        return 0;
    }

    size_t send_len = len > TCP_MSS ? TCP_MSS : len;

    if (tcp_sendq_has_unacked(tcb)) {
        return -1;
    }   

    uint32_t old_snd_nxt = tcb->snd_nxt;
    Packet *track_clone = NULL;
    int res = tcp_send_segment(sim, 
                               tcb->local_ip, 
                               tcb->remote_ip, 
                               tcb->local_port, 
                               tcb->remote_port, 
                               old_snd_nxt, 
                               tcb->rcv_nxt, 
                               TCP_FLAG_ACK | TCP_FLAG_PSH, 
                               data, 
                               send_len, 
                               &track_clone);
    if (res == -1) {
        packet_free(track_clone);
        return -1;
    } 

    uint64_t now = sim->sched ? scheduler_now(sim->sched) : 0;
    if (tcp_sendq_track(tcb, 
                        track_clone, 
                        old_snd_nxt, 
                        old_snd_nxt + send_len, 
                        TCP_FLAG_ACK | TCP_FLAG_PSH, 
                        now) == -1) {
        packet_free(track_clone);
        return -1;
    }

    tcb->snd_nxt += send_len;
    if (sim->sched) {
        tcp_schedule_retransmit(sim, NULL, tcb);
    }   

    return 0;
}


int  tcp_close(Simulator *sim, Tcb *tcb) {
    if (!sim || !tcb) {
        return -1;
    }

    if (tcb->valid != 1) {
        return -1;
    }

    if (tcb->state != TCP_ESTABLISHED && tcb->state != TCP_CLOSE_WAIT) {
        return -1;
    }

    if (tcp_sendq_has_unacked(tcb)) {
        return -1;
    }

    TcpState old_state = tcb->state;
    Packet  *track_clone = NULL;
    int res = tcp_send_segment(sim, 
                               tcb->local_ip, 
                               tcb->remote_ip, 
                               tcb->local_port, 
                               tcb->remote_port, 
                               tcb->snd_nxt, 
                               tcb->rcv_nxt, 
                               TCP_FLAG_FIN | TCP_FLAG_ACK, 
                               NULL, 
                               0, 
                               &track_clone);
    if (res == -1) {
        packet_free(track_clone);
        return -1;
    }
    uint64_t now = sim->sched ? scheduler_now(sim->sched) : 0;
    if (tcp_sendq_track(tcb, 
                        track_clone, 
                        tcb->snd_nxt, 
                        tcb->snd_nxt + 1, 
                        TCP_FLAG_FIN | TCP_FLAG_ACK, 
                        now) == -1) {
        packet_free(track_clone);
        return -1;
    }   
    tcb->snd_nxt += 1;
    if (old_state == TCP_ESTABLISHED) {
        tcb->state = TCP_FIN_WAIT_1;
    } else if (old_state == TCP_CLOSE_WAIT) {
        tcb->state = TCP_LAST_ACK;
    }

    if (sim->sched) {
        tcp_schedule_retransmit(sim, NULL, tcb);
    }

    return 0;
}

void tcp_retransmit_handler(const Event *e, void *ctx) {
    if (!e) {
        return;
    }

    TcpContext *tcp_ctx = (TcpContext *)ctx;
    if (!tcp_ctx || !tcp_ctx->sim || !tcp_ctx->table) {
        return;
    }

    Tcb *tcb = (Tcb *)e->data;
    if (!tcb || tcb->valid != 1) {
        return;
    }

    for (int i = 0; i < TCP_MAX_INFLIGHT; i++) {
        TcpSegment *seg = &tcb->sendq.entries[i];
        if (seg->pkt && seg->acked == 0) {
            Packet *clone = packet_clone(seg->pkt);
            if (!clone) {
                continue;
            }

            if (ip_output(tcp_ctx->sim, 
                          tcb->local_ip, 
                          tcb->remote_ip, 
                          IPPROTO_TCP, 
                          clone) == -1) {
                packet_free(clone);
                return;
            }

            seg->retransmits++;
            if (tcp_ctx->sim->sched) {
                seg->sent_ts = scheduler_now(tcp_ctx->sim->sched);
                tcp_schedule_retransmit(tcp_ctx->sim, tcp_ctx, tcb);
            }
        }

    }
}
