#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"

static char *trim(char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1]))
        *--e = '\0';
    return s;
}

int config_parse(const char *text, struct bridge_config *cfg, const char **err)
{
    char line[256];
    int no = 0;

    if (!strncmp(text, "\xEF\xBB\xBF", 3))     /* Notepad's UTF-8 BOM */
        text += 3;
    while (*text) {
        size_t len = strcspn(text, "\n");
        no++;
        if (len >= sizeof line)
            return *err = "line too long", no;
        memcpy(line, text, len);
        line[len] = '\0';
        text += len + (text[len] == '\n');

        /* Cut the comment, but not a # inside a quoted value like "#000040". */
        char quote = 0;
        for (char *p = line; *p; p++) {
            if (quote) {
                if (*p == quote)
                    quote = 0;
            } else if (*p == '"' || *p == '\'') {
                quote = *p;
            } else if (*p == '#') {
                *p = '\0';
                break;
            }
        }
        char *key = trim(line);
        if (!*key)
            continue;
        char *eq = strchr(key, '=');
        if (!eq)
            return *err = "expected key = value", no;
        *eq = '\0';
        key = trim(key);
        char *val = trim(eq + 1);

        if (!strcmp(key, "brightness")) {
            char *end;
            long v = strtol(val, &end, 10);
            if (!*val || *end || v < 0 || v > 100)
                return *err = "brightness must be a whole number 0-100", no;
            cfg->brightness = (int)v;
        } else if (!strcmp(key, "color")) {
            if (strlen(val) != 9 || (val[0] != '"' && val[0] != '\'') || val[8] != val[0] || val[1] != '#' ||
                strspn(val + 2, "0123456789abcdefABCDEF") != 6)
                return *err = "color must be \"#RRGGBB\"", no;
            unsigned long rgb = strtoul(val + 2, NULL, 16);
            cfg->color[0] = (unsigned char)(rgb >> 16);
            cfg->color[1] = (unsigned char)(rgb >> 8);
            cfg->color[2] = (unsigned char)rgb;
        } else if (!strcmp(key, "report_rate")) {
            if (!strcmp(val, "250"))
                cfg->report_interval_ms = 4;
            else if (!strcmp(val, "500"))
                cfg->report_interval_ms = 2;
            else if (!strcmp(val, "1000"))
                cfg->report_interval_ms = 1;
            else
                return *err = "report_rate must be 250, 500 or 1000", no;
        } else if (!strcmp(key, "volume")) {
            char *end;
            long v = strtol(val, &end, 10);
            if (!*val || *end || v < 0 || v > 100)
                return *err = "volume must be a whole number 0-100", no;
            cfg->volume = (int)v;
        } else if (!strcmp(key, "audio_device")) {
            size_t n = strlen(val);
            if (n < 2 || (val[0] != '"' && val[0] != '\'') || val[n - 1] != val[0] || memchr(val + 1, val[0], n - 2))
                return *err = "audio_device must be a quoted string", no;
            if (n - 2 >= sizeof cfg->audio_device)
                return *err = "audio_device is too long", no;
            memcpy(cfg->audio_device, val + 1, n - 2);
            cfg->audio_device[n - 2] = '\0';
        } else {
            return *err = "unknown key (expected brightness, color, report_rate, volume or audio_device)", no;
        }
    }
    return 0;
}
