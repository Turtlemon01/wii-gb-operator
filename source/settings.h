#pragma once

typedef struct {
    float scale_gba;         /* GBA display scale; 0.0 = fill screen (aspect-correct) */
    float scale_gba_border;  /* GBA display scale when a custom border is active */
    float scale_gb;          /* GB/GBC display scale */
    int   dev_menu;          /* 0 = hide dev menu (default); 1 = show dev menu */
} WiiGBSettings;

extern WiiGBSettings g_settings;

/* Load sd:/apps/wii-gb-operator/settings.ini — call once after SD is mounted.
 * Unknown keys are ignored; missing keys use compiled-in defaults.
 * Keys: scale_gba, scale_gba_border, scale_gb, dev_menu */
void settings_load(void);
