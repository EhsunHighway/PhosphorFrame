# Module 10 - ARP Cache

**Files:** `src/protocols/arp_cache.c`, `src/protocols/arp_cache.h`
**Status:** Implemented core cache and pending queue
**Depends on:** `interface`, `packet`, `ip`, `simulator`

## Concepts First

Ethernet sends to MAC addresses. IPv4 code usually starts with an IP address.

The ARP cache is the per-node table that remembers:

```text
IPv4 address -> MAC address
```

Example:

```text
192.168.1.20 -> AA:BB:CC:11:22:33
```

The ARP cache is not the ARP protocol. It does not parse ARP requests or ARP
replies. It only stores learned mappings and queues packets that are waiting
for a mapping.

### Cache Versus ARP Protocol

ARP protocol code handles wire packets:

- build ARP request
- build ARP reply
- parse ARP request/reply
- decide whether a received ARP packet is relevant

ARP cache code handles storage:

- add mapping
- look up mapping
- expire old mapping
- enqueue packet while mapping is unknown
- flush queued packets when mapping becomes known

Keeping these separate matters because IP also needs the cache. IP should not
parse ARP packets, and ARP should not own IP's packet queue.

### Who Owns The Cache

The cache object is owned by the node, not by `arp.c`.

Current ownership model:

```text
Host or Router
  |
  +-- owns ArpCache storage
        |
        +-- Interface borrows pointer through iface->arp_cache
```

ARP uses the borrowed pointer when it learns mappings.

IP uses the borrowed pointer when it needs a destination MAC.

Interface does not own the cache. ARP does not own the cache.

### Learned Entries

A learned entry says:

```text
ip_addr is reachable at mac_addr
```

The entry has a timestamp so cleanup can expire old mappings.

The current implementation uses linear scans over 256 slots.

### Pending Packets

When IP wants to send a packet but the MAC address is unknown, the payload must
wait somewhere while ARP resolution happens.

That waiting area is the pending queue:

```text
target_ip -> queued payload + metadata needed to call ip_send later
```

After ARP learns the MAC address for `target_ip`, `arp_pending_flush` retries
the queued payloads by calling `ip_send`.

### Byte Order

Pending queue IP fields are host order because they are later passed directly to
`ip_send`.

For cache entries, the implementation stores the integer value exactly as passed
to `arp_cache_add` and compares it exactly in `arp_cache_lookup`. In the current
ARP/IP paths, this value should be treated as host-order IP. The header comment
currently says network byte order for `ArpCacheEntry.ip_addr`; that comment is
stale relative to the rest of the design and should be corrected when the header
ACSL/comments are updated.

## Purpose

The ARP cache module provides bounded ARP mapping storage and a bounded pending
packet queue.

It provides:

- cache initialization
- add/refresh learned mapping
- lookup learned mapping
- cleanup of stale mappings
- pending packet enqueue
- pending packet flush after resolution

It does not:

- allocate the `ArpCache` object
- free the `ArpCache` object
- parse ARP wire packets
- build ARP wire packets
- send ARP requests
- own interfaces
- inspect IP headers

## Architecture Boundary

| Responsibility | Owner |
| --- | --- |
| Own ARP cache storage | Host/Router |
| Store learned IP-to-MAC entries | ARP cache |
| Store unresolved outbound payloads | ARP cache pending queue |
| Parse ARP packets | ARP protocol |
| Add learned mappings after ARP receive | ARP protocol calling cache |
| Look up next-hop MAC before IP send | IP path calling cache |
| Retry queued payloads after resolution | ARP cache via `ip_send` |
| Own borrowed interface pointers | Host/Router/Device, not ARP cache |
| Own pending payload after enqueue success | ARP pending queue |

`arp_cache.c` includes `ip.h` because `arp_pending_flush` calls `ip_send`.

## Data Model

### Constants

```c
#define ARP_MAX_CACHE_SIZE      256
#define ARP_MAX_PENDING_PACKETS 32
```

### `ArpCacheEntry`

```c
typedef struct ArpCacheEntry {
    uint32_t ip_addr;
    uint8_t  mac_addr[6];
    uint64_t timestamp;
    int      valid;
} ArpCacheEntry;
```

Field meanings:

