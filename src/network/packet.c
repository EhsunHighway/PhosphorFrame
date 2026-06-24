#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "packet.h"



Packet *packet_create(size_t capacity) {
    Packet *p = malloc(sizeof(Packet));
    if (!p) {
        return NULL;
    }

    p->head = malloc(PKT_HEADROOM + capacity);
    if (!p->head) {
        free(p);
        return NULL;
    }

    p->data                 = p->head + PKT_HEADROOM;   // payload starts after headroom
    p->len                  = 0;
    p->capacity             = capacity;
    p->layer                = 0;
    static uint32_t next_id = 1;
    p->id = next_id++;

    return p;
}


void packet_free(Packet *p) {
    if (!p) {
        return;
    }
    free(p->head);   // head is the original malloc'd pointer; data may have moved
    free(p);
}

int packet_prepend(Packet *p, const void *header, size_t header_len) {
    // Check there is enough headroom before data to fit the new header
    if ((size_t)(p->data - p->head) < header_len) {
        return -1;
    }

    p->data -= header_len;   // retreat data pointer into headroom
    p->len  += header_len;
    memcpy(p->data, header, header_len);
    return 0;
}


int packet_strip(Packet *p, size_t header_len) {
    if (header_len > p->len) {
        return -1;
    }

    // Advance the data pointer — header bytes remain in buffer, still readable
    // via (p->data - header_len) until the next prepend overwrites them.
    p->data += header_len;
    p->len  -= header_len;
    return 0;
}

Packet *packet_clone(const Packet *p) {
    // Allocate with capacity == p->len so the clone is exactly the right size.
    // The clone gets a fresh PKT_HEADROOM, so ethernet_send can prepend onto it.
    Packet *clone = packet_create(p->len);
    if (!clone) {
        return NULL;
    }

    memcpy(clone->data, p->data, p->len);
    clone->len   = p->len;
    clone->layer = p->layer;
    return clone;
}

uint16_t packet_checksum(const void *data, size_t len) {
    const uint16_t *words = (const uint16_t *)data;
    uint32_t sum = 0;

    while (len > 1) {
        sum += *words++;
        len -= 2;
    }

    if (len == 1) {
        sum += *(const uint8_t *)words;
    }

    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return (uint16_t)(~sum);
}

void packet_dump(const Packet *p) {
    printf("Packet ID: %d, Length: %zu, Layer: %d\n", p->id ,p->len, p->layer);
    printf("Data:\n");
    for (int i = 0;i < (int)p->len;i++) {
        printf("%02X ", p->data[i]);
        if ((i + 1) % 16 == 0) {
            printf("\n");
        }
    }
    printf("\n");
}
