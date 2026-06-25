# Module 12 - MAC Table

**Files:** `src/network/mac_table.c`, `src/network/mac_table.h`
**Status:** Implemented
**Depends on:** `interface`

## Concepts First

A switch receives Ethernet frames on ports. To forward a unicast frame
efficiently, it needs to know:

```text
Which port leads to this destination MAC address?
```

The MAC table stores that learned mapping:

```text
MAC address -> Interface *
```

This table is also called:

- CAM table
- forwarding database
- bridge forwarding table

### Learning

Switches learn from source MAC addresses.

When a frame arrives on port `P` with source MAC `AA:AA:AA:AA:AA:AA`, the
switch can learn:

```text
AA:AA:AA:AA:AA:AA is reachable through port P
```

If the same MAC is seen later on another port, the table updates the port. That
models a host moving or a topology change.

### Lookup

When the switch sees a destination MAC:

- if the MAC table has an entry, forward only to that port
- if the MAC table has no entry, flood to all eligible ports except ingress

The MAC table module only performs lookup. The switch module decides whether to
forward or flood.

### Aging

Learned MAC locations can become stale. A host can move or disconnect.

Each entry stores a timestamp. `mac_table_age(table, now)` invalidates entries
whose age is greater than `MAC_AGE_MS`.

Current implementation uses:

```c
now - timestamp > MAC_AGE_MS
```

So an entry whose age is exactly `300000` remains valid; it expires only when
the age is greater than `300000`.

### Holes In The Table

The table is a fixed array of 1024 entries. Aging and flush operations mark
entries invalid; they do not compact the array.

That means valid entries may be anywhere in:

```text
entries[0 .. 1023]
```

All operations must scan the full array. Do not scan only `0 .. count - 1`.

### Borrowed Ports

Each MAC entry stores an `Interface *port`.

The MAC table does not own that interface. It never frees ports. If a port is
removed, switch code should call `mac_table_flush_port` before the interface is
freed.

## Purpose

The MAC table module is a bounded data structure for switch learning.

It provides:

- table initialization
- MAC learning/update
- MAC lookup
- aging of stale entries
- flushing entries for one port

It does not:

- parse Ethernet frames
- transmit packets
- flood packets
- own interfaces
- allocate table storage
- run its own timer
- schedule events

## Architecture Boundary

| Responsibility | Owner |
| --- | --- |
| Store learned MAC-to-port mappings | MAC table |
| Decide when to learn source MAC | Switch |
| Decide whether to forward or flood | Switch |
| Own switch ports/interfaces | Device/Switch |
| Own MAC table storage | Switch |
| Trigger periodic aging | Switch timer/event code |
| Free interfaces | Device/Switch, not MAC table |

MAC table is pure data structure code. It should not include scheduler,
simulator, Ethernet, packet, or switch behavior.

Current `mac_table.c` includes `simulator.h`, but the implementation does not
use simulator APIs. That include is unnecessary for the module boundary.

## Data Model

### Constants

```c
#define MAC_TABLE_SIZE 1024
#define MAC_AGE_MS     300000
#define MAC_ADDR_LEN   6
```

### `MacEntry`

```c
typedef struct MacEntry {
    uint8_t    mac[MAC_ADDR_LEN];
    uint8_t    _pad[2];
    Interface *port;
    uint64_t   timestamp;
    int        valid;
    int        _pad2;
} MacEntry;
```

Field meanings:

| Field | Meaning |
| --- | --- |
| `mac` | Learned six-byte MAC address. |
| `_pad` | Padding for alignment. |
| `port` | Borrowed egress/learned interface pointer. |
| `timestamp` | Time when the MAC was last learned/refreshed. |
| `valid` | `1` means active entry; `0` means empty/stale slot. |
| `_pad2` | Padding for alignment. |

### `MacTable`

```c
typedef struct MacTable {
    MacEntry entries[MAC_TABLE_SIZE];
    int      count;
    int      _pad;
} MacTable;
```

`count` is the number of valid entries.

The implementation does not maintain entries compactly.

## Ownership And Lifetime

The MAC table module does not allocate or free `MacTable`.

`mac_table_init` initializes existing storage.

`MacEntry.port` is borrowed. The table does not own or free it.

When `mac_table_learn` succeeds, it returns a pointer into the table:

```text
&table->entries[i]
```

The caller must not free that pointer.

## Public API

```c
void        mac_table_init(MacTable *table);

MacEntry   *mac_table_learn(MacTable      *table,
                            const uint8_t  mac[6],
                            Interface     *port,
                            uint64_t       now);

Interface  *mac_table_lookup(MacTable *table,
                             const uint8_t mac[6]);

void        mac_table_age(MacTable *table, uint64_t now);

void        mac_table_flush_port(MacTable *table, Interface *port);
```

## Function Behavior

### `mac_table_init`