| Field | Meaning |
| --- | --- |
| `ip_addr` | IP key stored exactly as passed by caller; current design uses host order. |
| `mac_addr` | Learned six-byte Ethernet MAC address. |
| `timestamp` | Last update time supplied by caller. |
| `valid` | `1` means active entry; `0` means empty/expired slot. |

### `ArpPendingPacket`

```c
typedef struct ArpPendingPacket {
    uint32_t   target_ip;
    uint32_t   src_ip;
    uint32_t   dst_ip;
    uint8_t    protocol;
    Interface *iface;
    Packet    *payload;
    int        valid;
} ArpPendingPacket;
```

Field meanings:

| Field | Meaning |
| --- | --- |
| `target_ip` | Host-order next-hop IP waiting for MAC resolution. |
| `src_ip` | Host-order source IP for later `ip_send`. |
| `dst_ip` | Host-order destination IP for later `ip_send`. |
| `protocol` | IPv4 protocol byte for later `ip_send`. |
| `iface` | Borrowed outgoing interface pointer. |
| `payload` | Owned queued payload after successful enqueue. |
| `valid` | `1` means this pending slot is active. |

### `ArpCache`

```c
typedef struct ArpCache {
    ArpCacheEntry    entries[ARP_MAX_CACHE_SIZE];
    int              count;
    ArpPendingPacket pending[ARP_MAX_PENDING_PACKETS];
    int              pending_count;
} ArpCache;
```

`count` is the number of valid learned entries.

`pending_count` is the number of valid pending packet slots.

Scans must cover the full fixed arrays, not only `count` or `pending_count`,
because deletion/cleanup leaves holes.

## Initial State

`arp_cache_init` defines an empty cache. Empty does not mean `NULL`.

After `arp_cache_init(cache)` on a valid cache:

- `cache->count == 0`
- every cache entry has `valid == 0`
- `cache->pending_count == 0`
- every pending entry has `valid == 0`
- every pending entry has `iface == NULL`
- every pending entry has `payload == NULL`

The current implementation reaches this state with:

```c
memset(cache, 0, sizeof(*cache));
```

`arp_cache_init(NULL)` is a no-op.

## Ownership And Lifetime

The ARP cache module does not allocate or free `ArpCache`.

`arp_pending_enqueue` transfers payload ownership only on success:

- return `0`: pending queue owns `payload`
- return `-1`: caller still owns `payload`

The pending queue never owns `iface`.

`arp_pending_flush` clears matching pending slots before calling `ip_send`.

If `ip_send` succeeds, ownership of the payload has moved into the send path.

If `ip_send` fails or the pending slot lacks usable `iface`/`payload`, flush
frees the payload with `packet_free`.

## Public API

```c
void arp_cache_init(ArpCache *cache);

void arp_cache_add(ArpCache     *cache,
                   uint32_t      ip_addr,
                   const uint8_t mac_addr[6],
                   uint64_t      timestamp);

int  arp_cache_lookup(const ArpCache *cache,
                      uint32_t        ip_addr,
                      uint8_t         out_mac[6]);

int  arp_pending_enqueue(ArpCache  *cache,
                         Interface *iface,
                         uint32_t   target_ip,
                         uint32_t   src_ip,
                         uint32_t   dst_ip,
                         uint8_t    protocol,
                         Packet    *payload);

int  arp_pending_flush(Simulator    *sim,
                       ArpCache     *cache,
                       uint32_t      target_ip,
                       const uint8_t mac_addr[6]);

void arp_cache_cleanup(ArpCache *cache, uint64_t current_time);
```

## Function Behavior

### `arp_cache_init`

Required behavior:

- If `cache == NULL`, return immediately.
- Clear the whole `ArpCache` object.

This function is the public initializer. Host and Router creation should call
it instead of manually setting fields.

### `arp_cache_add`

Required behavior:

- If `cache == NULL`, return without changing state.
- If `ip_addr == 0`, return without changing state.
- Search all 256 cache entries for a valid entry with the same `ip_addr`.
- If found:
  - copy six MAC bytes from `mac_addr`
  - update `timestamp`
  - leave `count` unchanged
  - return
- Otherwise, search all 256 cache entries for the first invalid slot.
- If found:
  - store `ip_addr`
  - copy six MAC bytes from `mac_addr`
  - store `timestamp`
  - set `valid = 1`
  - increment `count`
  - return
