# SmolTV Slots

A custom 3-reel slot machine game for the GeekMagic **Small TV Pro** (ESP32, ST7789 240×240).

This project was built utilizing [smolbase](https://github.com/sweetlilmre/smolbase) as its foundation, which provides the underlying OS plumbing including the captive portal provisioning, OTA (Over The Air) updates, and the LittleFS web server.

## Documentation
* **[design.md](design.md)** - Blueprint for the LVGL UI layout, game state machine, and hardware constraints.
* **[agents.md](agents.md)** - Context guide for AI agents working in this repository.

## Quick start

1. **Build** — requires [PlatformIO](https://platformio.org/):

   ```
   pio run            # build the firmware
   pio run -t buildfs # build the LittleFS image (packs html/ as gzip-only assets)
   ```

2. **Flashing Firmware**
   - **From stock/blank device:** Wire a 3.3V USB-TTL adapter to the internal header and run `pio run -t upload -t uploadfs --upload-port <YOUR_PORT>`.
   - **From an existing smolbase/smoltv-pro install:** Over-the-air (OTA) updates are supported. Go to the device's IP address in your browser, open the Settings page, and upload the `firmware.bin` and `littlefs.bin` directly via the Update tab.

3. **Provisioning** 
   If your device loses its Wi-Fi credentials (or is wiped), it will broadcast a hotspot named `smolbase-XXXX`. Connect to it, and a captive portal will appear to let you enter your home Wi-Fi details.

## Hardware Constraints
- **Zero PSRAM:** The ESP32 relies strictly on internal SRAM.
- **Single Input:** There is only one capacitive touch button on the top of the device (GPIO 32).
- **No Audio:** Visual animations (LVGL) must handle all jackpot feedback.