Required behavior:

- If `table == NULL`, return immediately.
- Clear the whole `MacTable` object.
- Set `count == 0`.
- Mark every entry invalid.

Current implementation uses `memset(table, 0, sizeof(MacTable))`.

### `mac_table_learn`

Required behavior:

- If `table == NULL`, return `NULL`.
- If `mac == NULL`, return `NULL`.
- If `port == NULL`, return `NULL`.
- Scan all 1024 entries for a valid entry whose MAC matches all six bytes.
- If found:
  - update `port`
  - update `timestamp`
  - leave `count` unchanged
  - return pointer to that entry
- Otherwise, scan all 1024 entries for the first invalid slot.
- If found:
  - copy all six MAC bytes
  - store borrowed `port`
  - store `timestamp = now`
  - set `valid = 1`
  - increment `count`
  - return pointer to that entry
- If no invalid slot exists, return `NULL`.

### `mac_table_lookup`

Required behavior:

- If `table == NULL`, return `NULL`.
- If `mac == NULL`, return `NULL`.
- Scan all 1024 entries.
- If a valid entry matches all six MAC bytes, return its `port`.
- If no match exists, return `NULL`.

The returned `Interface *` is borrowed from the table entry.

### `mac_table_age`

Required behavior:

- If `table == NULL`, return immediately.
- Scan all 1024 entries.
- For each valid entry where:

```text
now - entry.timestamp > MAC_AGE_MS
```

  - set `valid = 0`
  - decrement `count`

The current implementation does not clear `port` or MAC bytes when aging an
entry. The `valid` bit is the authority.

The current implementation does not guard against unsigned timestamp
wraparound.

### `mac_table_flush_port`

Required behavior:

- If `table == NULL`, return immediately.
- If `port == NULL`, return immediately.
- Scan all 1024 entries.
- For each valid entry whose `entry.port == port`:
  - set `valid = 0`
  - decrement `count`

The current implementation does not clear `entry.port` after invalidation. The
`valid` bit is the authority.

## Flow Charts

### Learn

```text
mac_table_learn(table, mac, port, now)
  |
  +-- reject NULL inputs
  |
  +-- scan all entries for valid matching MAC
  |     |
  |     +-- found:
  |          update port and timestamp
  |          return entry
  |
  +-- scan all entries for first invalid slot
        |
        +-- found:
        |     copy MAC
        |     store port and timestamp
        |     valid = 1
        |     count++
        |     return entry
        |
        +-- none:
              return NULL
```

### Lookup

```text
mac_table_lookup(table, mac)
  |
  +-- reject NULL inputs
  |
  +-- scan all entries
        |
        +-- valid matching MAC: return entry.port
  |
  +-- return NULL
```

### Age

```text
mac_table_age(table, now)
  |
  +-- NULL table: return
  |
  +-- for each entry:
        |
        +-- valid and now - timestamp > 300000:
              valid = 0
              count--
```

## ACSL Contracts

The contracts belong in `mac_table.h`. Use literal bounds in annotations:

- table entries: `0 .. 1023`
- MAC bytes: `0 .. 5`

### Shared Predicates

```c
/*@
    predicate mac_bytes_equal(uint8_t *a, uint8_t *b) =
        \valid_read(a + (0 .. 5)) &&
        \valid_read(b + (0 .. 5)) &&
        a[0] == b[0] &&
        a[1] == b[1] &&
        a[2] == b[2] &&
        a[3] == b[3] &&
        a[4] == b[4] &&
        a[5] == b[5];

    predicate mac_table_count_valid(MacTable *table) =
        0 <= table->count && table->count <= 1024;

    predicate mac_table_slots_valid(MacTable *table) =
        \forall integer i; 0 <= i && i < 1024 ==>
            (table->entries[i].valid == 0 ||
             (table->entries[i].valid == 1 &&
              table->entries[i].port != \null));

    predicate mac_table_well_formed(MacTable *table) =
        \valid(table) &&
        mac_table_count_valid(table) &&
        mac_table_slots_valid(table);
*/
```

### `mac_table_init`

```c
/*@
    behavior null:
        assumes table == \null;
        assigns \nothing;

    behavior valid:
        assumes \valid(table);
        assigns table->entries[0 .. 1023],
                table->count;
        ensures table->count == 0;
        ensures \forall integer i; 0 <= i && i < 1024 ==>
                table->entries[i].valid == 0;
        ensures mac_table_well_formed(table);

    complete behaviors;
    disjoint behaviors;
*/
void mac_table_init(MacTable *table);
```

### `mac_table_learn`

