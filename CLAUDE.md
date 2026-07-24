# wii-gb-operator

All-in-one Wii homebrew app that reads GB/GBC/GBA cartridges from the Epilogue GB Operator over USB, dumps ROMs to storage, and launches them in an integrated mGBA emulator — similar in concept to Epilogue Playback but running natively on Wii.

## Project Scope

1. **Cart reading**: User inserts a GB/GBC/GBA cart into the GB Operator connected to the Wii USB port. The app detects the device, reads the cart header, and identifies the game.
2. **ROM caching**: On first play, the full ROM is transferred to SD or USB storage. On subsequent plays, the cached ROM is used — only the save data is re-read from the cart.
3. **Save sync**: Save data is read from the cart at launch and written back during play. GBA: automatic background sync on every in-game save event (real-time, no player action needed). GB/GBC: player-initiated sync via OSD overlay (SELECT button → Sync to Cart), with automatic detection confirming an in-game save occurred before allowing the sync.
4. **Emulation**: mGBA is compiled into the same binary. After ROM + save are ready, the app boots the game directly in mGBA.
5. **Storage preference**: SD vs USB is a user-accessible setting.

### Future Plans
- Pokémon Box-style save management UI
- Link cable support: DOL-011 JoyBus path (GC↔GBA games) and Wi-Fi software link (general GBA/GB trading) — see Phase 4 item 5

### Phase 4 Plans (implementation order)
1. **Settings file** ✓ COMPLETE — `sd:/apps/wii-gb-operator/settings.ini` (plain-text key=value). Parsed by `source/settings.c`; loaded once after SD mount. Keys: `scale_gba` (float, GBA display scale), `scale_gb` (float, GB/GBC display scale), `dev_menu` (int: 0=hidden, 1=show Developer Menu in frontend). Copy `settings.ini` from repo root to SD card to apply.
2. **GB/C manual sync only** — remove auto-save detection for GB/GBC; replace with manual "Sync to Cart" workflow only. OSD exit prompt asks whether to sync before leaving. Player can choose not to sync to reset to last save point. GBA auto-sync unchanged. Simplifies existing code before adding new features.
3. **GBA borders** ✓ COMPLETE — composite a decorative border frame around the GBA game. Canvas size: **320×240** (4:3, 40px margin all around the 240×160 game area). File format: **8-bit indexed, 24-bit, or 32-bit uncompressed BMP, exactly 320×240**, stored in `sd:/apps/wii-gb-operator/borders/gba/border_<4-char-game-code>.bmp`. Game renders at pixel offset (40, 40) within the canvas. Scale setting `scale_gba_border` (default 1.0) is used when a border is loaded; `scale_gba` (default 0.8) is used when no border file is present. 8bpp indexed format confirmed working with Aseprite exports (test_118). Example filenames: `border_BPRE.bmp` (FireRed), `border_AXVE.bmp` (Sapphire).
4. **Save sync animation icons** — replace the 8×8 colored dot with a 2-frame animated sprite. File format: **32bpp BGRA uncompressed BMP (BI_BITFIELDS, compression=3, or comp=0), exactly 16×16 pixels**, three files in `sd:/apps/wii-gb-operator/icons/`: `sync_0.bmp` (frame 0 — floppy disk at rest), `sync_1.bmp` (frame 1 — floppy disk slightly offset, creates bobbing animation), `sync_done.bmp` (floppy with checkmark, shown on sync success). Alpha channel used directly (A=0 = transparent pixel skipped). Frames alternate every ~30 frames while sync is in progress. GBA only (GBC uses OSD manual sync). Pixel data offset 70 for BI_BITFIELDS BMPs (Aseprite export format). Fallback to 8×8 colored dot when files absent.
5. **Link cable support** *(DEFERRED — research pinned for future implementation)* — three sub-tracks, all long-term:
   - **Colosseum/XD middleman** *(highest priority sub-track)*: Gen 3 Pokémon trading between the emulated GBA game (mGBA, on Wii) and a physical GBA handheld connected via DOL-011 cable. The Wii acts as a fake Colosseum/XD GCN host: it sends the `colosseum-mb` multiboot ROM to the physical GBA via JoyBus (`SI_Transfer`), receives the physical GBA's party data, presents a trade OSD on the TV, and brokers the swap. **The physical GBA never runs its retail game during the trade** — colosseum-mb runs from GBA RAM, reads the physical cart save directly, shows its own trade UI on the GBA screen, and writes the received Pokémon back to the cart save. The mGBA player uses the GC controller on the TV OSD; the physical GBA player uses their GBA buttons (transmitted back to the Wii via `LINK_CMD_READ_INPUT`). No physical cart swapping; no Cable Club; both players interact simultaneously. **Rejected alternative**: a "save-swap" approach (cart physically inserted into GB Operator, save read/written over USB, no cable) was considered but rejected — it requires swapping the cartridge out of the GBA handheld and doesn't feel like a trade. **DOL-011 hardware note**: the cable carries only the SD/JoyBus pin — NOT SO, SI, or SC (GBA multiplayer SIO pins). Full GBA-to-GBA link cable (trading, battling) is physically impossible over DOL-011; the Colosseum middleman works only because colosseum-mb uses JoyBus, not SIO. **Key rules enforced by the middleman** (from pret/pokefirered `CanTradeSelectedMon()`): party must have ≥2 non-egg Pokémon after trade; Mew/Deoxys require MODERN_FATEFUL_ENCOUNTER (bit 31 of Misc Ribbons/Obedience u32); FRLG requires `FLAG_SYS_CAN_LINK_WITH_RS` (0x844); Egg trading requires both players to have `FLAG_SYS_NATIONAL_DEX` (0x840); Pokémon holding mail cannot trade; Enigma Berry blocks trade entirely. **Game-of-origin note**: traded Pokémon should have bits 7–10 of Origins Info set to 15 (Colosseum/XD) to explain why they crossed between GBA games without normal link-cable validation. **Save format**: party in Section 1 at offset 0x0038 (FRLG) or 0x0238 (RSE); 100-byte Pokémon slots × 6; 48-byte block XOR-encrypted with full OT ID u32; per-Pokémon checksum = sum of 24 u16 words in decrypted block (low 16 bits); per-section checksum = sum all u32 words in section data, add upper+lower u16 halves, take low 16 bits. **Protocol status — what is known**: GBA-side fully decompiled (pret/colosseum-mb); multiboot send on Wii already implemented (Wack0/gba-gen3multiboot); JoyBus transport layer known (GBATEK + Dolphin SI_DeviceGBA); LINK_CMD_* function names and behavior known from pret/colosseum-mb `payload/src/unk_200C5DC.c`. **Protocol status — what is unknown**: exact LINK_CMD_* byte values; GCN-side command send sequence and state machine (no Colosseum decomp exists). **One-session fix**: run Dolphin + Colosseum/XD ISO + mGBA connected via JoyBus TCP (Dolphin settings → GBA), capture port 54970 in Wireshark, complete one trade — this gives every GCN-side byte value and ordering needed to implement the middleman.
   - **DOL-011 JoyBus path**: The GameCube-to-GBA cable (DOL-011) connects only the SD pin (JoyBus protocol). It does NOT carry standard GBA multiplayer SIO (SO/SI/SC) and cannot be used for GBA-to-GBA trading. It IS usable for games that explicitly support GC↔GBA connectivity (FireRed/LeafGreen ↔ Pokémon Box/Colosseum, e-Reader). Implementation: adapt `vendor/mgba/src/gba/sio/dolphin.c` — a JoyBus SIO driver that currently uses TCP sockets for Dolphin emulator — replacing TCP with libogc `SI_Transfer` calls. Register via `GBASIOSetDriver(&gba->sio, &driver, SIO_JOYBUS)`.
   - **Wi-Fi software link**: For real-time GBA-to-GBA and GB/GBC-to-GB/GBC link cable (trading, battling, union room, co-op), the correct approach is software-to-software over Wii Wi-Fi. Two Wiis each running wii-gb-operator link their mGBA instances via libogc TCP sockets, using mGBA's `GBASIOLockstepDriver` (GBA) and `GBSIODriver` (GB/GBC) for byte-level synchronization. Covers all game types, no extra hardware required.
   - Implement after all other Phase 4 items are complete and stable.

---

## Current Status

**Phase 1 (COMPLETE):** USB communication verified. Cart info reads successfully on Wii hardware.
**Phase 2 (COMPLETE):** GB/GBC/GBA ROM dump, save dump, and save upload all working on hardware. GBA Flash write+verify confirmed reliable (test_51).
**Phase 3 (COMPLETE):** mGBA integration — emulator vendored in-tree, integrated into the same binary, with background save-to-cart sync.
**Phase 4 (IN PROGRESS):** Polish and additional features — settings file, GBA borders, save sync icons, GB/C manual-sync workflow.

### What is complete
- `source/rom_cache.c` — fully implemented, do not refactor
- `source/main.c` — orchestration loop, SD mount, SD file logging to `sd:/apps/wii-gb-operator/log.txt`; cart poll loop with auto-detect, enriched title display, IOS USB fd-spend recovery (fresh-fd wait, final drain after ROM dump)
- `source/log.h` — shared `lprintf()` that writes to both TV console and log file
- `source/gametitles.c` — enriched title lookup by game code (GBA), CGB code (GBC), or header title string (GB); region suffix from ROM header
- `source/cartindex.c` — fingerprint-based persistent cache mapping device fingerprints to game codes; survives across sessions on SD
- `source/settings.c` — `sd:/apps/wii-gb-operator/settings.ini` parser; keys: `scale_gba`, `scale_gba_border`, `scale_gb`, `dev_menu`
- Build system (CMake + DevKitPro), VSCode integration
- USB device enumeration — GB Operator confirmed visible at VID `0x16D0` PID `0x123D`
- `gbop_read_cart_info()` — fully working; GB/GBC/GBA fields parsed
- `gbop_dump_rom()` — ROM dump working; GBA 16MB confirmed bit-perfect; IOS EP IN buffer drained on completion (test_122)
- `gbop_read_save()` — fully working; GBA Flash and GB/GBC SRAM confirmed; bit-perfect vs Epilogue Playback
- `gbop_write_save()` — GBA Flash and GBC SRAM write+verify confirmed (test_51); 30s GBA verify delay
- `source/mgba_frontend.c` — emulation loop, GX video, AESND audio, GC input, save hook, OSD overlay, border loading, cart sync integration; all working on hardware
- `source/cart_sync.c` — background LWP thread, retry loop, 10s cooldown, success callback; all working on hardware
- Successive-session stability — `core->deinit()` skipped in all paths; `while(1)` loop in `main()` handles successive sessions without heap corruption (confirmed test_90/91/122)
- GBA auto-sync: `savedataUpdated` threshold=1, save indicator dot, background cart write with retry
- GBC/GB manual sync: OSD "Sync to Cart" with guards; stable-baseline diff ≥512 bytes gates the sync item; no save indicator dot
- OSD overlay (SELECT button): ROM browser, save browser, sync control, quit control; unsaved-but-unsynced exit warning for GB/C
- GBA border system: `sd:/apps/wii-gb-operator/borders/gba/border_<CODE>.bmp`; 8/24/32bpp BMP; scale_gba_border setting; pure GB uses mGBA SGB borders
- GX double-buffer rendering; `g_gx_initialized` guard prevents double `GX_Init` on successive sessions

### What is in progress (Phase 4)
- **Save sync animation icons** — replace 8×8 colored dot with 2-frame animated floppy-disk sprite (16×16 32bpp BGRA BMP). Files: `sync_0.bmp`, `sync_1.bmp`, `sync_done.bmp` in `sd:/apps/wii-gb-operator/icons/`. Frames alternate every ~30 frames during sync; done icon shown on success. GBA only. Code implemented in `source/mgba_frontend.c` (`load_icon_bmp`, `blit_icon`, `draw_save_indicator`); needs hardware test.
- **GC→GBA link cable** — long-term; implement after all other Phase 4 items complete

