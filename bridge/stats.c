#include "stats.h"

void stats_input(struct link_stats *s, uint64_t host_us, uint16_t tick)
{
    if (!s->started) {
        s->started = 1;
        s->pad_base_us = (int64_t)host_us;
    } else {
        uint64_t gap = host_us - s->last_host_us;
        if (gap > s->max_gap_us)
            s->max_gap_us = gap;
        if (gap >= 300000) {
            /* The tick wraps every 349 ms, so after a long gap pad time is unknown: resync it, counting the
             * resumed report as delivered no later than the previous one. */
            s->pad_ticks = 0;
            s->pad_base_us = (int64_t)host_us - s->last_offset_us;
        } else {
            s->pad_ticks += (uint16_t)(tick - s->last_tick);
        }
    }
    s->last_tick = tick;
    s->last_host_us = host_us;
    s->reports++;

    int64_t pad_us = s->pad_base_us + (int64_t)(s->pad_ticks * 16 / 3);
    s->last_offset_us = (int64_t)host_us - pad_us;
    if (s->n < STATS_MAX_SAMPLES)
        s->offset_us[s->n++] = s->last_offset_us;
}

void stats_bad(struct link_stats *s)
{
    s->bad++;
}

void stats_take(struct link_stats *s, struct link_window *w)
{
    int64_t min = 0, max = 0, sum = 0;
    for (unsigned i = 0; i < s->n; i++) {
        if (i == 0 || s->offset_us[i] < min)
            min = s->offset_us[i];
        if (i == 0 || s->offset_us[i] > max)
            max = s->offset_us[i];
        sum += s->offset_us[i];
    }
    w->reports = s->reports;
    w->bad = s->bad;
    w->max_gap_ms = s->max_gap_us / 1000.0;
    w->delay_avg_ms = s->n ? ((double)sum / s->n - (double)min) / 1000.0 : 0;
    w->delay_max_ms = s->n ? (max - min) / 1000.0 : 0;
    s->reports = s->bad = s->n = 0;
    s->max_gap_us = 0;
}
