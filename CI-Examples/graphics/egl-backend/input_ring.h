#pragma once

#include <stddef.h>
#include <stdint.h>

/* The VM uses one 64-byte communication slot. Keep a generous guard for future control fields
 * and place the input transport in the unused tail of the existing 4 KiB identity-mapped
 * communication page. */
#define SG_INPUT_RING_OFFSET 512U
#define SG_INPUT_RING_MAGIC UINT64_C(0x5347494e50555431) /* "SGINPUT1" */
#define SG_INPUT_RING_CAPACITY 64U
#define SG_INPUT_LATENCY_OFFSET 3072U
#define SG_INPUT_LATENCY_MAGIC UINT64_C(0x53474c4154454e31) /* "SGLATEN1" */

/* Compact, display-independent representation of the common fields in core X11 key, button and
 * motion events. Discrete events live in an ordered SPSC ring; motion uses a separate seqlock slot
 * so an arbitrarily fast pointer cannot crowd out a key or button release. */
typedef struct {
    uint8_t type;
    uint8_t detail;
    uint8_t same_screen;
    uint8_t reserved;
    uint32_t time;
    uint32_t window;
    uint32_t root;
    uint32_t child;
    int16_t root_x;
    int16_t root_y;
    int16_t event_x;
    int16_t event_y;
    uint16_t state;
    uint16_t reserved2;
} sg_input_event_t;

typedef struct {
    uint64_t magic;
    uint32_t enabled;
    uint32_t head;
    uint32_t tail;
    uint32_t dropped;
    uint32_t motion_seq;
    uint32_t motion_serial;
    sg_input_event_t motion;
    sg_input_event_t events[SG_INPUT_RING_CAPACITY];
} sg_input_ring_t;

/* Opt-in experiment control, deliberately isolated from the input ring's hot
 * cache lines. The test application advances processed_sequence only after its
 * click handler applies the action to application state. */
typedef struct {
    uint64_t magic;
    uint32_t enabled;
    uint32_t processed_sequence;
    uint32_t delivered_sequence;
    uint32_t reserved;
    uint64_t delivery_tsc;
    uint64_t action_tsc;
    uint8_t reserved2[24];
} sg_input_latency_t;

_Static_assert(sizeof(sg_input_event_t) == 32,
               "shared input event must occupy half a cache line");
_Static_assert(offsetof(sg_input_ring_t, motion) == 32,
               "coalesced motion must share the first cache line with ring control state");
_Static_assert(offsetof(sg_input_ring_t, events) == 64,
               "ordered input events must begin on a cache-line boundary");
_Static_assert(SG_INPUT_RING_OFFSET + sizeof(sg_input_ring_t) <= 4096,
               "shared input ring must fit in the communication page");
_Static_assert(sizeof(sg_input_latency_t) == 64,
               "input latency control must occupy one cache line");
_Static_assert(SG_INPUT_LATENCY_OFFSET >=
                   SG_INPUT_RING_OFFSET + sizeof(sg_input_ring_t),
               "input latency control must not overlap the input ring");
_Static_assert(SG_INPUT_LATENCY_OFFSET + sizeof(sg_input_latency_t) <= 4096,
               "input latency control must fit in the communication page");
