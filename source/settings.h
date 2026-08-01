#pragma once

typedef struct {
    float scale_gba;         /* GBA display scale; 0.0 = fill screen (aspect-correct) */
    float scale_gba_border;  /* GBA display scale when a custom border is active */
    float scale_gb;          /* GB/GBC display scale */
    int   dev_menu;          /* 0 = hide dev menu (default); 1 = show dev menu */
    int   use_old_firmware;  /* 0 = new-firmware protocol (default, GB Operator v10.0.10+);
                               * 1 = old-firmware protocol (tested on v9.2.0, 2023).
                               * Manual choice only — see CLAUDE.md "Old-firmware support
                               * decision": no runtime auto-detection or mismatch prompting
                               * (tried and reverted; false-positived on a genuine "no cart"
                               * state). Also toggleable at runtime from the dev menu. */
    int   auto_detect_cart;  /* 0 = disabled (default for now — see CLAUDE.md): poll_cart()
                               * still tracks presence/absence for the UI, but a detected
                               * change never triggers the expensive, blocking
                               * run_detect_cart_inner() on its own; use manual "Detect Cart"
                               * / "Detect Cart Swap" instead. 1 = re-enable automatic
                               * re-detection on a sensed cart change. */
} WiiGBSettings;

extern WiiGBSettings g_settings;

/* Load sd:/apps/wii-gb-operator/settings.ini — call once after SD is mounted.
 * Unknown keys are ignored; missing keys use compiled-in defaults.
 * Keys: scale_gba, scale_gba_border, scale_gb, dev_menu, use_old_firmware, auto_detect_cart */
void settings_load(void);
