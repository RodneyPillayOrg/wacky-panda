# Wacky Panda (ESP32) — Agent & Developer Guide

**Wacky Panda** is an embedded recreation of Microgaming's classic 3-reel, 1-payline slot game **Wacky Panda**, built for both:
1. **Xiaozhi ESP32-S3 AI Board** (`[env:xiaozhi_s3]` — with real-time I2S bamboo marimba & casino sound effects and physical button input)
2. **GeekMagic SmallTV Pro** (`[env:smalltv_pro]` — 240×240 ST7789V IPS display, capacitive touch input, Wi-Fi OTA)

---

## 1. Key Features

- **Authentic Wacky Panda Visuals**:
  - Bamboo stalk frame with corner rope lashings, grass thatch roof overhang, and woven bamboo mat reel backdrop.
  - All 8 iconic symbols rendered at 45–50 FPS via `TFT_eSPI`:
    1. **Red Panda** *(wearing a green watermelon hat — pays on 1×, 2×, or 3× up to 3,333 coins!)*
    2. **Yellow Panda** *(crying/laughing with tear splashes — 900 coins)*
    3. **Purple Panda** *(grape on head & pink heart eyes — 360 coins)*
    4. **Blue Panda** *(cool dark round sunglasses & big toothy grin — 180 coins)*
    5. **Brown Panda** *(bashful side glance — 120 coins)*
    6. **Watermelon** *(whole striped melon + red wedge — 90 coins)*
    7. **Bananas** *(golden banana bunch — 75 coins)*
    8. **Grapes** *(plump purple grape cluster with green leaf — 60 coins)*
- **Real-Time I2S Audio Synthesizer (`src/Sound.h`)**:
  - On `xiaozhi_s3`, synthesizes bamboo marimba spin riffs, woodblock reel ticks, mechanical bamboo thuds, Red Panda chimes, and pentatonic celebration fanfares on a dedicated Core 0 FreeRTOS task.
- **Exact 95.94% RTP Mathematical Model (`MATH.md`)**:
  - Matches Microgaming's published **95.94%** RTP for 3-coin max bet.

---

## 2. Build & Flash Commands

### Build for GeekMagic SmallTV Pro
```bash
pio run -e smalltv_pro
```

### Build for Xiaozhi ESP32-S3 (with I2S Audio)
```bash
pio run -e xiaozhi_s3
```

### Verify PAR Sheet & 1M-Spin Simulation
```bash
python3 scripts/slot_math.py
python3 scripts/simulate_spins.py
```