### Hardware test findings (2026-06-13)
- GB Operator power LED confirmed on when connected to Wii USB
- USB_GetDeviceList ignores class filter — returns all 3 interfaces for any class value
- GB Operator: 3 USB interface entries, EP OUT=`0x01`, EP IN=`0x81` confirmed working
- Confirmed VID/PID: `0x16D0:0x123D` — web research was wrong (`0x1D50:0x6018`)
- Commands require CRC32-MPEG2 in bytes 60-63; without it device returns all zeros
- CRC32-MPEG2 verified correct (check value 0x0376E6E7 passes)
- IOS USB DMA requires DCFlushRange before writes and DCInvalidateRange before reads
- Must open interface [1] (CDC Data, device_id index +1) for bulk transfers, not [0]
- Full command exchange (hardware verified): send 64B → ACK (two short USB packets: 60B + 4B, separate) → 2 × 64B data packets (single USB packet each, not 60+4 separate)
- Cart info lives in the first 60 bytes of data chunk 0 (not in the ACK)
- ACK uses 60+4 split (two real USB packets); data chunks use 64-byte single packets — must read data chunks with `USB_ReadBlkMsg(64)`, not `USB_ReadBlkMsg(60)` + `USB_ReadBlkMsg(4)`
- -7008 is the IOS USB timeout error code returned when `USB_ReadBlkMsg(4)` finds no pending 4-byte USB packet (e.g. after truncating a 64-byte data packet to 60 bytes)
- 60-byte `USB_ReadBlkMsg` reads hang indefinitely if no USB packet arrives; 4-byte reads return -7008 on timeout (asymmetric timeout behavior in IOS)
- ROM dump command (0x00) ACK is a 60-byte short USB packet (all zeros), followed by an optional 4-byte trailing packet — read with `USB_ReadBlkMsg(60)` + `USB_ReadBlkMsg(4)` (same pattern as command ACK); the 4-byte read may return -7008 if no trailing packet
- After ROM dump command ACK: host MUST send 64 zero bytes (host ACK) before device starts streaming. Without it device returns -7005 on first ROM data read. gbopyrator sends this host ACK before EVERY 320-chunk batch, starting with batch 0
- **CORRECTED (2026-06-13 test #3):** In-stream ACK device response: **NONE**. The device does NOT respond to in-stream host ACKs — it continues ROM streaming immediately. Our `USB_ReadBlkMsg(60)` for the "ACK response" was silently consuming 60 bytes of real ROM data at every 320-chunk boundary. Must NOT read after in-stream ACKs.
- **Initial host ACK device response (2026-06-13 test #3):** device sends 84 bytes across 4 USB packets: 16 + 4 + 60 + 4 bytes (all zeros). First 16 bytes are read by `USB_ReadBlkMsg(60)` → rd=16 (short packet); subsequent `USB_ReadBlkMsg(4)` → -7008 (IOS 4-byte timeout fires before 4-byte packet arrives). The remaining 3 USB packets (4+60+4=68 bytes) then sit in the IOS receive queue and are consumed by the first 3 streaming reads as `rd=4`, `rd=60`, `rd=4` (all zeros). These must be explicitly drained before ROM data accounting starts; otherwise the file begins with 68 bytes of zeros instead of the ROM header.
- **ROM data packet size (2026-06-13 test #3):** device sends ROM data as **60-byte USB short packets** (not 64-byte full packets). `USB_ReadBlkMsg(64)` returns rd=60. First valid ROM data appears at what is logically chunk[4] (after 3 setup/drain packets). Chunk[4] confirmed to start with GBA ROM entry branch + Nintendo logo bytes (`7F 00 00 EA 24 FF AE 51`).
- **Stolen bytes hang (2026-06-13 test #3):** with old code, each in-stream ACK read consumed 60 bytes of ROM data without crediting them to `dump_given`. After 873 in-stream ACKs × 60 bytes = ~52,380 bytes stolen, `dump_given` undershot `dump_total` by ~52 KB at end of stream. Loop then blocked on `USB_ReadBlkMsg(64)` waiting for data the device had already sent. Dump reached ~16,328 KB / 16,384 KB before hanging.
- **Abort button limitation:** X+Y PAD check in `rom_cache.c` cannot interrupt a blocked `USB_ReadBlkMsg` call inside `gbop_dump_rom`. Fix is correct byte accounting so the hang never occurs.
- resp[2]=0x20 → GB/GBC; resp[2]=0x30 → GBA confirmed on hardware with GBA cart inserted
- **2026-06-14 test #4 (GBA FireRed dump attempt):**
  - drain[0..2] confirmed correct (rd=4, rd=60, rd=4, all zeros) — drain fix from test #3 working ✓
  - chunk[1] rd=60: GBA ROM header `7F 00 00 EA 24 FF AE 51` at correct offset — no file-prefix zeros ✓
  - chunk[5] rd=16: `FF FF FF FF FF FF FF FF` — anomalous 16-byte short USB packet mid-stream; likely GBA ROM padding region (all-0xFF filler); device sends 16-byte USB short packet here instead of 60-byte
  - Dump progressed 0%→15% (0–2600 KB / 16384 KB) across 163 in-stream ACKs without error
  - **At ACK@52160 (batch 163):** `USB_WriteBlkMsg(64 zeros)` returned w=**-7005** — device rejected the OUT transfer
  - Subsequent `USB_ReadBlkMsg(64)` also returned rd=**-7005** — EP IN error or device stall
  - Dump aborted at offset 0x0028A000 (2600 KB); partial file kept at `sd:/apps/wii-gb-operator/roms/PBPR0_.gba`
  - **Root cause: UNKNOWN.** Hypotheses: (1) GBA does not need in-stream ACKs at all (different streaming protocol from GB/GBC); (2) 16-byte short packet at chunk[5] desynchronized something in the device; (3) device stalled EP IN and EP OUT after some internal error at byte offset ~2600 KB
  - **Next test: skip in-stream ACKs for GBA** — if dump completes, hypothesis (1) is confirmed. If dump hangs after the first missing ACK (chunk 320), GBA does need ACKs.
  - -7005 semantics: IOS returns -7005 when the USB device sends a STALL handshake on the endpoint, or when IOS itself rejects the transfer. Distinct from -7008 (IOS read timeout for 4-byte reads).
- **2026-06-14 test #5 (GBA — skip in-stream ACKs):**
  - Code change: in-stream ACKs skipped for GBA (hypothesis: GBA doesn't need them)
  - `ACK@320: given=16384 written=0 total=16777216 (0%)` → `ACK skipped (GBA: testing no-ACK mode)`
  - Immediate result: `ROM chunk read error: -7005 at chunk 320`
  - **Conclusion: GBA DOES need in-stream ACKs — hypothesis 1 is FALSE.** Without the ACK at chunk 320, the device stalls EP IN immediately.
  - The -7005 at batch 163 (test #4) is therefore NOT caused by the device not wanting ACKs. Root cause is still unknown but is NOT "GBA doesn't need ACKs."
  - **Next test: restore in-stream ACKs for GBA. When ACK write returns -7005 (at batch 163), attempt USB CLEAR_FEATURE on EP IN and EP OUT, then retry the ACK. Log all short packets (rd < 60) throughout the dump.**
- **2026-06-14 test #6 (GBA — stall clear + short packet logging):**
  - Short packets (rd=16) appear at **every 5th chunk** throughout the GBA ROM stream (chunks 5, 10, 15, ..., 320, ..., 52157). This is a structural device behavior for GBA streaming.
  - After `ACK@51840` (w=64, success): chunk 51841=rd60 (normal ROM), chunk 51842=**rd=4 [00 00 00 00]** (device in-stream ACK response), then ROM resumes with the 5-cycle re-anchored to 51847 (51842+5).
  - After `ACK@52160`: w=-7005 (device rejects ACK). Stall clear + retry also returned -7005. Next read returned rd=-4. Dump failed.
  - **Root cause confirmed:** device sends a 4-byte all-zeros USB packet as an in-stream ACK response, interleaved ~2 reads after each in-stream ACK. Consuming this as a ROM chunk and counting it in `dump_chunk_cnt` causes a 1-packet drift per ACK. After 162 ACKs, batch 163's ACK fires 1 read before the device's batch boundary → device rejects ACK with -7005.
  - **Fix implemented:** detect `rd==4` all-zeros in streaming loop, discard without incrementing `dump_chunk_cnt`, re-read next real ROM chunk. Stall-clear code removed (wrong diagnosis).
- **2026-06-14 test #7 (GBA — rd==4 zeros discard):**
  - ACK@320 (batch 1): response discarded at position 322 (rd=4 zeros) ✓
  - ACK@103360 (batch 323): first read after ACK was rd=16 (short chunk 103361), THEN response discarded at position 103363 (rd=4 zeros) ✓. Response position varies based on whether first post-ACK chunk is rd=60 (response at +2) or rd=16 (response at +3).
  - ACK@103680 (batch 324): w=-7005 — dump failed at same symptom.
  - Analysis: 103680/320 = 324 = 2×162, exactly twice the original failure batch. The fix caught ~50% of responses, halving the drift rate from 1 per ACK to 0.5 per ACK, doubling the run before failure. The `rd==4 && all-zeros` check is too strict — the response may sometimes carry non-zero content. ROM data is exclusively rd=60 or rd=16; any rd<16 packet is unambiguously a device response.
  - **Fix (test #8):** Changed discard condition from `rd==4 && all-zeros` to `while (rd > 0 && rd < 16)` — catches all response packets regardless of content, drains multiple consecutive responses if any.
- **2026-06-14 test #8 (GBA — while rd<16 drain):**
  - ACK@320 (batch 1): response discarded at chunk 322 (rd=4 zeros) ✓ — same as test #7
  - ACK@9920 (batch 31): response discarded at chunk 9952 (rd=4 zeros) ✓ — but position is +32, not +2
  - Position drift of +30 by batch 31 means ~30 intermediate ACK responses (batches 2-30) were NOT drained. Since `while (rd < 16)` catches rd=4 correctly, missed responses must be rd≥16. **Most likely hypothesis: the in-stream ACK response is 2 USB packets: rd=4 (caught) + rd=16 (NOT caught, counted as ROM data, causing 1-packet drift per ACK).**
  - Log ends at "ROM dump: 512 KB / 16384 KB (3%)" with no subsequent failure entry.
  - **Root cause of missing failure log:** `g_log` was closed and reopened with `fclose(g_log); g_log = fopen("sd:/apps/wii-gb-operator/log.txt", "a")` at the 512 KB progress mark. If `fopen` returns NULL, `g_log = NULL` → all subsequent `lprintf` output goes to TV console only, not the log file. Failure DID occur (visible on TV) but was not committed to the file.
  - **Fixes (test #9):** (1) Replace `fclose/fopen` with `fflush` only — prevents `g_log` from becoming NULL, ensures all failures are logged; (2) Added ACK write error check (abort on `wack2 < 0`); (3) Added `post_ack_dbg` field to device struct — logs all 10 USB reads after each in-stream ACK verbosely (`post-ACK[1..10]`) to capture the full response sequence and confirm 1-packet vs 2-packet response.
- **2026-06-14 test #9 (GBA — fflush fix + ACK error check + post-ACK[1..10] verbose logging):**
  - Log fix confirmed: failure now fully captured in log file. ACK error check confirmed: clean abort on `wack2 < 0`.
  - post-ACK sequence at batch 1 (ACK@320): `post-ACK[1]` rd=60, `post-ACK[2]` rd=4 zeros (response, drained), `post-ACK[3]` rd=60 (after drain). Response at position +2.
  - post-ACK sequence at batch 323 (ACK@103360): `post-ACK[1]` rd=16 (non-zero ROM data, chunk 103361), `post-ACK[2]` rd=60, `post-ACK[3]` rd=4 zeros (response, drained), `post-ACK[4]` rd=60 (after drain). Response at position +3.
  - **Failure: `ACK@103680 w=-7005`** — batch 324, exactly the same batch as test #7. Drift still ~50%.
  - **Root cause confirmed: ~50% of in-stream ACK responses are rd=16**, not caught by `while (rd < 16)`. These rd=16 response packets are the same size as rd=16 ROM short packets (every 5th chunk). The while loop catches rd=4 responses (and eliminates 50% of drift) but is blind to rd=16 responses. Both test #7 and test #9 fail at batch 324 = 2× the original batch 163, consistent with 0.5 drift/batch.
  - The `post-ACK[1]` rd=16 at batch 323 was non-zero ROM content (EA EA E7 E3), NOT a response. So rd=16 responses appear at the same position as legitimate rd=16 ROM data, making them indistinguishable by size alone.
  - **Pivoted to save data dump for new diagnostic info.** Save command (0x02) implementation added; streaming protocol for saves is unknown — test will reveal whether saves use the same 320-chunk ACK cycle, same packet sizes, and same handshake as ROM.
- **2026-06-14 save dump test #1 (command 0x02 — initial attempt):**
  - `USB_WriteBlkMsg` for save command returned **w=-4** immediately. Cart info command (sent right after USB_OpenDevice) worked fine; save command (sent seconds later after menu navigation) failed.
  - **Root cause: IOS USB handle goes stale after idle.** The USB write handle is valid immediately after USB_OpenDevice but becomes invalid if several seconds pass without a USB transaction. `-4` is IOS_EMAX or similar IOS-internal error for an invalidated handle.
  - **Fix:** Close device with `gbop_close()` BEFORE showing the menu, then call `gbop_find()` again immediately before the selected operation. This ensures the USB handle is fresh when any command is sent. The menu no longer holds the handle open.
- **2026-06-14 GBA dump — byte-based ACK (current code):**
  - Switched from chunk-count ACK (drifts due to uncaught rd=16 responses) to **byte-count ACK**: host ACK fires when `dump_rx_bytes` crosses each 16384-byte batch boundary.
  - Each batch = 256×rd=60 + 64×rd=16 = exactly 16384 bytes. rd=4 device responses are drained without adding to `dump_rx_bytes`.
  - Result: dump progresses to **batch 277 (4432 KB / 16384 KB) before failing with w=-7005** on the ACK write. Deterministic across multiple runs.
  - Verbose per-chunk logging (batches 275-277 logged every chunk to SD via lprintf+fflush) confirmed stream is clean through batch 277: all chunks normal rd=60/rd=16, no extra packets. ACK#277 fires at cnt=88593, last_rd=60. ACK write returns -7005 immediately.
  - **Root cause UNKNOWN.** The device stalls EP OUT at exactly the 277th in-stream ACK with no observable cause in the stream data or timing. Not related to ROM content at that offset.
- **2026-06-14 GB/GBC (Pokemon Gold) dump attempt — first test:**
  - **FAIL: `ROM chunk read error: -7005 at chunk 317`**
  - GB/GBC ROM dump handshake is DIFFERENT from GBA:

  | | Command ACK r4 | Host ACK resp r60 | Host ACK resp r4 | Extra drain |
  |--|--|--|--|--|
  | GBA | -7008 (timeout) | rd=16 (short) | -7008 (timeout) | 3 packets (rd=4+rd=60+rd=4) |
  | GB/GBC | rd=4 (real packet) | rd=60 (full) | rd=4 (real packet) | **NONE** |

  - For GB/GBC, ROM streaming starts immediately after the 60+4 byte host ACK response. Our drain loop unconditionally reads 3 more `USB_ReadBlkMsg(64)` calls, consuming the first 3×60=180 bytes of the GB ROM as if they were handshake zeros.
  - This shifts our chunk count 3 behind the device. At device chunk 320 (first batch boundary), we are at our chunk 317. Device stalls EP IN waiting for an undelivered ACK → read returns -7005.
  - **drain[0] data confirmed (`F3 C3 00 01`)**: matches the first bytes of the Pokemon Gold ROM (DI; JP $0100 entry), proving the drain was consuming real ROM.
  - **Fix (implemented):** drain loop is now conditional on `r1 == 16`. After `gbop_close`/`gbop_reopen`, device always uses GBA-style handshake (r1=16, r4=-7008, then 3-drain packets). The different handshake in the earlier GBC test (r1=60) was a stale-handle artifact.
  - **GB/GBC rd=16 pattern**: rd=16 at chunks 5, 10, 15, ... (same as GBA). The earlier "(5k+2) for GBC" note was wrong — derived from the old test where drain consumed ROM bytes and shifted chunk numbering.
- **2026-06-14 GBC dump success (Pokemon Gold):**
  - Drain fix confirmed working. Dump completed 100%, 2,097,152 bytes, 40938 chunks.
  - **ROM does not play in mGBA on PC.** Extension is `.gb`; working reference is `.gbc`. Root cause: `gbop_read_cart_info` always sets type to `CART_TYPE_GB` for resp[2]=0x20 carts — no GBC detection. CGB flag location in resp[] unknown; next test logs all 60 resp bytes to identify it.
  - Title parsed as "P" only: resp[14] is the MBC type byte (non-printable), stopping the title loop after 1 char. Device response has only 1 title char at resp[13].
- **2026-06-14 GBA dump — batch-277 stall root cause identified:**
  - `post-fail probe: rd=-7005` — both EP IN and EP OUT stalled simultaneously. Confirms hard device error state on ACK rejection.
  - Root cause: uncaught rd=16 response packets cause byte drift. Each uncaught rd=16 adds 16 bytes to `dump_rx_bytes`, firing ACK 16 bytes early per batch. After 277 batches × 16 = 4,432 bytes cumulative drift, ACK fires ~4 KB before the device's batch boundary → stall.
  - **Fix (implemented):** rd=16 ROM chunks appear ONLY at every-5th-chunk positions (dump_chunk_cnt+1 ≡ 0 mod 5). rd=16 at any other position is a response. Drain condition: `while (rd < 16 || (rd == 16 && (dump_chunk_cnt+1) % 5 != 0))`. All drained responses logged.
- **2026-06-15 GBA dump — drain[0] corruption root cause identified (binary comparison):**
  - Dump completed 100% (16MB, no errors) in tests #12+, but dump produced white screen in mGBA for both US FireRed (BPRE) and JP FireRed (BPRJ).
  - Binary comparison of BPRJ dump against a known-good reference ROM (confirmed working in mGBA): first difference at byte `0x4000` (exactly 16,384 bytes = first in-stream ACK boundary).
  - Dump has 60 zero bytes at offset 0x4000; reference has valid game data. Dump's game data (matching reference offset 0x4000) appears 60 bytes later at dump offset 0x403C.
  - **Root cause:** `drain[0]` in the deferred-drain loop read `rd=60` (all zeros — device ACK handshake response) and, since `60 >= 16`, wrote it to the ROM file as if it were ROM data. This injected 60 corrupt zero bytes at every 16KB ACK boundary.
  - With 1023 in-stream ACKs per 16MB dump: 1023 × 60 = 61,380 corrupt zero bytes scattered through the file (every 16KB), causing game crashes on any 16KB-aligned code/data.
  - **Correct in-stream ACK response sequence (confirmed from binary):** device sends `rd=60 all-zeros` (drain[0]) then `rd=4 all-zeros` (drain[1]) before resuming ROM streaming. BOTH are device protocol overhead, never ROM data.
  - **Fix:** drain loop now always discards both reads regardless of size (removed `if (dr >= 16) { COPY_TO_BUF }` branch). Both drain reads logged for verification. Next hardware test should produce a bit-perfect dump.
- **Save write test #18 (known-good save source, all 547 chunk ACKs received, game showed new game):**
  - Source file `P.sav` first 4 bytes: `00 00 00 10` (confirmed correct known-good save). All 547 per-chunk ACKs returned rd=4, retries=2. Write reported complete.
  - After hard power-off, game showed "new game" screen — SRAM was not written.
  - **CRC hypothesis:** data packets (command 0x03 write loop) were sent without CRC (bytes 60-63 = zeros). Device may ACK each 64-byte OUT packet for USB flow control but silently discard packets with bad CRC without writing to SRAM. Fix added: call `gbop_make_pkt()` on each data payload before sending, appending CRC32-MPEG2 in bytes 60-63.
  - **Alternative hypothesis (unresolved):** hard power-off may have interrupted SRAM programming. Verify read after write added to distinguish: if read-back matches source immediately after write (before power-off), SRAM was written.
- **Save write test #19 (CRC fix + verify read — cmd=0x03 tx=-4):**
  - Source file `P.sav` first 4 bytes: `11 12 1E 1F` (different from test 18; likely a different reference save or a new dump from the cart).
  - **`[gbop] cmd=0x03 tx=-4` — write COMMAND failed before any data was sent.**
  - Root cause: `upload_save_to_cart` in `main.c` read the save file from SD (32KB `fread`) AFTER `gbop_reopen()` but BEFORE the first `USB_WriteBlkMsg`. The SD I/O between `USB_OpenDevice` and `USB_WriteBlkMsg` invalidates the IOS USB handle (-4 = IOS_EMAX / stale handle). This is distinct from the menu-idle stale-handle bug (previously fixed) — here the handle is fresh but SD I/O corrupts its state.
  - **Fix:** pre-load the save file from SD BEFORE calling `gbop_reopen()`. The file read, size check, confirmation dialog, and malloc all happen while USB is closed. Only after SD operations complete is the USB device reopened, with the first USB transfer immediately following. Implemented by splitting `upload_save_to_cart` into `load_save_from_sd` (SD ops, before reopen) and `write_save_to_cart` (USB ops, after reopen).
  - **CRC hypothesis still unconfirmed** — test 19 failed at command level before any data packets.
  - **Exit button change:** Wiimote HOME button never worked (user uses GC controller). Exit loop and "Press HOME to exit" prompt changed to `PAD_ButtonsDown(0) & PAD_BUTTON_START` / "Press START to exit."
- **Save write test #20 (SD/USB ordering fix — verify read hangs, game shows new game):**
  - SD/USB ordering fix confirmed: `cmd=0x03 tx=-4` gone. Write command succeeded.
  - Verify read (`gbop_read_save` on same handle after `gbop_write_save`) caused hang: `USB_ReadBlkMsg(60)` for command ACK blocks indefinitely — device not ready for new command without USB close/reopen between operations. Program stuck; never reached exit loop; user power-cycled.
  - Game showed "new game" — write may have been interrupted by forced power-off during verify hang.
  - **CRC on data packets hypothesis**: unconfirmed, may have caused write to not commit.
  - **Fix for verify hang**: removed inline verify; added post-upload close/reopen + verify read with fresh handle.
  - **Fix for data packet format**: switched data chunks from 60 bytes payload + 4 bytes CRC to 64 bytes raw save data (hypothesis: device writes all 64 bytes per packet to SRAM; 60-byte chunks would interleave 4 garbage bytes every 60 save bytes, corrupting the file).
  - **Exit button**: `PAD_ButtonsDown` failed to detect START in the exit loop. Changed to `PAD_ButtonsHeld` (hold to exit) and moved `PAD_ScanPads` to before `VIDEO_WaitVSync`.
- **Save write test #21 (64-byte data chunks, game still shows new game):**
  - `Write ready resp: r60=16 r4=-7008` (changed from r60=60 in test 18, due to new open timing).
  - `write drain: rd=4` consumed correctly.
  - 512 chunks × 64 bytes = 32768 bytes, all ACKs rd=4 retries=2. Write protocol complete.
  - `write final resp: rd=-7008` (no final device packet).
  - Program reached exit loop ("Press START to exit." in log). `PAD_ButtonsHeld` also did not exit — likely requires further investigation.
  - Game showed "new game" after upload. 64-byte chunks alone do not fix the write.
  - **Root cause unknown**: device accepts all chunks and ACKs, but SRAM not confirmed written. Two remaining hypotheses: (A) device writes all 64 bytes per chunk to SRAM correctly but the save file (`11 12 1E 1F`) is invalid for this cartridge; (B) device does not write to SRAM at all despite sending per-chunk ACKs.
  - **Next test: add verify read after close/reopen** (test #22) — dumps save immediately after upload to determine if SRAM received correct data (MATCH vs MISMATCH).
- **Save write test #22 (64-byte chunks + verify read after close/reopen):**
  - Write completed: 512 chunks × 64 bytes, all ACKs rd=4 retries=2, final probe rd=-7008.
  - Verify read (fresh handle after 200ms close/reopen): `Save cmd ACK: r60=4 r4=-7008`. Command ACK returned only 4 bytes instead of the normal 60 or 16 — anomalous.
  - `Save host ACK resp: r60=4 r4=-7008` — host ACK response also only 4 bytes.
  - `save[0] rd=60 [0..3]=00 00 00 00` — first data packet all zeros (actually the real 60-byte command ACK packet, consumed as data because the two 4-byte residuals shifted reads).
  - `[verify] SRAM[0..15]: 00 00 00 00 11 12 1E 1F 1E 1F 10 11 11 11 11 11`
  - `[verify] 32 KB compare: MISMATCH`
  - **Analysis:** The MISMATCH is a false alarm from a 4-byte IOS buffer residual. Two anomalous `r60=4` reads in the verify consumed two residual 4-byte packets buffered from the write. This shifted all subsequent reads by two 4-byte slots, making the real 60-byte command ACK appear as `save[0]` (all zeros), and the real SRAM data appear starting at verify buffer offset 4. SRAM[4..7] = `11 12 1E 1F` = our upload data[0..3], confirming a 4-byte read shift rather than a write failure.
  - **Root cause identified (gbopyrator inspection 2026-06-19):** `write_save` in `coms_utils.py` sends NO initial host ACK before data. Our implementation was sending 64 zero bytes as a spurious "host ACK" — the device interpreted this as the FIRST 64-byte data chunk and wrote zeros to SRAM[0..63], shifting all actual data by one chunk. Additionally, gbopyrator reads `60+4` bytes after each data chunk (not just 4), matching the command ACK format. Our implementation read only 4 bytes per chunk, leaving one read's worth of data buffered in IOS per chunk (512 × unread packets accumulated).
  - **Fix (test #23):** Removed spurious initial host ACK from `gbop_write_save`. Changed per-chunk ACK from 4-byte retry loop to `USB_ReadBlkMsg(60)` + `USB_ReadBlkMsg(4)` (matches gbopyrator).
- **Save read protocol fix (2026-06-20 — same root cause as write):**
  - `gbop_read_save` had the same structural bugs as the old write: spurious 64-zero initial host ACK (consuming stream position), "ready to stream" response reads (consuming real save bytes), drain-if-r1==16, and device-initiated ACK handling (rd==4 → send 64 zeros). None of these exist in the actual protocol.
  - Confirmed from gbopyrator `coms_utils.py read_save`: after command ACK (60+4), data streams immediately with **no host ACKs at any point**. `read_bulk_in(with_ack=False)` reads 64-byte USB reads until save_size bytes accumulated.
  - **Fix:** Stripped all spurious handshake logic. New implementation: send command → 60+4 command ACK → pure `USB_ReadBlkMsg(64)` loop accumulating bytes until save_size received. No ACKs, no drain, no device response handling.
  - **Verify delay:** increased post-write delay before verify from 200ms to 1500ms for GBA (Flash programming), kept 200ms for GB/GBC (SRAM).
- **Save command byte 1 bug (2026-06-20):**
  - gbopyrator confirmed: save read trigger = `[0x02, 0x00, 0x00, 0x00, 0x00, size_LE...]`; save write trigger = `[0x03, 0x00, 0x00, 0x00, 0x00, 0x00, size_LE...]`. **cmd[1] must be 0x00 for both.**
  - Our code had `{ 0x02, 0x02 }` for read and `{ 0x03, 0x02 }` for write (cmd[1] wrong).
  - GB/GBC SRAM saves tolerate the wrong cmd[1] → Gold dumps were correct.
  - GBA Flash saves require cmd[1]=0x00 to select Flash read mode → FireRed dumps were near-all-zeros (device reading from wrong memory type).
- **Save write size offset bug (2026-06-20):**
  - Write trigger has ONE extra `0x00` at byte 5 vs read trigger: `[0x03, 0x00, 0x00, 0x00, 0x00, 0x00, size_lo, size_mid, size_hi]`. Size at bytes 6..8.
  - Our write code put size at bytes 5..7 (same as read) → device received 512 instead of 131072 for 128KB save. Device wrote only first 512 Flash bytes and ACKed the rest without programming.
  - Game save survived because section footer at 0xFF4 (byte 4084) was beyond the 512-byte write, and/or the game loaded from the backup save slot.
  - **Fix:** size moved to cmd[6..8] in `gbop_write_save`.
- **Save read protocol — host ACK overhead confirmed (2026-06-20) — RETRACTED:**
  - The original "host ACK required" conclusion was wrong. It was tested with the buggy `cmd[1]=0x02`, which caused undefined device behavior. With the correct `cmd[1]=0x00` the device behavior is different.
  - **Save read command**: size at bytes 5..7 (same as gbopyrator). **Save write command**: size at bytes 6..8 (one extra zero at byte 5).
  - **Save read streaming log (cosmetic)**: The `chunk_cnt < 5 || rd != 60` logging condition caused data chunks at indices 5+ to be silently suppressed. Data WAS received — probe count incremented chunk_cnt — but chunks 5+ were invisible. Fixed in test_29 code.
- **GBA Flash write delay — 1.5s insufficient (test_27, 2026-06-20):**
  - test_27 wrote valid FireRed save (non-zero at 0xFF4), all 2048 × 64-byte chunks ACKed. Verify after 1.5s delay showed all zeros and MISMATCH.
  - Root cause: GBA Flash erase before programming takes ~1-20s per 64KB sector; 1.5s catches the device mid-erase.
  - **Fix:** GBA verify delay increased from 1.5s to 10s. Added first-diff position logging to verify output.
- **GBA save dump returns all zeros (test_28, 2026-06-20) — ROOT CAUSE IDENTIFIED:**
  - Windows GB Operator dump of same cart at same time: 131072 bytes, first non-zero at 0xFF4=0x0A (valid). Wii program dump: 131072 bytes, ALL ZEROS.
  - Streaming completed normally (2185 chunks received, `save complete: 131072 bytes`). The device was NOT stalling. Data arrived but contained only zeros.
  - **Log analysis**: The alternating `rd=60 / rd=4` pattern (data chunk / probe, data chunk / probe) observed in the stream was caused by the host ACK putting the device into a special streaming mode. Without the host ACK, gbopyrator (PC) reads save correctly without any probes.
  - **Root cause**: The 64-zero host ACK sent to EP OUT after the command ACK triggered a Flash erase on the GBA cart before streaming. The device then streamed from freshly-erased Flash (all zeros). This matches the write protocol behavior: sending 64 bytes to EP OUT in the context of a pending Flash operation triggers Flash manipulation. gbopyrator sends NO host ACK and reads correctly; our host ACK was the destructive operation.
  - **Fix (test_29)**: Removed host ACK from `gbop_read_save` entirely. Also removed ready-response reads and drain (those only existed because of the host ACK response). Streaming now matches gbopyrator exactly: command → ACK (60+4) → pure `USB_ReadBlkMsg(64)` loop. All chunks logged with non-zero detection and pre-read flush for first 10 chunks (hang detection).
  - **test_29 result**: Device hung at `save pre-read chunk=3 rx=80`. Chunks 0,1,2 = 60+16+4=80 bytes received before hang. The rd=4 at chunk 2 is a per-chunk probe the device waits on before continuing (per-chunk flow control). Without responding to it, device stalls on chunk 3.
  - **test_30 result (no initial ACK, 0x00 probe responses)**: Device sends one batch of rd=16 header + 9×rd=60 data chunks (556 bytes total), each preceded by a rd=4 probe (responded 64 zeros). After chunk 9, device sends ONLY rd=4 probes forever (chunk_cnt stuck at 10, no data). Dump is all zeros (only 556 bytes received, rest calloc zeros; PKHeX "cannot read"). Root cause: without initial host ACK, device uses batched streaming (9 chunks/batch). The 64-zero probe response satisfies in-batch flow control but NOT the end-of-batch "request next batch" signal. Device gets stuck waiting for a different batch ACK.
  - **Fix (test_31)**: Restored initial host ACK (enables continuous streaming, bypassing batch mode) but changed content from 0x00 to 0xFF. On Sanyo LE26FV10N1TS / Panasonic MBM29F033C GBA Flash chips, 0xFF is the Reset/Read-Array command — keeps the chip in read mode instead of triggering a write/status-register mode. Probe responses also changed to 0xFF for same reason.
  - **test_31 result**: Same as test_28 — 2185 chunks received, 131072 bytes, ALL ZEROS. PKHeX "all zeros." Changing probe content from 0x00 to 0xFF made no difference.
  - **test_31 log analysis**: After `save[9]` (10 × rd=60 logged), the loop shows alternating `rd=4 probe → silent rd=60 chunk` pairs indefinitely. Silent chunks (chunk_cnt ≥ 10, rd=60, all-zero) are NOT logged because logging was suppressed for chunk≥10 and non-zero detection only checked s_rx[0..3]. The first non-zero byte in a valid FireRed save is at offset 0xFF4=4084, which is byte 4 of chunk 68 (s_rx[4]). The old `has_nz` check was blind to s_rx[4+]. PKHeX confirms file is all zeros.
  - **Key insight**: probe content (0x00 vs 0xFF) doesn't affect outcome → not a Flash chip command issue. The act of sending a 64-byte OUT packet during save read streaming is what causes zeros. The device write protocol uses 64-byte OUT chunks; hypothesis is the device interprets each 64-byte OUT response as a WRITE DATA chunk, overwriting Flash or putting the device into write-status mode, causing IN stream to return zeros/status bytes.
  - **Fix (test_32)**: Change probe responses from 64 bytes to 4 bytes. A 4-byte OUT cannot be mistaken for a 64-byte write data chunk. Also fixes `has_nz` detection to scan all `rd` bytes (not just first 4) and logs `first_nz` byte offset and value. Initial host ACK remains 64 × 0xFF (needed for continuous streaming).
  - If test_32 dumps valid data: confirmed that 64-byte probe responses were triggering write mode; 4 bytes are safe.
  - If test_32 returns all zeros: probe SIZE is not the issue; something about the initial 64-byte host ACK itself causes zeros regardless of subsequent probe size.
- **GBC save write test_47 (Pokemon Gold, 32KB SRAM):**
  - Write completed: 512 × 64-byte chunks sent, all per-chunk ACKs received.
  - Write cmd ACK: `r60=60 r4=-7008` — the 4-byte footer arrived after IOS timeout. Deferred 4-byte packet not consumed.
  - Per-chunk ACK sizes: chunk 0 = r60=16 (deferred 4-byte footer consumed by IOS as short packet, then partially merged with first device chunk ACK); chunks 1-511 = r60=4. Device on Wii sends smaller ACK packets than on PC (Epilogue shows 60+4); root cause is IOS USB gap timing.
  - Verify read (after close/reopen/200ms): `r60=60 r4=4` — normal GBC ACK. Code skipped drain (drain only fires on `rb<0`). BUT device sent 2 extra packets (rd=60+rd=4 = 64 bytes all zeros) before save data.
  - **MISMATCH was a false negative**: source first diff at 0x009E (0x80); verify dump first NZ at 0x00DE (0x80 = 0x009E + 0x40 = 0x009E + 64). Verify buffer offset by exactly 64 bytes = the 2 extra undrained packets.
  - **SRAM was written correctly** — game plays without corruption; mismatch only in the verify read.
  - **Root cause**: GBC device sends 64-byte protocol overhead (rd=60+rd=4 zeros) before save data when a read command immediately follows a write operation. Cold reads (Epilogue test_46 capture, fresh Dump Save) start data immediately with no overhead.
  - **Fix (peek-2 in `gbop_read_save`)**: After ACK, for GBC (rb>=0), read two packets: pkt_a and pkt_b. If (pkt_a=60 and pkt_b<60): drain pair (protocol overhead), discard. Else: real data, inject both into buffer. Distinguisher: drain pair = 60+short; real data = 60+60 (first two data packets are always rd=60).
- **GBA write and verify test_48 (FireRed, 128KB Flash — first confirmed full pass):**
  - test_48 attempt 1: write loop completed all 2048 chunks (128/128 KB logged to TV). Hang then occurred in `gbop_read_save()` verify streaming — `USB_ReadBlkMsg(64)` blocked because GBA Flash sector erase takes >10s and was still in progress when verify started. The write itself was never the problem.
  - test_48 attempt 2 (after power cycle): write + verify both completed cleanly. Flash erase was pre-done by attempt 1, so programming finished within 10s.
  - Per-chunk ACK confirmed pattern on Wii: chunk 0 = r60=16, chunks 1+ = r60=4, gap r4=-7008 for all.
  - **Root cause of "128/128 hang"**: NOT the write ACK. The write always completed. The hang was in `gbop_read_save()` verify because the 10-second delay was insufficient for fresh Flash erase+program.
- **GBA write attempt tests_49/50 (REGRESSION — code change error, do not repeat):**
  - Changing `USB_ReadBlkMsg(60)` to `USB_ReadBlkMsg(4)` for per-chunk ACK reads caused immediate -7005 STALL at chunk 1 write in test_49 (no gap) and chunk 1 ACK in test_50 (gap restored but protocol corrupted by IOS residual packets from test_49).
  - **Per-chunk ACK must use `USB_ReadBlkMsg(GBOP_PAYLOAD_SIZE)` (60-byte read)** — it handles chunk 0's 16-byte packet and chunks 1+'s 4-byte packets without issue. Changing to 4-byte reads disrupts the protocol because the 60-byte request size is part of how IOS accepts variable-length USB short packets.
  - IOS USB receive buffers are NOT flushed on `gbop_close()`/`gbop_reopen()`. Packets stranded by a mid-transfer failure persist until the device is physically power-cycled.
- **GBA write and verify test_51 (first-attempt success, 2026-06-21):**
  - Write cmd ACK: r60=60 r4=-7008 (clean state after power cycle). All 2048 chunks, all ACKs correct. write final probe: rd=4.
  - 30s verify delay: Flash completed within window, no hang.
  - Verify MATCH. First NZ at offset 0x0FF4 = val 0x0A (valid FireRed section footer).
  - **GBA write+verify confirmed reliable on first attempt** with 30s delay.
- **GBC teardown hang — thread abandonment (test_73, 2026-07):**
  - After GBC save write completes, `gbop_write_save` read a final `USB_ReadBlkMsg(4)` diagnostic probe. On Wii this call blocks longer than the 4.5s `cart_sync_shutdown` timeout — the main thread abandoned the sync LWP, which survived into the next session.
  - Abandoned thread's deferred probe return then wrote to session-2 globals (`s_buf`, `s_info`, `s_save_path`), racing with session-2 `cart_sync_init` calls. Interleaved log lines from both threads visible in session-2 logs.
  - **Fix (test_74):** Removed the final GBC probe entirely from `gbop_write_save`. All chunks are already sent and per-chunk ACKed before the probe; the probe was diagnostic-only. GBC write now ends after the last chunk ACK. Sync thread exits within the 4.5s window and is joined cleanly every session.
- **GBC save detection for OSD — stable baseline (test_74, 2026-07):**
  - Rolling `s_sync_snapshot` (updated every 30 frames of `savedataUpdated`) couldn't reliably detect real CGB saves. CGB RTC games (Pokémon Gold/Silver) continuously dirty SRAM every second with clock bytes (~16–200 bytes/tick). A 30-frame window shows the RTC diff but not the cumulative total, so `is_real_save` (threshold=64 bytes) fires on RTC ticks.
  - **Fix:** Added `s_stable_baseline` — initialized to on-disk save at session load, advanced only on confirmed cart sync via `on_cart_sync_success`. Compared to current SRAM in the `savedataUpdated` else-branch: if total diff ≥ 512 bytes, sets `s_new_save_since_sync = 1`. A real Pokémon save rewrites ≥3584 bytes in a single operation; the stable baseline accumulates ALL drift since the last sync, making this detectable even when individual 30-frame diffs are small.
- **GB/C OSD workflow (test_74, 2026-07):**
  - OSD "Sync to Cart" is now gated by `s_new_save_since_sync` for GB/C. If no in-game save detected since last sync, the sync item shows "Save in-game first, then sync" and does nothing. After a save is detected (stable-baseline diff ≥ 512), sync is allowed; after a successful sync clears `s_new_save_since_sync`, the guard re-arms until the next save.
  - On exit: if `s_player_saved_ingame && s_new_save_since_sync` (saved but not synced) for GB/C, a Y/N screen warns "You saved in-game but haven't synced to the cartridge yet. Your save is backed up on SD." Pressing A exits anyway; B returns to the OSD to sync first.
  - GBA exit: silent if `!s_new_save_since_sync` (auto-sync completed). If auto-sync failed and `s_new_save_since_sync` is still set, same Y/N prompt.
- **GX successive-session crash — GX_Init guard (test_74, 2026-07):**
  - On the second `mgba_run` call, `gx_setup` called `GX_Init(s_gxfifo, GX_FIFO_SIZE)` again. The GX CP remains armed to `s_gxfifo` between sessions (intentional — freeing it causes machine check exceptions). Calling `GX_Init` on an already-armed CP produces undefined CP state; manifested as intermittent crashes on session 2 boot with red-screen visual artifact.
  - **Fix:** Added `s_gx_initialized` flag. `GX_Init` is called only on the first `gx_setup` invocation. All register configuration (clear color, viewport, vertex format, TEV, projection matrix, texture objects) still runs every session.
- **Border filename format (test_74, 2026-07):**
  - Changed border lookup from sanitized 11-char title (`POKEMON_SLV.bmp`) to 4-char game code read from ROM header offset 0x13F (`border_AAXE.bmp` for Pokémon Silver, `border_AAUE.bmp` for Gold). Falls back to title-based name if the 4 bytes are not valid uppercase ASCII/digits. Pure GB games have no external border — SGB borders supplied by mGBA core only.
- **Save indicator hidden for GB/C (test_74, 2026-07):**
  - GBA auto-sync is reliable (threshold=1, fires on every save). The save indicator dot is shown for GBA only. For GB/C, `draw_save_indicator` returns early without drawing but still calls `cart_sync_state()` to keep the sticky-success timer ticking (required to prevent timer drift when the cart sync succeeds during a GB session).
- **Successive-session malloc hang — heap corruption via log race (test_76, 2026-07):**
  - After 3-4 sessions (mixed GBA + GBC), `malloc(16MB)` for the GBA ROM hung indefinitely (no crash, no NULL return, visible on TV console). All cart_sync threads had joined cleanly — no abandoned thread. Root cause: heap use-after-free corrupting the malloc free list.
  - **Mechanism:** `mgba_frontend.c` called `log_commit_sd()` (→ `fclose(g_log)` + `fopen`) every 300 frames during emulation. The cart_sync LWP thread simultaneously called `lprintf` → `vfprintf(g_log)` on the same FILE. `fclose` frees the FILE's internal stdio buffer while `vfprintf` is still writing into it. The freed buffer is then overwritten by the sync thread, corrupting the heap's free list nodes. The corruption accumulates over several sessions until `malloc(16MB)` follows a corrupted pointer chain → infinite loop → hang.
  - **Secondary symptom:** When `log_commit_sd`'s `fopen` returns NULL (corrupted libfat state), `g_log` becomes NULL and all subsequent file logging in that session is silently lost. This is why Session 5's teardown shows `freeing snapshot` but not `freeing border` in the log file (those lines were printed to the TV console but g_log was NULL).
  - **Fixes applied:**
    1. `source/mgba_frontend.c` line ~1226: Changed per-300-frame `log_commit_sd()` in the emulation loop to `log_force_flush()` (fflush only — does not free the FILE struct; safe for concurrent callers).
    2. `source/log.h` `log_commit_sd`: Changed `if (g_log && ...)` guard to unconditional reopen — if g_log is NULL from a prior fopen failure, next `log_commit_sd` call will recover it.
    3. `source/mgba_frontend.c` teardown: Added `AUDIO_RegisterDMACallback(NULL)` + `s_audio_cur = s_audio_next = 0` after `AUDIO_StopDMA()` to prevent stray AI interrupts from restarting DMA after teardown.
  - **Rule:** `log_commit_sd()` (fclose+fopen) must NEVER be called while the cart_sync thread is alive. Use `log_force_flush()` (fflush) during emulation.
- **test_77 regression — `mCoreConfigDeinit` before `core->deinit` causes crash (2026-07):**
  - Adding `mCoreConfigDeinit(&core->config)` before `core->deinit(core)` in teardown to fix a ~3.7KB/session leak (3 config hash tables not freed by `_GBACoreDeinit`/`_GBCoreDeinit`) caused a crash at or just after "core deinit done" — next heap operation (`malloc` inside `fopen` in `log_commit_sd`, or `free(rom_data)`) hit corrupted heap metadata.
  - Likely mechanism: freeing 43 small hash-table blocks triggered dlmalloc coalescing that corrupted adjacent block headers.
  - **Fix:** Reverted — `mCoreConfigDeinit` is NOT called in teardown. The ~3.7KB/session leak is benign; the malloc hang was caused entirely by the log FILE* race, which is fixed by `log_force_flush()`. Do not add `mCoreConfigDeinit` back to teardown.
- **test_78/79/80 — session-3 DSI crash from repeated fclose+fopen cycling (2026-07):**
  - test_78: after reverting `mCoreConfigDeinit`, crash still occurred at "core deinit done" in session 2 teardown. Root cause: `log_commit_sd()` calls `fclose(g_log)` then `fopen(g_log_path, "a")`, each cycling the stdio FILE struct through malloc/free. With ~32 `log_commit_sd()` calls per session, and mGBA's large per-session heap alloc/free cycles (16MB ROM, 288KB WRAM, 96KB VRAM, 130KB audio, 128KB save), the dlmalloc free list accumulates corruption across sessions. Session 3's first `fclose(g_log)` triggers a bad-pointer dereference in the coalescing path → DSI red-border crash.
  - test_79 diagnostic: replacing `log_commit_sd()` with `log_force_flush()` at "core deinit done" fixed the session-2 teardown crash, but crash moved to session-3 setup (line 946 `log_commit_sd()` after "Save loaded into core"), confirming cumulative corruption across sessions — not a single-point issue.
  - **Fix (test_80):** ALL `log_commit_sd()` calls inside `mgba_run()` changed to `log_force_flush()` (fflush only), EXCEPT the final "Session ended" line. `fflush` is sufficient to commit log data to SD on Wii (libfat writes sectors on fflush). Reducing fclose+fopen cycles per session from ~32 to 1 eliminates the cumulative FILE-struct malloc/free corruption. Rule: never call `log_commit_sd()` inside `mgba_run()` except at "Session ended".
- **Successive-session DSI crash — root cause identified (tests 81–84, 2026-07):**
  - After test_80 fixes, a DSI crash (red-border) occurred on the SECOND session: quitting mGBA after a successful save+sync produced DAR=0x10000000, PC≈0x800A7E9C, same values every run.
  - **Root cause:** `core->deinit(core)` (mGBA teardown) corrupts dlmalloc's free-list metadata. Specifically, freeing mGBA's internal allocations (WRAM 288KB, VRAM 96KB, audio 130KB etc.) triggers dlmalloc coalescing that writes invalid pointer values into adjacent block headers. The next `malloc()` call traverses the corrupted list, follows a poisoned pointer to 0x10000000 (physical MEM2 base, not mapped in virtual address space) → DSI. `free()` still works on a corrupted heap (it links blocks without traversing); only `malloc()` crashes.
  - **Proof from test_84 log:** all teardown `free()` calls succeeded (rom, save, snapshot, border logged). Last line was `[mgba] Session ended`. Crash occurred in the `log_commit_sd()` call that followed → `fopen()` → `malloc()` → DSI. No `[reload]` line appeared, confirming crash was before `dol_reload()` ran.
  - **Fix (test_84):** Changed `log_commit_sd()` at "Session ended" to `log_force_flush()` (fflush only — no malloc). Added `source/dol_reload.c`: zeros BSS (resetting dlmalloc to pristine state), then branches directly to `main(0, NULL)`. Branches to `main()` directly (NOT the CRT entry point) — the CRT calls `SYSTEM_Init()` which reinitialises IOS on already-live hardware → DSI (confirmed test_82). After BSS zero, `dol_reload()` uses `fclose(g_log)` (not `fopen`) to close the log — fclose uses `free()` (safe on corrupted heap), not `malloc()`.
  - **libogc exception API does not exist:** attempted to add a DSI exception handler using `frame_context`, `SYS_SetExceptionCallback`, `SYS_EXCEPTION_DSI`, `exception_t` — all undefined in the installed libogc. Do not attempt to use any of these symbols.
- **Wii devkitPPC memory layout — stack overlaps BSS (discovered tests 85–86, 2026-07):**
  - On this Wii, the initial SP captured at `dol_capture_entry()` (very start of `main()`) falls INSIDE the BSS address range `[__bss_start, __bss_end)`. Example from test_86: `sp=0x802055F0, bss=[0x8018679c,0x80263614)` — 0x802055F0 is between those two values.
  - The stack grows DOWNWARD from the initial SP. Therefore, all active stack frames during `dol_reload()` (which is called many frames deep after a full emulation session) are entirely within the BSS range. `memset(__bss_start, 0, __bss_end - __bss_start)` zeros the live call stack — all local variables saved before the memset are wiped by it.
  - **Consequence:** saving values to local variables (e.g. `bool saved_gx = g_gx_initialized; ... memset(BSS); ... g_gx_initialized = saved_gx;`) does NOT work. After the memset, `saved_gx` reads as 0 regardless of its pre-memset value.
  - **Fix (test_87):** Any value that must survive the BSS zero must be stored in a `.data` static variable (not BSS, not on the stack) BEFORE the memset. Use `__attribute__((section(".data")))` with a **non-zero initialiser** (to ensure the compiler emits the variable into `.data` rather than `.bss`). In `dol_reload.c`: `s_initial_sp` and `s_preserve_gx` are both `.data` statics. The BSS memset cannot reach `.data`. After the memset, `g_gx_initialized = s_preserve_gx` (read from `.data`) correctly restores the GX guard. The asm input `"r"(s_initial_sp)` reads directly from the `.data` address (still valid post-memset).
  - **test_87 result: crash with different visual glitches.** The `.data` fix worked (confirmed by `nm`), but a new crash occurred in `main()` restart. Root cause: `memset(__bss_start, ...)` only zeroed `.bss`, leaving `.sbss` untouched. libogc's `s_firstThread` (LWP thread-list head, at 0x80186444 in `.sbss`) still pointed into the now-zeroed `.bss` TCB pool after the memset. When session 2's `LWP_CreateThread` walked that dangling pointer → DSI. Additionally, `postRetraceCB` (VI retrace callback, in `.sbss`) retained its session-1 function pointer; when `VIDEO_Init` in main() restart re-enabled VI interrupts, the stale callback fired → DSI. The MSR[EE] bit (external interrupt enable) was also left cleared by `_CPU_ISR_Disable` and never re-enabled before `b main`.
- **test_88 fix — zero `.sbss` too, re-enable interrupts before branch (2026-07):**
  - `nm` of test_87 build: `__sbss_start = 0x801863cc`, `__bss_start = 0x801866fc`. The `.sbss` section (48 bytes) occupies `[0x801863cc, 0x801866fc)` and contains `g_gx_initialized`, `s_firstThread`, `__ppc_next_ctx`, `postRetraceCB`, `g_log`, `rmode`, `xfb`, and libogc LWP pool state — none of which were zeroed by the old `memset(__bss_start, ...)`.
  - **Fix:** `dol_reload()` now zeroes from `__sbss_start` through `__bss_end` in a single `memset`. This clears `s_firstThread` (→ LWP_CreateThread starts from clean empty list) and `postRetraceCB` (→ no stale VI callback fires). `g_gx_initialized` is in `.sbss` and gets zeroed, but is then restored from `s_preserve_gx` (in `.data`) after the memset, same as before.
  - **Fix:** Added interrupt re-enable in asm before `b main`: `mfmsr r11; ori r11, r11, 0x8000; mtmsr r11; isync`. MSR[EE] was cleared by `_CPU_ISR_Disable`; without this restore, `VIDEO_WaitVSync()` in `init_video()` would spin forever (VI sync interrupt never delivered).
  - **`s_initial_sp` and `s_preserve_gx` confirmed safe in `.data`** (addresses 0x80182fa0 and 0x80182f9c, both below `__sbss_start` 0x801863cc — not in the zeroed range).
  - **Diagnostics:** `printf/lprintf` checkpoints added at start of `dol_reload()` and after `init_video()` in `main()` to pinpoint crash location on TV if test_88 still fails.
- **GX FIFO static buffer — moved from heap to .data (2026-07):**
  - `s_gxfifo` was a heap-allocated pointer (`memalign(32, GX_FIFO_SIZE)`). After BSS zero, dlmalloc resets and the pointer becomes NULL. On the next session, `gx_alloc()` would call `memalign` again, allocating a NEW buffer at a different address. The GX CP remains armed to the OLD address → CP reads heap data as GX commands → machine check exception.
  - **Fix:** `s_gxfifo_buf` is now `static uint8_t s_gxfifo_buf[GX_FIFO_SIZE] __attribute__((aligned(32), section(".data")));` — a fixed-address 256KB array in `.data`. The heap allocation is gone. The CP is always armed to the same fixed address regardless of dlmalloc state. `gx_alloc()` no longer calls `memalign` for the FIFO.
  - `g_gx_initialized` (renamed from `s_gx_initialized`, now non-static) is preserved across the BSS zero via `s_preserve_gx` in `dol_reload.c`, ensuring `GX_Init()` is never called twice on the armed CP.
- **test_89 session 2 crash — IPC interrupt handler wiped by BSS zero (2026-07):**
  - test_89 dol_reload succeeded: session 2 started, cart info read OK, menu appeared (confirmed by 29-line log1.txt). Crash occurred during menu wait loop (`VIDEO_WaitVSync` / `WPAD_ScanPads`).
  - **Root cause:** `memset(__sbss_start, 0, ...)` wiped the `__irqhandler[]` table in `.bss`. This table is populated by `IOS_Init()` (called from CRT's `SYSTEM_Init()` at binary load time). Among the entries it registers: `__IPC_IRQHandler` for `IRQ_PI_MEMLO`. In session 2's menu loop, the first `WPAD_ScanPads()` triggers a WPAD Bluetooth event from IOS → `IRQ_PI_MEMLO` interrupt fires → NULL handler → DSI red-border crash. Cannot re-call `SYSTEM_Init()` on live hardware (confirmed test_82 DSI).
  - **Conclusion:** BSS zero is fundamentally incompatible with libogc's one-time-init architecture. Every layer fixed by dol_reload (stack overlap, `.sbss` pointers, MSR[EE], GX FIFO) exposed another unmovable dependency. `core->deinit()` is the SOLE source of heap corruption — the entire dol_reload approach was a workaround for one root cause.
  - **Final fix (test_90):** Skip `core->deinit()` in all paths in `mgba_frontend.c`. Remove `dol_reload()` call from `main.c`. The `while(1)` loop handles successive sessions naturally. mGBA internals (~640KB) leak per session; the heap stays clean and the while(1) loop runs indefinitely without crashes.
- **IOS USB fd spend — play_game cart_info stall (test_114 / test_118, 2026-07):**
  - The boot sequence uses fd A for: `gbop_find` cart-info probe, ROM header mini-dump, and repeated poll iterations (each calling `gbop_read_cart_info`). After ~5 EP OUT writes the IOS USB fd is "spent" — `USB_WriteBlkMsg` returns -7005.
  - **test_114:** fd was spent before the save read in `play_game`. Fix: fresh-fd wait loop (50 × 60ms) before the save read, replacing the spent fd with a new one after IOS re-enumerates (~2.5s, confirmed pattern: ~36 same-fd tries → 6 GetDeviceList-failed → 1 new fd).
  - **test_116:** fresh-fd wait timing confirmed: new fd appears at try ~42 (2.5s). 50-try limit has ~8 tries of headroom.
  - **test_118:** fd spent even earlier — at the `gbop_read_cart_info` probe at the top of `play_game`'s save-read section. The fresh-fd wait (which was after cart_info) never ran. Result: `cmd=0x04 tx=-7005`, "Cart not detected" shown, user had to press Play Game twice.
  - **Fix (post-test_118):** When `gbop_read_cart_info` returns `GBOP_USB` in `play_game`, set `need_cart_recheck=true` and fall through to the fresh-fd wait (which already exists for the save-read path). After the fresh-fd wait's commit-cycle, retry `gbop_read_cart_info` on the clean fd. The save read then runs on the same fresh fd (2 total EP OUT ops — well within budget). First Play Game attempt now succeeds transparently even when the fd is spent.
- **GBA border BMP format — 8bpp indexed accepted (test_118, 2026-07):**
  - `load_border_bmp` previously rejected 8bpp indexed BMPs (`need 320x240 24/32bpp; got 320x240 bpp=8 comp=0`). Aseprite saves indexed-mode sprites as 8bpp by default.
  - **Fix:** Added 8bpp support to `load_border_bmp` in `source/mgba_frontend.c`. After validating bpp=8/comp=0, reads 256-entry BGRA palette from file offset 54 (standard BITMAPINFOHEADER), then for each pixel looks up the palette entry and converts B/G/R → RGB565. Row stride = `(exp_w + 3) & ~3`.
  - test_118 confirmed: `[border] Loaded: ...border_BPRE.bmp (8bpp)` — border displayed correctly, `GBA-border: YES`, `scale_gba_border=1.00` applied.
- **Dev menu log_commit_sd before USB ops (2026-07):**
  - Dev menu choices 0–2 (Dump ROM, Dump Save, Upload Save) had no `log_commit_sd()` call before `gbop_reopen()`. If any operation hung, log lines since the last commit were lost.
  - **Fix:** Added `log_commit_sd()` immediately before `op = gbop_reopen()` in the dev menu choices 0–2 block. USB is closed at that point for all three paths. "[dev] Dump ROM/Save/Upload Save" line is now committed to FAT before the USB handle opens.
- **ROM install → "Cart not detected" on immediate Play Game (test_121, 2026-07):**
  - After `rom_cache_dump` completes, the last in-stream ACK fires at the exact last byte boundary (`dump_iter_cnt % 320 == 0` simultaneously with `written == buffer_size`). The ACK sets `dump_pending_drain = 2` and the loop breaks immediately — the 2 deferred drain reads (rd=60 + rd=4, device's ACK response) are never executed.
  - IOS EP IN buffer retains those 2 packets after `gbop_close()` (IOS buffers are not flushed on close/reopen). When play_game's save-read section calls `gbop_read_cart_info()`, `gbop_bulk_recv` reads the residual rd=60 packet as the command ACK and the residual rd=4 as the optional footer, consuming the cart_info ACK (all zeros) as chunk[0]. `resp[3:5] == 0` → NOCART → "Cart not detected. Check insertion." play_game returns. Second Play Game attempt succeeds (buffer is now clean).
  - **Fix:** In `gbop_dump_rom`, within the `dump_given >= dump_total` completion block, drain any remaining `dump_pending_drain` packets before setting `dump_active = 0` and logging "ROM dump complete". Logged as `[gbop] final drain rd=N [0..3]=...`.
- **test_122 (2026-07) — final drain fix confirmed, multi-cart stress:**
  - Pokemon Gold (GBC, 2MB ROM install → immediate Play Game → GBC session with cart sync), Pokemon FireRed (GBA, cached, quick session), Pokemon Emerald Japan (GBA, 16MB ROM install → Play Game → GBA session with save sync), Mario Deluxe (GBC-only, 512KB ROM install → Play Game → save sync), plus second sessions of Emerald and Mario from cache. No issues on any of 5 Play Game presses following a ROM install. `final drain rd=60` + `final drain rd=4` confirmed in log for every completed ROM install.

---

## GB Operator USB Protocol

### Device Identifiers
- **VID:** `0x16D0`
- **PID:** `0x123D`
- Confirmed from Wii USB enumeration (web research had wrong values: 0x1D50:0x6018)

### Transport
- **USB bulk transfers** on EP OUT=`0x01`, EP IN=`0x81` (confirmed from hardware)
- IOS `USB_GetDescriptors` does not work — endpoints found by trial and error
- IOS `USB_GetDeviceList` ignores class filter — always returns all devices

### Packet Format
- **OUT (command):** 64 bytes — bytes 0-59 = payload, bytes 60-63 = CRC32-MPEG2 of bytes 0-59 (little-endian)
- **IN (command response — hardware-verified structure):**
  1. **ACK:** two separate USB short packets: `USB_ReadBlkMsg(60)` → 60 bytes (zeros, discard), then `USB_ReadBlkMsg(4)` → 4 bytes (zeros, discard)
  2. **Data chunk 0:** one 64-byte USB packet: `USB_ReadBlkMsg(64)` → 64 bytes (first 60 = actual response payload, last 4 = CRC)
  3. **Data chunk 1:** one 64-byte USB packet: `USB_ReadBlkMsg(64)` → 64 bytes (zeros/padding)
  - Cart info lives in the first 60 bytes of data chunk 0 (NOT in the ACK)
  - Total: device sends 4 USB packets (ACK as 2 short packets + 2 full 64-byte data packets)
  - **Critical Wii vs PC difference:** the ACK is sent as two separate USB packets (60 and 4 bytes), while data chunks are sent as single 64-byte USB packets. Reading a data chunk with `USB_ReadBlkMsg(60)` truncates the 64-byte packet and the subsequent `USB_ReadBlkMsg(4)` times out (-7008 = IOS USB timeout error) because no 4-byte packet follows. Data chunks must be read with a single `USB_ReadBlkMsg(64)` call.
  - **gbopyrator on PC accumulates 256 bytes (4×64) using PyUSB's `read(endpoint, 64)` which catches `USBTimeoutError` gracefully. On Wii, reading past the 4 real packets with a 60-byte read hangs indefinitely (no IOS timeout for 60-byte reads). Only read 2 data chunks.**
- **ROM dump command (0x00) response (hardware-verified 2026-06-13):** device sends 84 bytes across 4 USB packets as a "ready to stream" handshake, all zeros: 16-byte + 4-byte + 60-byte + 4-byte. Read sequence: `USB_ReadBlkMsg(60)` → rd=16 (short packet); `USB_ReadBlkMsg(4)` → -7008 (IOS 4-byte timeout fires before 4-byte packet arrives). Then drain the remaining 3 packets with 3 × `USB_ReadBlkMsg(64)` calls (returns rd=4, rd=60, rd=4). Only after all 84 bytes are drained does ROM data begin. **Do NOT attempt to read any response after subsequent in-stream host ACKs** — device sends none and continues streaming immediately.
- **Save read data stream format (confirmed test_43/test_45/test_46):**
  - **Command format differs by cart type** (confirmed Epilogue Playback USB captures; size at bytes 6-8 LE for both):
    - GBA: `cmd[0]=0x02, cmd[1]=0x03, cmd[4]=0x00, cmd[5]=0x01` (Flash — test_45 bit-perfect)
    - GBC: `cmd[0]=0x02, cmd[1]=0x00, cmd[4]=0x20, cmd[5]=0x00` (SRAM — test_46 GBC_SAVE_CART2WIN)
  - **GBA: drain after ACK.** r4 of command ACK returns -7008 (no real 4-byte trailer). Then drain 3 "ready to stream" packets — `rd=16 + rd=60 + rd=4` = 80 bytes (all zeros, discard). Data starts after drain.
  - **GBC: peek-2 drain detection.** r4 of command ACK returns 4 (real packet). After the 60+4 ACK, read two more packets: if (pkt_a=rd60)+(pkt_b=rd<60), discard both (64-byte protocol overhead sent after write); otherwise inject both as real data. Cold reads (no prior write) start data immediately with no overhead; after-write reads have 60+4 overhead (confirmed test_47).
  - After ACK (+ drain): pure `USB_ReadBlkMsg(64)` loop accumulating bytes until save_size received. Device streams (4×rd=60 + 1×rd=16) = 256 bytes/batch. No OUTs at any point during streaming.
  - **Critical errors in earlier implementations:** (1) same command bytes for GBA/GBC — wrong mode for GBC; (2) wrong GBA bytes put Flash in wrong mode → all zeros; (3) spurious 64-zero host ACK triggered Flash erase → all zeros; (4) 80-byte drain not discarded → 80-byte offset corruption (PKHeX "corrupt").
- **Save write data packet format (confirmed test_46 GBA_SAVE_WIN2CART + GBC_SAVE_WIN2CART):**
  - **Command format differs by cart type** (size at bytes 6-8 LE for both):
    - GBA: `cmd[0]=0x03, cmd[1]=0x03, cmd[4]=0x00, cmd[5]=0x01`
    - GBC: `cmd[0]=0x03, cmd[1]=0x00, cmd[4]=0x20, cmd[5]=0x00`
  - After command ACK (60+4 bytes): data upload begins IMMEDIATELY — NO initial host ACK.
  - Each data packet: 64 bytes of raw save data, no CRC.
  - After each data packet: device sends per-chunk ACK — `USB_ReadBlkMsg(60)` then `USB_ReadBlkMsg(4)`. Same format for GBA and GBC.
  - No final command sent after all chunks.
  - **Critical errors in earlier implementations:** (1) wrong command bytes (cmd[1]=0x00 for GBA instead of 0x03) — device may not program Flash correctly; (2) spurious initial host ACK wrote zeros to SRAM[0..63] and shifted all data one chunk; (3) per-chunk ACK read with 4 bytes instead of 60+4, leaving unread packets in IOS buffers.
- CRC32-MPEG2: Poly=0x04C11DB7, Init=0xFFFFFFFF, RefIn=false, RefOut=false, XorOut=0x00
- **Without correct CRC the device returns the ACK but no data follows**

### Command Codes (byte 0 of payload)
| Byte | Command | Notes |
|------|---------|-------|
| 0x00 | Read ROM | cmd[1]=0x00; bytes 2-5 = ROM size LE; bytes 6-8 = save size LE (Epilogue includes this; device ignores it for ROM streaming) |
| 0x01 | Write ROM | |
| 0x02 | Read Save | GBA: cmd[1]=0x03,cmd[5]=0x01; GBC: cmd[1]=0x00,cmd[4]=0x20,cmd[5]=0x00; size at bytes 6-8 LE |
| 0x03 | Write Save | GBA: cmd[1]=0x03,cmd[5]=0x01; GBC: cmd[1]=0x00,cmd[4]=0x20,cmd[5]=0x00; size at bytes 6-8 LE |
| 0x04 | Read Cart Info | no parameters needed |
| 0x05 | Enter DFU Mode | **never send as a probe** |

### Command 0x04 Response Format (60-byte payload)
| Offset | Meaning |
|--------|---------|
| resp[2] | 0x20 = GB/GBC cart; other = GBA |
| resp[3:5] | 0x00 = no cart inserted |
| resp[5:8] | ROM size in bytes, 3-byte little-endian |
| resp[9:12] | RAM/save size in bytes, 3-byte little-endian |
| resp[13+] | Cart title (ASCII) |
| resp[14] | MBC type byte |
| resp[15] | ROM type byte |
| resp[16] | RAM type byte |

### libogc USB APIs
```c
USB_GetDeviceList()   // enumerate — class param ignored by IOS
USB_OpenDevice()      // open interface by device_id
USB_WriteBlkMsg()     // bulk OUT — 64-byte command packet
USB_ReadBlkMsg()      // bulk IN — call twice: 60 bytes then 4 bytes
USB_CloseDevice()     // release handle
```

---

## Sources

All external sources consulted during development are listed here. **This section is mandatory: any external source used to inform protocol decisions, API usage, or implementation details must be added before that information is used in code.** Citing the project is sufficient for collaborative/open-source works; individual files or sections only need to be called out when a specific file is the primary reference.

| Source | URL | Used for |
|--------|-----|----------|
| gbopyrator | https://github.com/N0ciple/gbopyrator | Primary GB Operator USB protocol reference. Command codes, packet format (64-byte OUT with CRC32-MPEG2, 5×(60+4) IN exchange), ROM read trigger format (`_craft_rom_read_trigger` in `coms_utils.py`), save read/write trigger format, in-stream ACK cycle (every 320 chunks). |
| Epilogue Playback | (proprietary — Epilogue website) | Manufacturer's Windows companion app for GB Operator. USB traffic captured with Wireshark to confirm exact command bytes for GBA/GBC save read/write (cmd[1], cmd[4], cmd[5] fields), absence of initial host ACK in write protocol, per-chunk ACK format (r60+r4), and streaming packet structure. Ground truth for all protocol details not covered by gbopyrator. |
| Wireshark | https://www.wireshark.org | USB traffic capture tool. Used to sniff Epilogue Playback ↔ GB Operator traffic on Windows. Primary method for reverse-engineering undocumented protocol fields (save command bytes, ACK packet sizes, streaming batch format). Captures referenced as test_43/45/46 in hardware findings. |
| PKHeX | https://github.com/kwsch/PKHeX | Pokémon save editor. Used as save file validator throughout GBA Flash save work — "cannot read", "all zeros", and "corrupt" errors pinpointed protocol bugs; successful parse with correct game state confirmed bit-perfect saves. |
| gba-link-cable-dumper | https://github.com/FIX94/gba-link-cable-dumper | Wii/GC homebrew GBA ROM/save dumper via link cable. **Different transport (SI serial, not USB) — protocol does not apply.** Consulted for: GBA ROM header layout (title at 0xA0, 12 bytes; game code at 0xAC, 4 bytes; maker at 0xB0, 2 bytes); future reference for GBA save type detection. |
| No-Intro | https://www.no-intro.org | Canonical ROM preservation database. Used as the primary source for GBA game codes (4-char at ROM header 0xAC) and GB/GBC title strings (at ROM header 0x134) in `source/gametitles.c`. Game codes and titles are factual data; No-Intro is credited as the authoritative reference for completeness and verification. |
| libretro-database | https://github.com/libretro/libretro-database | Community ROM database derived from No-Intro data. Cross-referenced for GBC CGB codes (4-char field at ROM header 0x13F) and GB DMG header title strings used in `source/gametitles.c`. |
| gb-operator-reverse-engineering | https://github.com/jaames/gb-operator-reverse-engineering | Early reverse engineering notes. **Targets old firmware (VID `0x1D50` PID `0x6018`) — not the hardware we are using.** Consulted for context only; do not use its VID/PID or endpoint assumptions. |
| DevKitPro | https://devkitpro.org | Wii/GameCube homebrew toolchain (devkitPPC compiler, CMake integration). |
| libogc | https://github.com/devkitPro/libogc | Wii runtime library. Provides USB bulk transfer APIs (`USB_GetDeviceList`, `USB_OpenDevice`, `USB_WriteBlkMsg`, `USB_ReadBlkMsg`, `USB_WriteCtrlMsg`, `USB_CloseDevice`), DMA cache coherency macros (`DCFlushRange`, `DCInvalidateRange`), video, and controller input. |
| libfat | https://github.com/devkitPro/libfat | FAT filesystem driver for SD and USB mass storage. Used for all SD file I/O (`fatMountSimple`, stdio `fopen`/`fwrite`/`fclose`). |
| mGBA | https://github.com/mgba-emu/mgba | Game Boy / GBC / GBA emulator. Vendored at `vendor/mgba/` as the Phase 3 emulation core. API used: `mCoreFind`, `mCoreCreate`, `mCoreLoadFile`, `mCoreCallbacks.savedataUpdated`, `blip_buf` audio ring buffer, GBA/GB core headers. Link cable design: `include/mgba/gba/interface.h` (`struct GBASIODriver`, `GBASIOSetDriver`), `include/mgba/internal/gba/sio.h` (`struct GBASIODriverSet`, `SIO_JOYBUS` enum), `include/mgba/gb/interface.h` (`struct GBSIODriver`, `GBSIOSetDriver`), `src/gba/sio/dolphin.c` (JoyBus TCP driver — template for SI_Transfer adaptation), `src/gba/sio/lockstep.c` (multiplayer lockstep — template for Wi-Fi link), `src/gb/sio/lockstep.c` (GB lockstep). |
| mGBA SIO dolphin driver | https://github.com/mgba-emu/mgba/blob/master/src/gba/sio/dolphin.c | mGBA's JoyBus SIO driver for Dolphin emulator connectivity. Uses two TCP sockets (clock port 49420, data port 54970) and processes JoyBus commands 0xFF/0x00/0x14/0x15 via `GBASIOJOYSendCommand`. Direct template for the DOL-011 hardware JoyBus path: replace TCP with libogc `SI_Transfer`. |
| gba-link-cable-rom-sender | https://github.com/FIX94/gba-link-cable-rom-sender | Wii homebrew that uses SI_Transfer to send GBA ROM data via JoyBus over DOL-011. Reference for libogc SI_Transfer call patterns (command buffer layout, `SI_GetTypeAsync` for GBA detection, 50µs inter-command delay). |
| afska/gba-link-connection | https://github.com/afska/gba-link-connection | GBA-side link cable library. `LinkCube.hpp` documents the GBA JoyBus slave register interface: RCNT bits 14–15=1 to enter joybus mode, REG_JOYCNT (0x4000140), REG_JOYSTAT (0x4000158), JOY_RECV_L/H (0x4000150/52), JOY_TRANS_L/H (0x4000154/56). Confirms GBA is always slave in JoyBus. |
| GBATEK SIO JoyBus | https://problemkaputt.de/gbatek-sio-joy-bus-mode.htm | Hardware reference for the GBA JoyBus register interface and protocol. JoyBus command byte encoding (0xFF reset, 0x00 poll, 0x14 read 4B, 0x15 write 4B), 32-bit transfer format, and RCNT/JOYCNT register semantics. |
| Pan Docs Serial Transfer | https://gbdev.io/pandocs/Serial_Data_Transfer_(Link_Cable).html | Reference for GB/GBC SIO protocol: 8-bit transfers, $FF01 (SB data) and $FF02 (SC control) registers, master/slave clock selection, transfer timing (512 cycles normal / 16 cycles GBC high-speed). Informs `GBSIODriver.writeSB`/`writeSC` hook design. |
| Celio-Client | https://github.com/Celio-Link/Celio-Client | Online Pokémon Gen 3 trading client (browser-side). Uses WebUSB/WebSerial to talk to Celio-Firmware dongle. Network protocol to Celio-Server not published. Consulted for architecture reference only; not directly adaptable to Wii. |
| Celio-Firmware | https://github.com/Celio-Link/Celio-Firmware | RP2040 firmware for the Celio-Link custom dongle. Connects to a physical GBA via AGB-005 link cable (SO/SI/SC pins — GBA multiplayer SIO, NOT JoyBus). Targets Gen 3 GBA Pokémon only. Reference for understanding GBA multiplayer SIO signal requirements. Not compatible with DOL-011 (which uses only SD/JoyBus). |
| pret/pokefirered | https://github.com/pret/pokefirered | FRLG decompilation. Primary reference for trade legality rules (`src/trade.c` `CanTradeSelectedMon()`), flag constants (`include/constants/flags.h`: `FLAG_SYS_NATIONAL_DEX`=0x840, `FLAG_SYS_CAN_LINK_WITH_RS`=0x844), and FRLG save layout (party at Section 1 offset 0x0038, 100 bytes/slot, 6 slots). Confirms National Dex and Celio quest are independent flags. |
| pret/pokeemerald | https://github.com/pret/pokeemerald | RSE decompilation. Cross-reference for `CanTradeSelectedMon()` (same trade rules as FRLG), RSE party offset (Section 1 offset 0x0238), and save section layout. |
| Bulbapedia Pokémon data structure Gen III | https://bulbapedia.bulbagarden.net/wiki/Pok%C3%A9mon_data_structure_(Generation_III) | Authoritative reference for the 100-byte GBA party Pokémon structure: 32-byte outer header (PV, OT ID, nickname 10B, OT name 7B, marks, language, bad egg/obedience, checksum), 48-byte XOR-encrypted block (key = full OT ID u32). Four 12-byte substructures in permutation order `PV % 24`. Growth substructure: species, item, XP, friendship. Attacks: moves + PP. EVs/Condition: EVs, contest. Misc: Origins Info (game-of-origin bits 7–10; 15=Colosseum/XD), IVs/Egg/Ability (bit 30 = Egg flag, bit 31 = MODERN_FATEFUL_ENCOUNTER). Per-Pokémon checksum: sum of all 24 u16 words in decrypted 48-byte block, take low 16 bits. |
| Bulbapedia Save data structure Gen III | https://bulbapedia.bulbagarden.net/wiki/Save_data_structure_(Generation_III) | Gen 3 save file layout: 128KB Flash, two alternating 57,344-byte save slots, 14 sections × 4,096 bytes each. Section structure: 3,968-byte data area, section ID (u16), checksum (u16), validation (u32=0x08012025), save index (u32). Per-section checksum: sum all u32 words in data; add upper and lower u16 halves of 32-bit result; take low 16 bits. Party data in Section 1 (ID=1): FRLG offset 0x0038, RSE offset 0x0238. |
| Bulbapedia Pokémon Colosseum | https://bulbapedia.bulbagarden.net/wiki/Pok%C3%A9mon_Colosseum | Reference for Colosseum GBA trade rules: requires completion of Story Mode, trade via Pokémon Center in Phenac City basement, Shadow Pokémon must be fully purified before they can be traded to GBA, Eggs cannot be sent to or received from GCN games, English GCN ↔ English GBA language restriction. |
| Bulbapedia Pokémon XD | https://bulbapedia.bulbagarden.net/wiki/Pok%C3%A9mon_XD:_Gale_of_Darkness | Reference for XD GBA trade rules (same as Colosseum). Purification and MODERN_FATEFUL_ENCOUNTER requirements for Shadow Pokémon. |
| pret/colosseum-mb | https://github.com/pret/colosseum-mb | Full C decompilation of the GBA-side multiboot ROM that Colosseum/XD sends to the GBA during a trade. This is the primary reference for the JoyBus trade protocol GBA side. Key file: `payload/src/unk_200C5DC.c` — implements `JoyBusVCOuntIntr`, all LINK_CMD_* handlers (`LINK_CMD_RESET`, `LINK_CMD_READ_INPUT`, `LINK_CMD_TRAN_PLAYER_DATA1/2`, `LINK_CMD_RECV_TEXT`, `LINK_CMD_RECV_MON_DATA`, `LINK_CMD_RECV_PARTY_MON`, `LINK_CMD_RECV_GIFT_DATA`, `LINK_CMD_TRAN_GIFT_DATA`, `LINK_CMD_SOFT_RESET`). GBA JoyBus slave setup: `REG_RCNT = 0xC000` (bits 14–15 = JoyBus mode), `REG_JOYCNT |= JOYCNT_IRQ_ENABLE`. Game type detection: reads ROM header at 0x80000AC to distinguish FRLG vs RSE, selects party save offset accordingly. colosseum-mb reads the physical cart save directly via `agb_flash.c`/`libpmagb`, not via the retail game. **The retail GBA game is NOT running during a Colosseum/XD trade.** |
| Wack0/gba-gen3multiboot | https://github.com/Wack0/gba-gen3multiboot | Wii/GC homebrew that sends the Gen3 multiboot ROM to a GBA via SI_Transfer. Implements the full multiboot key exchange algorithm. Usable as a template for sending colosseum-mb from the Wii to a physical GBA over DOL-011. Note: key derivation occasionally fails; may need retry loop. |
| hatkirby/gen3uploader | https://github.com/hatkirby/gen3uploader | Wii homebrew using Gen3 multiboot (same SI_Transfer approach as Wack0). More reliable implementation; "works within a few tries." Second reference for multiboot send patterns. |
| Dolphin SI_DeviceGBA | https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Core/HW/SI/SI_DeviceGBA.cpp | Dolphin's emulated GCN-side JoyBus device implementation. Uses TCP sockets (clock port 49420, data port 54970) to forward JoyBus commands to mGBA. Running Dolphin + Colosseum ISO + mGBA and capturing port 54970 with Wireshark would reveal the exact GCN-side LINK_CMD byte values and command sequence — the primary remaining unknown for the middleman implementation. |
| Deokishisu GC link requirements | https://gist.github.com/Deokishisu/c9abef7d41dbd898194197c2509c62fd | Documents which in-game flags the GBA game must have set for GCN link features to work. XD specifically requires `FLAG_SYS_GAME_CLEAR` from the GBA side in addition to the Colosseum requirements. |
| AxioDL/jbus | https://github.com/AxioDL/jbus | JoyBus TCP server library (same protocol as Dolphin, port 54970). Alternative reference for the Dolphin ↔ GBA TCP data format. |

---

## Architecture

```
main.c
  └─ gb_operator.c          USB bulk driver (complete)
  └─ rom_cache.c            SD/USB ROM file cache (complete)
  └─ mgba_frontend.c        mGBA Wii frontend — video, audio, input, save hook (Phase 3)
  └─ cart_sync.c            Background LWP thread for save-to-cart upload (Phase 3)
  └─ dol_reload.c           In-place heap reset for successive sessions (Phase 3)
  └─ vendor/mgba/           mGBA source tree vendored in-tree (Phase 3)
```

### File Roles
- **`source/main.c`** — entry point; inits video, Wiimote, filesystem; orchestrates cart ops and mGBA launch; all logging to `sd:/apps/wii-gb-operator/log.txt`
- **`source/gb_operator.h/.c`** — all USB communication with the GB Operator; owns device lifecycle
- **`source/rom_cache.h/.c`** — builds filenames from cart title/code, reads/writes ROM chunks to `sd:/apps/wii-gb-operator/roms/`
- **`source/mgba_frontend.h/.c`** — wraps mGBA core; owns emulation loop, GX video, AESND audio, GC controller input, save dirty callback, Reset button hook
- **`source/cart_sync.h/.c`** — background LWP thread that receives queued save buffers and calls `gbop_write_save()`; owns retry logic and notification state
- **`source/dol_reload.h/.c`** — in-place app restart after mGBA session; zeros BSS (resetting dlmalloc and all statics), then branches directly to `main()`. Preserves GX CP state and initial SP via `.data` statics across the BSS zero.
- **`vendor/mgba/`** — mGBA source vendored in-tree; modifications tracked in this repo; base version recorded in `vendor/mgba/VENDOR.md`

---

## Build System

**Toolchain:** DevKitPro (devkitPPC). Requires `DEVKITPRO` environment variable set.

```bash
# Configure
cmake -S . -B build -G "Unix Makefiles"

# Build
cmake --build build -j4

# Output: build/wii-gb-operator.dol
# Deploy: copy DOL to SD card at /apps/wii-gb-operator/boot.dol
```

VSCode tasks are configured for all three steps (configure, build, copy to SD).

**Links against:** `ogc` (libogc — Wii runtime + USB APIs), `fat` (libfat — SD/USB filesystem), `mgba` (mGBA core — vendored static lib, Phase 3)

---

## Implementation Notes

- ROM cache filenames are sanitized from cart title + game code; `.gb` / `.gba` extension based on cart type
- Cache check compares file size to expected `rom_size_kb * 1024` — not a hash
- ROM is dumped in 4KB chunks (`DUMP_CHUNK_SIZE = 4096`) looped across full ROM size
- SD is always preferred over USB for both cache reads and writes; USB is fallback
- Cart header location: GB/GBC at `0x100–0x14F`, GBA at `0x00–0xBF`

---

## mGBA Integration (Phase 3)

### Intent
mGBA is compiled into the same binary — no separate DOL, no channel switch. The Wii reset button exits emulation and returns to the wii-gb-operator main menu. Every action (ROM load, save event, cart sync result) is logged to the same `sd:/apps/wii-gb-operator/log.txt`.

### User Flow
1. From the main menu (after reading cart info), the user may select **Launch mGBA**.
2. A ROM browser lists all files in `sd:/apps/wii-gb-operator/roms/` (`.gb`, `.gbc`, `.gba`).
3. After selecting a ROM, a save browser lists matching save files from `sd:/apps/wii-gb-operator/saves/`. The user may also choose "No save / new game."
4. mGBA boots with the selected ROM and save. Emulation begins immediately.
5. The player plays normally (see Save Sync Design below for how saves are handled per cart type).
6. Pressing SELECT opens the OSD overlay menu (see OSD Overlay below).
7. Pressing the Wii physical Reset button exits the emulation loop and returns to the main menu. If an unsaved-but-unsynced state exists for GB/C, a Y/N prompt warns the player before exit.

### Save Sync Design

Save handling differs by cart type because GBA SRAM is written atomically (one save event per save action) while GBC games continuously dirty SRAM via hardware RTC, making it impossible to tell a real save from a clock tick without comparing total SRAM drift since the last known-good sync.

**GBA (automatic):**
- `savedataUpdated` threshold = 1 (any change is treated as a save).
- Every save event queues a background cart write on the LWP thread. Emulation is never paused.
- Save indicator dot appears in-game (green dot, bottom-right) while syncing and after success.
- Player never needs to interact with the OSD to sync — it happens automatically.
- OSD "Sync to Cart" remains available as a manual override.
- 10s cooldown after each sync prevents hammering on games that update SRAM frequently.

**GB/GBC (manual, with automatic detection assist):**
- Two comparison baselines are maintained:
  - `s_sync_snapshot` — rolling, updated every 30 frames of `savedataUpdated`; used by `is_real_save` check (threshold = 64 bytes for CGB, higher for pure GB) to trigger **auto-sync** when a large single-event SRAM change is detected (e.g. a fresh save from a GB game that only writes on explicit save).
  - `s_stable_baseline` — initialized to on-disk save at session load; updated only on `on_cart_sync_success`. Accumulates total SRAM drift since last confirmed sync. If diff ≥ 512 bytes, sets `s_new_save_since_sync = 1`. This detects Pokémon Gold/Silver saves that arrive in small RTC-sized increments across many `savedataUpdated` firings.
- **No save indicator dot** is shown for GB/C (RTC background writes cause too many false positives).
- OSD "Sync to Cart" is the primary cart-write mechanism for GB/C. It is gated by `s_new_save_since_sync`: if 0, shows "Save in-game first, then sync" and does nothing. If 1, performs a cart write and clears the flag on success.
- After a successful sync, the post-sync guard re-arms: if the player continues playing and then tries to sync again without an intervening in-game save, the same "Save in-game first" message appears.

**SD is always authoritative for both cart types.** The sync thread writes the save to `sd:/apps/wii-gb-operator/saves/<name>.sav` after every successful cart write. The final save on teardown (`savedataClone` → fwrite) is a separate failsafe that runs if the sync thread's SD write was skipped (e.g. due to shutdown timing). The SD file is never contingent on cart sync success.

**Retry on failure.** If `gbop_write_save()` fails, the sync thread retries with a 3s delay between attempts. Retries continue until success or shutdown.

### OSD Overlay
Opened with SELECT. Items:
0. **Sync to Cart** — GB/C: gated by `s_new_save_since_sync`; blocked with hint if no save detected since last sync or if save+sync then continued play without saving. GBA: always allowed as manual override.
1. *(reserved for future ROM/save browser navigation)*
2. **Quit** — exits emulation. For GB/C, if `s_player_saved_ingame && s_new_save_since_sync`, shows Y/N warning: "You saved in-game but haven't synced to the cartridge yet. Your save is backed up on SD." A: exit anyway. B: return to OSD to sync first. If saved+synced (or GBA auto-synced), exits silently.

Session status line shown in OSD:
- "Not saved yet" — `!s_player_saved_ingame`
- "Saved (not synced)" — `s_new_save_since_sync == 1`
- "Saved + Synced" — `s_synced_this_session && !s_new_save_since_sync`

### Save Indicator (GBA only)
Small dot rendered on the game framebuffer, bottom-right corner. States driven by `cart_sync_state()`:
- *(idle)* — not shown
- **syncing** — yellow/orange dot
- **success** — green dot, shown for ~2 seconds (120 frames), then clears
- **failed** — red dot, shown persistently while retry is pending

### Threading Model
| Thread | Content | Priority |
|--------|---------|----------|
| Main thread | mGBA emulation loop, GX video render, overlay draw | Normal |
| AESND callback | Audio mixing, pulled by Wii DSP | High (system) |
| Cart sync LWP | `gbop_write_save()` + retry loop | Low (background) |

The cart sync thread owns the GB Operator handle exclusively during upload. The main thread must not call any `gbop_*` functions while the sync thread is active. A mutex guards the handle.

### mGBA Frontend (mgba_frontend.c)
- **Core init**: `mCoreFind()` → `mCoreCreate()` → load ROM → load save → `mCoreRun()`
- **Video**: GX, nearest-neighbor scale of GBA native 240×160 to TV output. Letterboxed on 4:3; pillarboxed on 16:9.
- **Audio**: AESND double-buffer at 48kHz. mGBA audio core configured to match.
- **Input**: GC controller (PAD). Mapping: A→A, B→B, Z→Select, Start→Start, D-pad→D-pad, L→L, R→R, X/Y unused initially.
- **Reset hook**: `SYS_SetResetCallback(on_reset)`. `on_reset` sets `g_exit_emulation = 1`. Emulation loop checks flag each frame.
- **Save hook**: `mCoreCallbacks.savedataUpdated` → copies save buffer to shared memory, signals cart sync thread via `LWP_SemPost`.

### mGBA Source (vendor/mgba/)
- Vendored in-tree (not a git submodule) so modifications are tracked in this repo.
- Base version and any local changes documented in `vendor/mgba/VENDOR.md`.
- CMake builds `vendor/mgba/` as a static library target (`mgba`) linked into the main executable.
- Do not add mGBA upstream changes without updating `VENDOR.md`.

### Phase 3 Implementation Order
1. ~~CMake: add `vendor/mgba/` as static lib target, verify build links~~ ✓
2. ~~`source/mgba_frontend.c`: core init, ROM load, minimal emulation loop~~ ✓
3. ~~GX video output: render GBA framebuffer to TV~~ ✓
4. ~~AESND audio: wire mGBA audio core to Wii DSP~~ ✓
5. ~~GC input: map controller to mGBA buttons~~ ✓
6. ~~Reset button: `SYS_SetResetCallback` → exit loop → return to main menu~~ ✓
7. ~~`source/cart_sync.c`: LWP thread, mutex, retry loop~~ ✓
8. ~~Save hook: `savedataUpdated` callback → signal sync thread~~ ✓
9. ~~OSD overlay (SELECT): sync control, session status, quit with unsaved-warn~~ ✓
10. ~~`source/main.c`: add "Launch mGBA" menu option, ROM browser, save browser~~ ✓
11. ~~Custom border system: load `border_<CODE>.bmp` from SD for CGB games~~ ✓
12. ~~GB/C manual-sync workflow: stable baseline, `s_new_save_since_sync` gate~~ ✓
13. ~~Successive-session heap corruption: skip `core->deinit()` in all paths; `while(1)` in `main()` handles successive sessions~~ ✓ (confirmed test_90/91)
14. ~~Main menu: Commit Log + Exit to Menu options; automatic log_commit_sd after mGBA session; log_force_flush fix at cart-info checkpoint~~ ✓
15. ~~GBA sync thread crash (intermittent) — not reproduced across extended stress testing (test_91/122); considered resolved for now~~ ✓
16. ~~GB SGB border slow load — inherent SGB protocol delay in mGBA core; low priority, deferred~~ ✓ (deferred, not a blocker)
17. ~~→ Phase 4 begins~~ ✓ (underway — see Phase 4 Plans above)

---

## Do Not
- Refactor `rom_cache.c` — it is complete and correct
- Use interrupt transfer APIs (`USB_WriteIntrMsg`/`USB_ReadIntrMsg`) for GB Operator — it uses bulk transfers
- Call any `gbop_*` function from the main thread while the cart sync LWP thread is active — the GB Operator handle is not thread-safe; the sync thread owns it exclusively during upload
- Call `GX_Init` more than once per process lifetime — `g_gx_initialized` guards against this. The GX CP stays armed to `s_gxfifo_buf` between sessions; a second `GX_Init` on a live CP causes crashes. Do not remove this guard. Do not make `g_gx_initialized` static again — `dol_reload.c` must be able to preserve it across a BSS zero.
- Free or move `s_gxfifo_buf` — it is a `.data` static array at a fixed address. The GX CP stays armed to that address permanently; reallocating or freeing it causes machine check exceptions. It must remain a `.data` static for its lifetime.
- Store values that must survive a BSS zero in local variables or BSS statics — on this Wii, the initial SP falls inside the BSS range (both `.sbss` and `.bss` are zeroed by `dol_reload`), meaning the live call stack is inside the zeroed range. Use `.data` statics with non-zero initialisers instead (`__attribute__((section(".data")))`). All `.data` addresses must be below `__sbss_start` to be safe.
- Zero only `.bss` (from `__bss_start`) in `dol_reload` — `.sbss` (small BSS, below `__bss_start`) contains libogc's `s_firstThread` (LWP thread-list head) and `postRetraceCB` (VI retrace callback). Leaving `.sbss` unzeroed leaves these pointing into the now-zeroed `.bss` TCB pool → DSI in `LWP_CreateThread` or VI interrupt. Always zero from `__sbss_start` through `__bss_end`.
- Leave `MSR[EE]` (external interrupt enable) disabled after `dol_reload` branches to `main()` — `_CPU_ISR_Disable` clears it for the memset; it must be restored (`ori MSR, 0x8000; mtmsr; isync`) before `b main`. Without this, `VIDEO_WaitVSync()` in `init_video()` spins forever.
- Branch to the CRT entry point (`_start`) during `dol_reload` — the CRT calls `SYSTEM_Init()` which reinitialises IOS on already-live hardware → DSI crash. Branch directly to `main()` instead.
- Call `core->deinit(core)` anywhere in `mgba_frontend.c` — it corrupts dlmalloc's free list when freeing mGBA's internal allocations (WRAM 288KB + VRAM 96KB + audio ~130KB). The next `malloc()` call (including inside `fopen`) follows a poisoned free-list pointer to 0x10000000 (physical MEM2 base, not mapped) → DSI. Confirmed across tests 84-89. Null `s_core` and leave mGBA internals allocated; ~640KB leaks per session but the heap stays intact for the next while(1) iteration in `main()`.
- Call `log_commit_sd()` at "Session ended" in `mgba_frontend.c` — use `log_force_flush()` there instead (no malloc involved). `fopen()` inside `log_commit_sd()` calls `malloc()` which crashes on the corrupted heap if it runs before the skip-deinit fix is in place. Even with the fix, prefer `log_force_flush()` in teardown paths.
- Add a final `USB_ReadBlkMsg(4)` probe at the end of `gbop_write_save` for GB/C — this read blocks longer than the 4.5s `cart_sync_shutdown` timeout, causing thread abandonment and a use-after-free race in the next session. All protocol data is already transmitted after the last per-chunk ACK. The GBA path does not have this problem (GBA probe is quick).
- Read a save immediately after writing without a close/reopen cycle and appropriate delay — the device is not ready for a new command until the USB handle is cycled; attempting a read on the same open handle after a write causes an indefinite `USB_ReadBlkMsg` hang. GBC needs 200ms delay after reopen; GBA needs 30s for Flash erase+program.
- Call `log_commit_sd()` (fclose+fopen on g_log) from the emulation loop or anywhere while the cart_sync LWP thread is alive — fclose frees the FILE's internal stdio buffer while the sync thread may be inside vfprintf on the same FILE, causing a heap use-after-free that corrupts the malloc free list and causes future malloc calls to hang. Use `log_force_flush()` (fflush only) during emulation.
- **Use any external source (repository, documentation, specification, or web resource) to inform implementation without first adding it to the Sources table.** This applies to protocol details, API behavior, hardware specs, and reference implementations — not to general language/stdlib knowledge.

---

## Pokémon Box Wii (PBW)

A separate homebrew project that grew out of wii-gb-operator. PBW provides Pokémon Box-style save management for Gen 1–3 games running on the Wii via the GB Operator. It is developed in its own GitHub repository (`pokemon-box-wii`) and must NOT be merged into or built from the wii-gb-operator repository. The wii-gb-operator repo is archived (hardware-verified, complete) and is never modified by PBW work.

**SD path:** `sd:/apps/pokemon-box-wii/`

### Main Menu

Three options on boot:
1. **Box** — open the box management UI (primary feature)
2. **Adventure** — boot a specific game from PBW storage (plays via mGBA integration; player has access to their full PBW box storage during the session)
3. **Options** — display scale, storage device preference, and other settings

### Box Storage Structure

- **14 PBW boxes**, each holding 30 Gen 3 Pokémon slots = 420 total
- Boxes are displayed as **7 pairs** (two visible at a time); L/R cycles through pair index 0–6
- Gen 1 and Gen 2 Pokémon are stored separately in Gen 2 format boxes (20-slot boxes); they live in their own storage pool pending PCCS migration to Gen 3
- The cart row at the bottom of the Box screen shows up to 30 Pokémon currently in the connected cartridge (read via GB Operator) using animated icon sprites

**Subsidiary saves (PBW box persistence):** PBW boxes are stored as blank Emerald saves (PKHeX-compatible), 14 boxes × 30 = 420 slots per file. File: `sd:/apps/pokemon-box-wii/saves/pbw_main.sav`. Any PKHeX-compatible Gen 3 tool can read or validate this file directly.

### Box Screen Layout (GX framebuffer — 640px wide)

- **32px safe-area buffer** on all sides (Wii overscan)
- **Controls bar at top:** L/R pair navigation with "Pair X/7" label; hint badges for A (context menu), X (grab mode), Y (quick move); **Save & Exit** button top-right (no Cancel button)
- **Two 6×5 PBW boxes** side by side below the controls bar (30 slots each, `aspect-ratio:1` slots)
- **Box header** for each: box name (click to rename inline) + "Box X/14" counter. No navigation arrows on the headers — L/R on the controls bar handles all pair cycling.
- **"Stored XXX/420"** total count shown below or adjacent to box pair
- **Cart icon row** at the bottom: animated icon sprites (2-frame canvas animation, 500ms toggle) from the connected cartridge or selected save file

### Slot Interaction Model

- **Single-click** to pick up a Pokémon (sets `selected`; slot highlights)
- **Single-click on destination** to place or swap (atomically swaps slot contents; marks `dirty = true`)
- **Right-click** opens a context menu with: **Move** (pick up), **Summary** (show summary panel). No Migrate option; no Sort Box option.
- **Summary panel:** shows full battle sprite + Level, Nature, OT, ID No.
- **Sprite transition:** Pokémon in the cart row use animated icon sprites. When picked up (selected), the cursor display transitions to the full-size battle sprite.

### Inline Box Rename

Clicking a box name converts it to an `<input>` field in place. Enter confirms, Escape cancels, blur confirms. Name is bound to `boxNames[pairIdx*2]` or `boxNames[pairIdx*2+1]`. Names persist to the subsidiary save file header or a separate metadata file.

### Dirty State and Save & Exit

- `dirty = true` on any slot swap or box rename
- **Save & Exit:** if `dirty`, shows an "unsaved changes" confirmation modal before exiting. No background auto-save; the player must confirm Save & Exit to commit changes.
- No Cancel button — the only exit path is Save & Exit (saves and exits) or the modal's "go back" option.

### Sprite Naming Conventions

All sprite files live in `sprites/` at the repo root. Names are **UPPERCASE** with `.png` extension.

| Folder | Content | Frame format |
|--------|---------|-------------|
| `sprites/gen3/` | Full battle sprites, Gen 3 | Single frame |
| `sprites/gen3-shiny/` | Shiny full battle sprites, Gen 3 | Single frame |
| `sprites/gen2/` | Full battle sprites, Gen 2 | Single frame |
| `sprites/gen2-shiny/` | Shiny full battle sprites, Gen 2 | Single frame |
| `sprites/gen3-Icons/` | Icon sprites, Gen 3 | 2 frames **horizontal** (frame 0 left, frame 1 right; split at `naturalWidth/2`) |
| `sprites/gen2-Icons/` | Icon sprites, Gen 2 | 2 frames **vertical** (frame 0 top, frame 1 bottom; split at `naturalHeight/2`) — **folder currently empty** |

**Gen 2 icon download:** `https://github.com/cRz-Shadows/Pokemon_Crystal_Legacy/tree/main/gfx/icons`

Icon animation renders via `<canvas>` (48×48 display). Every 500ms, `frame` toggles 0↔1 and `drawImage` redraws with the correct x-offset (gen3: `frame * naturalWidth/2`) or y-offset (gen2: `frame * naturalHeight/2`). `imageSmoothingEnabled = false` for pixel-accurate rendering.

### PCCS Legal Mode — Gen 2 → Gen 3 Conversion

This is a one-way migration only (no Gen 3 → Gen 2 conversion is planned). **Legal** is the preferred standard (not Faithful) — the goal is a Pokémon that is legal in Gen 3 without requiring exact reproduction of Gen 2 internal values.

PCCS Legal mode is not yet implemented in the upstream PCCS project; PBW will implement from the spec below.

**PID generation (LCRNG loop):**
1. Generate a candidate PID via LCRNG.
2. Accept if `PID % 25 == target_nature` (nature determination).
3. Accept if gender DV threshold satisfied: gender ratio byte from species data; female if `DV_ATK % 8 < ratio_threshold`, male otherwise — PID gender bit must match Gen 2 gender.
4. Accept if ability bit (PID bit 0) matches desired ability.
5. For Unown: accept if `((PID >> 28 & 3) << 6 | (PID >> 24 & 3) << 4 | (PID >> 20 & 3) << 2 | (PID >> 16 & 3)) >> 2` matches the letter encoded in Gen 2 DVs.
6. Loop until all constraints satisfied; no iteration cap (legal values always exist).

**Shininess (SID calculation):**
- Gen 2 shiny condition: `DV_ATK == 10 && DV_DEF == 10 && DV_SPD == 10 && DV_SPC == 10`. If shiny: choose TID (from Gen 2 save), generate PID, compute `SID = TID ^ (PID & 0xFFFF) ^ (PID >> 16)` such that `(TID ^ SID ^ (PID & 0xFFFF) ^ (PID >> 16)) < 8` (shiny XOR threshold). Assign that SID to the Gen 3 trainer profile.
- Non-shiny: assign `SID = 51691` (arbitrary value that will not accidentally create shininess with any common TID/PID combination) or `SID = 0`.

**EV conversion (proportional):**
- Gen 2 uses 0–65535 StatExp per stat. Compute `total = sum(all 6 StatExp values)`.
- If `total == 0`: all Gen 3 EVs = 0.
- If `total > 0`: `EV[i] = min(255, floor(StatExp[i] / total × 510))`. All-maxed Gen 2 (total = 393210) → 85 per stat (6 × 85 = 510).
- Gen 3 EV sum cap = 510 enforced automatically by the proportional formula.

**EXP:** Copied exactly from Gen 2 to Gen 3 (no conversion needed; both use the same EXP growth curves for the same species).

**Nature:** Determined by the LCRNG loop (PID % 25); not read from Gen 2 (Gen 2 has no nature concept).

**Held items:** Before migration, attempt to return the held item to the Gen 2 trainer bag. If bag is full, place in PC item storage. If both full, block the migration and inform the player — the item must be manually freed first. The item is never silently discarded.

**Korean nicknames:** If a Pokémon has a Korean-encoded nickname that cannot be represented in the target Gen 3 language encoding (e.g. English cartridge), prompt the player to enter a new nickname before proceeding. Default to the species name in the target language if the player skips.

### Box UI Mockup (Web Artifact)

A browser-based interactive mockup of the Box screen exists at:
**`https://claude.ai/code/artifact/3e619d84-1fbe-4811-ac71-34928dbfa1c0`**

Implements: 640px Wii-width canvas, 32px safe area, controls bar with L/R pair cycling, two 6×5 PBW box grids with full battle sprites, animated icon canvas row for cart Pokémon, right-click context menu (Move + Summary), summary panel, inline box rename, Save & Exit with unsaved-changes modal, dirty-state tracking. Source file: `pbw_box_ui.html` in the session scratchpad. Embedded sprite data (`sprites.js`) includes 10 full sprites + 10 icon sprites for: Bulbasaur, Pikachu, Charizard, Gengar, Eevee, Snorlax, Gyarados, Mewtwo, Arcanine, Alakazam.

### PBW — What Is Not Yet Decided

- Exact SD file layout for Gen 1/2 box storage (Gen 2 format; 20-slot boxes)
- Adventure mode integration with mGBA (how to wire PBW box access into the emulation session)
- Link cable trading from Adventure mode (deferred — see Phase 4 item 5 in wii-gb-operator)
- Box background art, cursor sprite, home screen background (all placeholder — user will supply art; generate named template BMPs when requested)
- Gen 2 icon sprite download and integration (Crystal Legacy repo, see above)
- Wii GX rendering pipeline for the Box screen (to be ported from wii-gb-operator's GX setup)
