/* ds4bridge settings from config.toml. Pure C, so it builds in the test on any OS. */
#pragma once

struct bridge_config {
    int brightness;             /* light bar, 0-100 */
    unsigned char color[3];     /* light bar RGB until a game sets one */
    int report_interval_ms;     /* 1/2/4 (1000/500/250 Hz), 0 = leave the pad's default */
};

/* Parses the flat subset of TOML the bridge uses: `key = value` lines and # comments.
 * Keys absent from text keep their values in cfg. Returns 0, or the 1-based line of the first error
 * with *err describing it; unknown keys and anything else are errors, not silently ignored. */
int config_parse(const char *text, struct bridge_config *cfg, const char **err);
