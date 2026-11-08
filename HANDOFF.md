# Wacky Panda (ESP32 / ESP32-S3) — Session Handoff Guide

**Date:** 2026-11-08  
**Repository:** `/Users/rodneypillay/source/rodneypillayorg/smoltv/wacky-panda` (`https://github.com/RodneyPillayOrg/wacky-panda`)  
**Current `HEAD`:** `9a0d5fb` (`main`)  
**Open Issue for Tomorrow:** [Issue #1 — Bug: Game can get stuck on 'SPINNING...' HUD state](https://github.com/RodneyPillayOrg/wacky-panda/issues/1)

---

## 1. Hardware & Build Environment

- **Active Development Device**: Spotpear ESP32-S3 1.54-inch LCD N16R8 AI Chat Box (`sp-esp32-s3-1.54-muma` / `EN-V5 with-B-T`)
  - **PlatformIO Environment**: `[env:xiaozhi_s3]`
  - **Serial / Upload Port**: `/dev/cu.usbmodem101` (`USB-Serial/JTAG`, `115200` baud)
  - **Display**: 240×240 ST7789 (`MOSI=2, SCLK=1, CS=5, DC=4, RST=48, BL=42` active-HIGH)
  - **Touchscreen**: CST816D I2C capacitive touch controller on `Wire1` (`SDA=11, SCL=7, RST=6, INT=12`, 7-bit I2C addr `0x15`) + BOOT button (`GPIO 0`) and side buttons (`GPIO 40`, `GPIO 41`)
  - **Audio**: ES8311 I2C DAC codec on `Wire` (`SDA=15, SCL=14`, 7-bit addr `0x18`) + I2S0 (`MCLK=16, BCLK=9, WS/LRCK=45, DOUT=8`) + Speaker PA enable (`GPIO 46` HIGH = on, LOW = muted)
  - **Battery Power Hold**: `GPIO 3` (`HIGH`)
- **Legacy Target Preserved**: GeekMagic SmallTV Pro (`[env:smalltv_pro]`)
- **Build & Flash Commands**:
  ```bash
  # Build & upload to Spotpear ESP32-S3
  cd /Users/rodneypillay/source/rodneypillayorg/smoltv/wacky-panda
  SMOLTV_SKIP_PREBUILD=1 ~/.platformio/penv/bin/pio run -e xiaozhi_s3 -t upload

  # Verify SmallTV Pro build
  SMOLTV_SKIP_PREBUILD=1 ~/.platformio/penv/bin/pio run -e smalltv_pro
  ```
- **Live Runtime Serial Monitor Command (Do NOT modify `termios.HUPCL`)**:
  ```bash
  ~/.platformio/penv/bin/python3 -u -c '
  import serial, time
  port = "/dev/cu.usbmodem101"
  fd = serial.Serial(port, 115200, timeout=0.1, dsrdtr=False, rtscts=False)
  print("=== CONNECTED TO /dev/cu.usbmodem101 ===")
  t0 = time.time()
  while time.time() - t0 < 120:
      line = fd.readline()
      if line:
          s = line.decode("utf-8", errors="replace").rstrip()
          print(f"[{time.time()-t0:6.2f}s] {s}")
  '
  ```

---

## 2. What Was Completed & Verified Today

1. **Action-Tuned Wacky Panda PAR Sheet (34.34% Hit Rate, 96.65% RTP)**:
   - Implemented in [`src/apps/AppSlots.h`](file:///Users/rodneypillay/source/rodneypillayorg/smoltv/wacky-panda/src/apps/AppSlots.h), [`scripts/slot_math.py`](file:///Users/rodneypillay/source/rodneypillayorg/smoltv/wacky-panda/scripts/slot_math.py), [`scripts/simulate_spins.py`](file:///Users/rodneypillay/source/rodneypillayorg/smoltv/wacky-panda/scripts/simulate_spins.py), and documented in [`MATH.md`](file:///Users/rodneypillay/source/rodneypillayorg/smoltv/wacky-panda/MATH.md).
   - 1× Any-Position Red Panda pays `6`, 2× Any-Position Red Panda pays `15`, 3× Grapes pays `9` (`14.58%` alone), 3× Red Panda Jackpot pays `1,500`.
2. **Restored Authentic Wacky Panda Audio & 3-Voice Mixer ([`src/Sound.h`](file:///Users/rodneypillay/source/rodneypillayorg/smoltv/wacky-panda/src/Sound.h), [`src/WackyPandaAudio.h`](file:///Users/rodneypillay/source/rodneypillayorg/smoltv/wacky-panda/src/WackyPandaAudio.h))**:
   - Restored the authentic `7.35s–10.20s` Spin Gong + upbeat bamboo/guzheng spin groove (`assets/audio/sfx_spin.wav`) and `9.30s–9.62s` hollow bamboo woodblock stop clack (`assets/audio/sfx_stop.wav`).
   - Normalized `assets/audio/sfx_intro.wav` (pure bamboo forest birds/crickets/chimes) to a subtle background loop (`~28%` idle, `~12.5%` active).
   - Speaker amp (`AUDIO_PA_PIN = 46`) is strictly `LOW` (muted) in the Launcher menu and Settings screen, and `HIGH` only inside Wacky Panda.
3. **Rendering & Pre-Spin Lag Fixes ([`src/apps/AppSlots.h`](file:///Users/rodneypillay/source/rodneypillayorg/smoltv/wacky-panda/src/apps/AppSlots.h), [`src/Display.h`](file:///Users/rodneypillay/source/rodneypillayorg/smoltv/wacky-panda/src/Display.h))**:
   - Preserved `_reels[i].y = (float)(_reels[i].targetIndex * CELL_H)` *before* picking the new `targetIndex` in `_startSpin()` so the upward `easeInBack` **Reel Spin Breathe** (`-18 px` pullback staggered by `20ms` across Reels 0 → 1 → 2) starts from the visible locked symbols.
   - Added band Y-clipping (`inBand`) and `_idleDrawn` gating so idle polls run at `500 Hz` and active spin frames render in `33ms` (`30 FPS`).

---

## 3. First Priority Tomorrow: Issue #1 ("Stuck on `SPINNING...`")

**GitHub Issue:** https://github.com/RodneyPillayOrg/wacky-panda/issues/1

### Symptoms Reported at End of Session
- The game can get stuck showing `"SPINNING..."` in the bottom HUD bar (`_drawFooter()` in [`src/apps/AppSlots.h`](file:///Users/rodneypillay/source/rodneypillayorg/smoltv/wacky-panda/src/apps/AppSlots.h#L721-L723), which renders `"SPINNING..."` whenever `_gameState == GameState::SPINNING`).

### Investigation Checklist for Tomorrow
1. **Attach Live Serial Monitor First**:
   - Before flashing anything, connect the Python serial monitor to `/dev/cu.usbmodem101` and observe the `[touch-diag]` and `[spin-diag]` output when reproducing the `"SPINNING..."` state.
   - Determine whether:
     - **(A)** `AppSlots::loop()` is still running frames (`[spin-diag]` continues or stops at `f=15`), meaning one of the reels never reached `ReelState::LOCKED` so `_evaluateSpinResult()` was never called; **OR**
     - **(B)** `_startSpin()` rendered its initial frame (`Display::renderApp` at line 263 of `src/apps/AppSlots.h`, which draws `"SPINNING..."`), and then the main loop stalled or exited; **OR**
     - **(C)** All 3 reels reached `ReelState::LOCKED` before `_gameState` was evaluated, or `_idleDrawn` prevented the final `TAP TO SPIN` frame from redrawing.
2. **Inspect SUSPECT #1 — Unarmed `stopTriggerMs` on Reels 1 & 2 ([`src/apps/AppSlots.h:380-397`](file:///Users/rodneypillay/source/rodneypillayorg/smoltv/wacky-panda/src/apps/AppSlots.h#L380-L397))**:
   - In `_startSpin()`, `_reels[1].stopTriggerMs = 0` and `_reels[2].stopTriggerMs = 0`.
   - Reel `i + 1` only gets its `stopTriggerMs` set at line 388 when Reel `i` finishes `SETTLING` **and** `if (_reels[i + 1].state == ReelState::SPINNING)`.
   - If `_reels[i + 1]` is in any state other than `SPINNING` at that moment (or if `_quickStop()` / frame timing causes a mismatch), `_reels[i + 1].stopTriggerMs` remains `0` forever, or if a reel locks without chaining, `_gameState` stays `GameState::SPINNING` forever!
   - **Fix to apply tomorrow**:
     - Always arm fallback `stopTriggerMs` timestamps for all 3 reels in `_startSpin()` (`now + 720`, `now + 1350`, `now + 1980`).
     - In `_updateReels()`, when Reel `i` enters `LOCKED`, if `_reels[i + 1].state == ReelState::BREATHING || _reels[i + 1].state == ReelState::SPINNING`, set `_reels[i + 1].stopTriggerMs = now + 200`.
     - Add a safety watchdog in `_updateReels()`: if `now - _spinStartMs > 3500`, force all reels to `ReelState::LOCKED` and call `_evaluateSpinResult()`.
3. **Inspect SUSPECT #2 — `_idleDrawn` Flag When Transitioning Out of `SPINNING` / `WIN_ANIMATION` ([`src/apps/AppSlots.h:127-154`](file:///Users/rodneypillay/source/rodneypillayorg/smoltv/wacky-panda/src/apps/AppSlots.h#L127-L154))**:
   - Ensure `_idleDrawn = false;` is explicitly set whenever `_gameState` transitions to `GameState::IDLE` (both in `_evaluateSpinResult()` at line 484 and when `WIN_ANIMATION` expires at line 129) so the HUD footer is guaranteed to redraw `"TAP TO SPIN"` before `_idleDrawn` gates further idle redraws.