- If no free slot exists, leave state unchanged.

The current implementation does not check `mac_addr == NULL`. Callers must pass
a readable six-byte MAC pointer when `cache != NULL` and `ip_addr != 0`.

### `arp_cache_lookup`

Required behavior:

- If `cache == NULL`, return `-1`.
- If `ip_addr == 0`, return `-1`.
- If `out_mac == NULL`, return `-1`.
- Search all 256 cache entries.
- If a valid entry with matching `ip_addr` exists:
  - copy six MAC bytes into `out_mac`
  - return `0`
- Otherwise return `-1`.

Lookup does not send ARP requests.

### `arp_pending_enqueue`

Required behavior:

- If `cache == NULL`, return `-1`.
- If `iface == NULL`, return `-1`.
- If `target_ip == 0`, return `-1`.
- If `payload == NULL`, return `-1`.
- Search all 32 pending slots for the first invalid slot.
- If found:
  - store `target_ip`
  - store `src_ip`
  - store `dst_ip`
  - store `protocol`
  - store borrowed `iface`
  - store owned `payload`
  - set `valid = 1`
  - increment `pending_count`
  - return `0`
- If no free slot exists, return `-1`.

### `arp_pending_flush`

Required behavior:

- If `sim == NULL`, return `0`.
- If `cache == NULL`, return `0`.
- If `target_ip == 0`, return `0`.
- If `mac_addr == NULL`, return `0`.
- Initialize `sent = 0`.
- Scan all 32 pending slots.
- For each valid pending slot whose `target_ip` matches:
  - copy `payload`, `iface`, `src_ip`, `dst_ip`, and `protocol` into locals
  - set pending `valid = 0`
  - set pending `payload = NULL`
  - set pending `iface = NULL`
  - decrement `pending_count` if it is positive
  - if `iface != NULL`, `payload != NULL`, and `ip_send(...) >= 0`, increment
    `sent`
  - otherwise call `packet_free(payload)`
- Return `sent`.

`mac_addr` is passed to `ip_send` as the destination MAC.

### `arp_cache_cleanup`

Required behavior:

- If `cache == NULL`, return immediately.
- Scan all 256 cache entries.
- If an entry is valid and:

```text
current_time - entry.timestamp >= 300000
```

then:

- set `valid = 0`
- decrement `count`

Cleanup does not touch pending packets.

The current code does not guard against unsigned timestamp wraparound.

## Flow Charts

### IP Send Miss

```text
IP wants to send payload
  |
  +-- arp_cache_lookup(cache, target_ip, mac)
        |
        +-- hit:
        |     send immediately through Ethernet/IP path
        |
        +-- miss:
              queue payload with arp_pending_enqueue
              send ARP request elsewhere
```

### ARP Reply Learns MAC

```text
ARP receive path accepts reply
  |
  +-- sender_ip = ARP sender protocol address converted to host order
  |
  +-- arp_cache_add(cache, sender_ip, sender_mac, now)
  |
  +-- arp_pending_flush(sim, cache, sender_ip, sender_mac)
        |
        +-- for each matching pending payload:
              clear pending slot
              call ip_send(...)
```

### Initialize Cache

```text
arp_cache_init(cache)
  |
  +-- NULL cache: return
  |
  +-- memset whole object to zero
  |
  +-- counts are zero
  +-- valid bits are zero
  +-- pending pointers are NULL
```

## ACSL Contracts

The contracts belong in `arp_cache.h`. Use literal bounds for KLEVA/EVA:

- cache entries: `0 .. 255`
- pending entries: `0 .. 31`
- MAC bytes: `0 .. 5`

### Shared Predicates

