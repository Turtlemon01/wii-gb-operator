#include "settings.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

WiiGBSettings g_settings = { .scale_gba = 0.8f, .scale_gba_border = 1.0f, .scale_gb = 0.8f, .dev_menu = 0 };

void settings_load(void) {
    char settings_path[64];
    snprintf(settings_path, sizeof(settings_path), "%s/settings.ini", g_app_root);
    FILE *f = fopen(settings_path, "r");
    if (!f) {
        lprintf("[settings] not found — using defaults (scale_gba=%.1f scale_gba_border=%.1f scale_gb=%.1f)\n",
                (double)g_settings.scale_gba, (double)g_settings.scale_gba_border, (double)g_settings.scale_gb);
        return;
    }

    char line[128];
    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n'); if (nl) *nl = '\0';
        char *cr = strchr(line, '\r'); if (cr) *cr = '\0';
        if (line[0] == '#' || line[0] == '\0') continue;

        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line, *val = eq + 1;

        while (*key == ' ' || *key == '\t') key++;
        while (*val == ' ' || *val == '\t') val++;
        char *ke = key + strlen(key) - 1;
        while (ke > key && (*ke == ' ' || *ke == '\t')) *ke-- = '\0';

        if (strcmp(key, "dev_menu") == 0) {
            char *end;
            long v = strtol(val, &end, 10);
            if (end != val) {
                g_settings.dev_menu = (int)v;
                lprintf("[settings] dev_menu=%d\n", (int)v);
            } else {
                lprintf("[settings] dev_menu: invalid value '%s', using default\n", val);
            }
            continue;
        }

        float *target = NULL;
        if      (strcmp(key, "scale_gba")        == 0) target = &g_settings.scale_gba;
        else if (strcmp(key, "scale_gba_border") == 0) target = &g_settings.scale_gba_border;
        else if (strcmp(key, "scale_gb")         == 0) target = &g_settings.scale_gb;

        if (target) {
            char *end;
            float v = strtof(val, &end);
            if (end != val) {
                *target = v;
                lprintf("[settings] %s=%.2f\n", key, (double)v);
            } else {
                lprintf("[settings] %s: invalid value '%s', using default\n", key, val);
            }
        } else {
            lprintf("[settings] unknown key '%s' ignored\n", key);
        }
    }
    fclose(f);
}
