/* Input link statistics per window: report rate, longest gap, delivery delay. Pure C, testable anywhere.
 *
 * The pad timestamps every 0x11 report (16-bit, 5.33 us ticks, SDL's unit). Host arrival time minus pad time,
 * relative to the window's fastest delivery, is how much later than necessary a report arrived (Bluetooth and
 * Windows queueing). The constant part of the latency is invisible from here. */
#pragma once
#include <stdint.h>

#define STATS_MAX_SAMPLES 4096

struct link_stats {
    int started;
    uint16_t last_tick;
    uint64_t pad_ticks, last_host_us;
    int64_t pad_base_us, last_offset_us;
    unsigned reports, bad, n;
    uint64_t max_gap_us;
    int64_t offset_us[STATS_MAX_SAMPLES];
};

struct link_window {
    unsigned reports, bad;
    double max_gap_ms, delay_avg_ms, delay_max_ms;
};

/* A valid report arrived at host_us carrying the pad's timestamp tick. */
void stats_input(struct link_stats *s, uint64_t host_us, uint16_t tick);
/* A packet failed validation. */
void stats_bad(struct link_stats *s);
/* Returns the window's figures in w and starts a new window. */
void stats_take(struct link_stats *s, struct link_window *w);