```c
/*@
    predicate arp_cache_count_valid(ArpCache *cache) =
        0 <= cache->count && cache->count <= 256;

    predicate arp_pending_count_valid(ArpCache *cache) =
        0 <= cache->pending_count && cache->pending_count <= 32;

    predicate arp_cache_slots_valid(ArpCache *cache) =
        \forall integer i; 0 <= i && i < 256 ==>
            (cache->entries[i].valid == 0 || cache->entries[i].valid == 1);

    predicate arp_pending_slots_valid(ArpCache *cache) =
        \forall integer i; 0 <= i && i < 32 ==>
            ((cache->pending[i].valid == 0 &&
              cache->pending[i].iface == \null &&
              cache->pending[i].payload == \null) ||
             (cache->pending[i].valid == 1 &&
              cache->pending[i].target_ip != 0 &&
              cache->pending[i].iface != \null &&
              cache->pending[i].payload != \null));

    predicate arp_cache_well_formed(ArpCache *cache) =
        \valid(cache) &&
        arp_cache_count_valid(cache) &&
        arp_pending_count_valid(cache) &&
        arp_cache_slots_valid(cache) &&
        arp_pending_slots_valid(cache);
*/
```

### `arp_cache_init`

```c
/*@
    behavior null:
        assumes cache == \null;
        assigns \nothing;

    behavior valid:
        assumes \valid(cache);
        assigns cache->entries[0 .. 255],
                cache->count,
                cache->pending[0 .. 31],
                cache->pending_count;
        ensures cache->count == 0;
        ensures cache->pending_count == 0;
        ensures \forall integer i; 0 <= i && i < 256 ==>
                cache->entries[i].valid == 0;
        ensures \forall integer i; 0 <= i && i < 32 ==>
                cache->pending[i].valid == 0;
        ensures \forall integer i; 0 <= i && i < 32 ==>
                cache->pending[i].iface == \null;
        ensures \forall integer i; 0 <= i && i < 32 ==>
                cache->pending[i].payload == \null;
        ensures arp_cache_well_formed(cache);

    complete behaviors;
    disjoint behaviors;
*/
void arp_cache_init(ArpCache *cache);
```

### `arp_cache_add`

```c
/*@
    behavior null_or_zero:
        assumes cache == \null || ip_addr == 0;
        assigns \nothing;

    behavior valid:
        assumes arp_cache_well_formed(cache);
        assumes ip_addr != 0;
        assumes \valid_read(mac_addr + (0 .. 5));
        assigns cache->entries[0 .. 255],
                cache->count;
        ensures cache->count == \old(cache->count) ||
                cache->count == \old(cache->count) + 1;
        ensures arp_cache_well_formed(cache);

    complete behaviors;
    disjoint behaviors;
*/
void arp_cache_add(ArpCache *cache,
                   uint32_t ip_addr,
                   const uint8_t mac_addr[6],
                   uint64_t timestamp);
```

Additional required proof/test property:

- Refreshing an existing valid IP updates MAC and timestamp without changing
  `count`.
- Inserting into a free slot stores all six MAC bytes and increments `count`.
- Adding when the table is full and no existing IP matches leaves `count`
  unchanged.

### `arp_cache_lookup`

```c
/*@
    behavior bad_input:
        assumes cache == \null || ip_addr == 0 || out_mac == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes \valid_read(cache);
        assumes ip_addr != 0;
        assumes \valid(out_mac + (0 .. 5));
        assigns out_mac[0 .. 5];
        ensures \result == 0 || \result == -1;

    complete behaviors;
    disjoint behaviors;
*/
int arp_cache_lookup(const ArpCache *cache,
                     uint32_t ip_addr,
                     uint8_t out_mac[6]);
```

Additional required proof/test property:

- On hit, `out_mac[0 .. 5]` equals the matching entry MAC.
- On miss, return `-1`.

### `arp_pending_enqueue`

```c
/*@
    behavior bad_input:
        assumes cache == \null || iface == \null ||
                payload == \null || target_ip == 0;
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes arp_cache_well_formed(cache);
        assumes \valid(iface);
        assumes packet_layout(payload);
        assumes target_ip != 0;
        assigns cache->pending[0 .. 31],
                cache->pending_count;
        ensures \result == 0 || \result == -1;
        ensures \result == 0 ==>
                cache->pending_count == \old(cache->pending_count) + 1;
        ensures \result == -1 ==>
                cache->pending_count == \old(cache->pending_count);
        ensures arp_cache_well_formed(cache);

    complete behaviors;
    disjoint behaviors;
*/
int arp_pending_enqueue(ArpCache *cache,
                        Interface *iface,
                        uint32_t target_ip,
                        uint32_t src_ip,
                        uint32_t dst_ip,
                        uint8_t protocol,
                        Packet *payload);
```

Additional required proof/test property:

