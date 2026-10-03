/* gcc -I bridge bridge/config.c test/test_config.c -o tc && ./tc */
#include <assert.h>
#include <stdio.h>
#include "config.h"

int main(void)
{
    struct bridge_config cfg = { 100, { 0, 0, 0x40 } };
    const char *err;

    /* Comments, blank lines, CRLF, BOM, # inside a quoted value */
    assert(config_parse("\xEF\xBB\xBF# light bar\r\n\r\nbrightness = 30  # dim\r\ncolor = \"#FF8000\" # orange\r\n",
                        &cfg, &err) == 0);
    assert(cfg.brightness == 30 && cfg.color[0] == 0xFF && cfg.color[1] == 0x80 && cfg.color[2] == 0x00);

    /* Missing keys keep their values; single quotes work */
    assert(config_parse("color='#00ff40'", &cfg, &err) == 0);
    assert(cfg.brightness == 30 && cfg.color[1] == 0xFF && cfg.color[2] == 0x40);
    assert(config_parse("", &cfg, &err) == 0);

    /* Errors report the line and leave nothing half-guessed */
    assert(config_parse("brightness = 101", &cfg, &err) == 1);
    assert(config_parse("brightness = \"50\"", &cfg, &err) == 1);
    assert(config_parse("brightness =", &cfg, &err) == 1);
    assert(config_parse("# ok\ncolor = \"#12345\"", &cfg, &err) == 2);
    assert(config_parse("color = \"#12345G\"", &cfg, &err) == 1);
    assert(config_parse("colour = \"#123456\"", &cfg, &err) == 1);
    assert(config_parse("[bridge]", &cfg, &err) == 1);
    assert(cfg.brightness == 30);

    puts("ok");
    return 0;
}
