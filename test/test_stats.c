/* gcc -I bridge bridge/stats.c test/test_stats.c -lm -o ts && ./ts */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "stats.h"

static struct link_stats s;     /* big: keep it off the stack */

int main(void)
{
    struct link_window w;

    /* Steady 4 ms reports (750 ticks of 5.33 us), delivered on time: no delay, gap 4 ms */
    uint16_t tick = 65000;      /* crosses the 16-bit wrap */
    for (int i = 0; i < 250; i++, tick += 750)
        stats_input(&s, 1000000 + i * 4000, tick);
    stats_take(&s, &w);
    assert(w.reports == 250 && w.bad == 0);
    assert(fabs(w.max_gap_ms - 4.0) < 1e-9 && w.delay_max_ms == 0 && w.delay_avg_ms == 0);

    /* One report held up 6 ms, then released together with the next: delay max 6 ms, gap 10 ms */
    uint64_t t = 1000000 + 250 * 4000;
    stats_input(&s, t, tick);
    stats_input(&s, t + 10000, tick += 750);    /* due at +4 ms, arrives +10 ms */
    stats_input(&s, t + 10000, tick += 750);    /* due at +8 ms, arrives +10 ms */
    stats_input(&s, t + 12000, tick += 750);    /* on time again */
    stats_bad(&s);
    stats_take(&s, &w);
    assert(w.reports == 4 && w.bad == 1);
    assert(fabs(w.max_gap_ms - 10.0) < 1e-9 && fabs(w.delay_max_ms - 6.0) < 1e-9);
    assert(fabs(w.delay_avg_ms - 2.0) < 1e-9);  /* (0 + 6 + 2 + 0) / 4 */

    /* After a stall longer than the tick wrap, pad time resyncs instead of producing a bogus delay */
    stats_input(&s, t + 12000 + 500000, 123);
    stats_input(&s, t + 12000 + 504000, 123 + 750);
    stats_take(&s, &w);
    assert(w.reports == 2 && fabs(w.max_gap_ms - 500.0) < 1e-9 && w.delay_max_ms == 0);

    /* Empty window */
    stats_take(&s, &w);
    assert(w.reports == 0 && w.max_gap_ms == 0 && w.delay_avg_ms == 0);

    puts("ok");
    return 0;
}