- On success, one pending slot stores the exact metadata and payload pointer.
- On failure, payload ownership remains with the caller.

### `arp_pending_flush`

```c
/*@
    behavior bad_input:
        assumes sim == \null || cache == \null ||
                target_ip == 0 || mac_addr == \null;
        assigns \nothing;
        ensures \result == 0;

    behavior valid:
        assumes simulator_well_formed(sim);
        assumes arp_cache_well_formed(cache);
        assumes target_ip != 0;
        assumes \valid_read(mac_addr + (0 .. 5));
        assigns cache->pending[0 .. 31],
                cache->pending_count;
        ensures \result >= 0;
        ensures cache->pending_count <= \old(cache->pending_count);
        ensures \forall integer i; 0 <= i && i < 32 ==>
                cache->pending[i].valid == 0 ||
                cache->pending[i].target_ip != target_ip;
        ensures arp_cache_well_formed(cache);

    complete behaviors;
    disjoint behaviors;
*/
int arp_pending_flush(Simulator *sim,
                      ArpCache *cache,
                      uint32_t target_ip,
                      const uint8_t mac_addr[6]);
```

### `arp_cache_cleanup`

```c
/*@
    behavior null:
        assumes cache == \null;
        assigns \nothing;

    behavior valid:
        assumes arp_cache_well_formed(cache);
        assigns cache->entries[0 .. 255],
                cache->count;
        ensures cache->count <= \old(cache->count);
        ensures arp_cache_well_formed(cache);

    complete behaviors;
    disjoint behaviors;
*/
void arp_cache_cleanup(ArpCache *cache, uint64_t current_time);
```

Additional required proof/test property:

- Entries whose age is at least `300000` are invalidated.
- Entries whose age is less than `300000` remain valid.
- Pending queue state is unchanged by cleanup.

## KLEVA Verification Plan

Minimum KLEVA tests:

1. `arp_cache_init(NULL)` does not crash.
2. `arp_cache_init(valid)` sets `count == 0`.
3. `arp_cache_init(valid)` clears all 256 entry valid bits.
4. `arp_cache_init(valid)` sets `pending_count == 0`.
5. `arp_cache_init(valid)` clears all 32 pending valid bits.
6. `arp_cache_init(valid)` clears dirty pending `iface` pointers.
7. `arp_cache_init(valid)` clears dirty pending `payload` pointers.
8. `arp_cache_add(NULL, ...)` is a no-op.
9. `arp_cache_add(cache, 0, ...)` is a no-op.
10. `arp_cache_add` inserts a new entry.
11. New add copies all six MAC bytes.
12. New add increments `count`.
13. Add for existing IP refreshes MAC and timestamp.
14. Refresh does not increment `count`.
15. Add to full cache with no matching IP leaves `count` unchanged.
16. `arp_cache_lookup` rejects NULL cache, zero IP, and NULL output.
17. Lookup hit returns `0`.
18. Lookup hit copies all six MAC bytes.
19. Lookup miss returns `-1`.
20. `arp_pending_enqueue` rejects NULL cache, interface, payload, or zero
   target.
21. Pending enqueue success stores all metadata.
22. Pending enqueue success increments `pending_count`.
23. Pending enqueue failure leaves payload ownership with caller.
24. Pending enqueue full queue returns `-1`.
25. `arp_pending_flush` rejects NULL/zero inputs with return `0`.
26. Pending flush clears all matching target slots.
27. Pending flush decrements `pending_count` for cleared slots.
28. Pending flush calls `ip_send` for valid queued payloads.
29. Pending flush frees payload when handoff fails.
30. Cleanup rejects NULL cache.
31. Cleanup expires stale entries.
32. Cleanup leaves fresh entries valid.
33. Cleanup does not touch pending queue.

## Common Mistakes

- Do not treat an empty cache as `NULL`; empty is initialized storage with zero
  counts and invalid slots.
- Do not let Interface own or free the ARP cache.
- Do not let ARP protocol own or allocate the cache object.
- Do not free pending payload after successful enqueue; the queue owns it.
- Do not forget to clear stale pending `iface` and `payload` pointers in init
  and flush.
- Do not scan only `count`; entries can have holes.
- Do not use the stale `ArpCacheEntry.ip_addr` header comment as the design
  source of truth for byte order.