```c
/*@
    behavior bad_input:
        assumes table == \null || mac == \null || port == \null;
        assigns \nothing;
        ensures \result == \null;

    behavior valid:
        assumes mac_table_well_formed(table);
        assumes \valid_read(mac + (0 .. 5));
        assumes \valid(port);
        assigns table->entries[0 .. 1023],
                table->count;
        ensures \result == \null ||
                (\exists integer i; 0 <= i && i < 1024 &&
                    \result == &table->entries[i] &&
                    table->entries[i].valid == 1 &&
                    table->entries[i].port == port &&
                    table->entries[i].timestamp == now);
        ensures \result == \null ||
                table->count == \old(table->count) ||
                table->count == \old(table->count) + 1;
        ensures mac_table_well_formed(table);

    complete behaviors;
    disjoint behaviors;
*/
MacEntry *mac_table_learn(MacTable *table,
                          const uint8_t mac[6],
                          Interface *port,
                          uint64_t now);
```

Additional required proof/test property:

- Updating an existing MAC leaves `count` unchanged.
- Inserting a new MAC increments `count`.
- Full-table miss returns `NULL` and leaves `count` unchanged.
- On non-null result, `result->mac[0 .. 5]` equals `mac[0 .. 5]`.

### `mac_table_lookup`

```c
/*@
    behavior bad_input:
        assumes table == \null || mac == \null;
        assigns \nothing;
        ensures \result == \null;

    behavior valid:
        assumes mac_table_well_formed(table);
        assumes \valid_read(mac + (0 .. 5));
        assigns \nothing;
        ensures \result == \null ||
                (\exists integer i; 0 <= i && i < 1024 &&
                    table->entries[i].valid == 1 &&
                    table->entries[i].port == \result);

    complete behaviors;
    disjoint behaviors;
*/
Interface *mac_table_lookup(MacTable *table, const uint8_t mac[6]);
```

Additional required proof/test property:

- A non-null result belongs to a valid matching MAC entry.
- A null result means no valid entry matches all six MAC bytes.

### `mac_table_age`

```c
/*@
    behavior null:
        assumes table == \null;
        assigns \nothing;

    behavior valid:
        assumes mac_table_well_formed(table);
        assigns table->entries[0 .. 1023],
                table->count;
        ensures table->count <= \old(table->count);
        ensures \forall integer i; 0 <= i && i < 1024 ==>
                (table->entries[i].valid == 0 ||
                 now - table->entries[i].timestamp <= 300000);
        ensures mac_table_well_formed(table);

    complete behaviors;
    disjoint behaviors;
*/
void mac_table_age(MacTable *table, uint64_t now);
```

### `mac_table_flush_port`

```c
/*@
    behavior null:
        assumes table == \null || port == \null;
        assigns \nothing;

    behavior valid:
        assumes mac_table_well_formed(table);
        assumes \valid(port);
        assigns table->entries[0 .. 1023],
                table->count;
        ensures table->count <= \old(table->count);
        ensures \forall integer i; 0 <= i && i < 1024 ==>
                (table->entries[i].valid == 0 ||
                 table->entries[i].port != port);
        ensures mac_table_well_formed(table);

    complete behaviors;
    disjoint behaviors;
*/
void mac_table_flush_port(MacTable *table, Interface *port);
```

## KLEVA Verification Plan

Minimum KLEVA tests:

1. `mac_table_init(NULL)` does not crash.
2. `mac_table_init(valid)` sets `count == 0`.
3. `mac_table_init(valid)` clears all 1024 valid bits.
4. `mac_table_learn` rejects NULL table, MAC, or port.
5. Learning a new MAC returns a non-null entry.
6. New learn copies all six MAC bytes.
7. New learn stores port and timestamp.
8. New learn increments count.
9. Learning existing MAC updates port.
10. Learning existing MAC updates timestamp.
11. Learning existing MAC does not increment count.
12. Learning into a full table with no matching MAC returns `NULL`.
13. Full-table learn failure leaves count unchanged.
14. `mac_table_lookup` rejects NULL table or MAC.
15. Lookup hit returns learned port.
16. Lookup miss returns `NULL`.
17. Lookup ignores invalid entries.
18. `mac_table_age(NULL, now)` does not crash.
19. Aging removes entries with age greater than `300000`.
20. Aging keeps entries with age equal to `300000`.
21. Aging keeps fresh entries.
22. Aging decrements count for each invalidated entry.
23. `mac_table_flush_port(NULL, port)` does not crash.
24. `mac_table_flush_port(table, NULL)` does not crash.
25. Flush invalidates entries pointing at the port.
26. Flush leaves entries for other ports valid.
27. Flush decrements count for each invalidated entry.

## Common Mistakes

- Do not scan only `0 .. count - 1`; holes are possible.
- Do not free `Interface *port`; it is borrowed.
- Do not say age expires entries at `>= 300000`; current code uses `> 300000`.
- Do not require invalidated entries to clear MAC or port; current code only
  clears `valid`.
- Do not put switch flooding or Ethernet parsing in this module.
- Do not assume `count` gives an insertion index.
