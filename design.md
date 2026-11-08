# SmolTV Slots - Design Document

## 1. Overview
The **SmolTV Slots** app is a simple, lightweight 3-reel slot machine game designed specifically for the ESP32-based SmolTV Pro. It relies on a single-button interface and runs entirely on the device's internal SRAM using the LVGL graphics library.

## 2. User Interaction
Given the lack of a touchscreen or face buttons, interaction is reduced to a single input mechanism:
* **Short Tap (Top Button):** Triggers the slot machine to spin. If the reels are currently spinning, a short tap could optionally trigger a "quick stop".
* **Long Press (Top Button):** Automatically handled by the underlying `AppBase` architecture to exit the application and return to the main launcher.

## 3. UI Layout & Graphics (LVGL)
The device features a 240x240 pixel display. 
* **Layout Structure:**
  * **Top Bar (Optional):** Current "Credits" or "Score" (if we decide to keep track of a session score).
  * **Center Area:** Three vertical columns representing the 3 slot reels. Each reel will display one active symbol in the center, and partially cut-off symbols above and below.
  * **Bottom Area:** A status text label ("Tap to Spin!" or "WINNER!").

* **Reel Implementation (LVGL):**
  * We will use either the built-in `lv_roller` widget (if we use text/icon-fonts) or a custom LVGL container masking a vertically scrolling column of images. 
  * Given the PSRAM constraints, if we use images, they will be small (e.g., 50x50 pixels) and compiled directly into C-arrays rather than loaded dynamically from the filesystem.

* **Asset Requirements:**
  * 4 to 5 unique symbols (e.g., 🍒 Cherry, 🔔 Bell, 7️⃣ Seven, 🍉 Watermelon).
  * 1 Background frame to make it look like a physical machine.

## 4. State Machine
The app will operate using a simple state machine to manage the game loop cleanly without blocking the main device execution thread:

* `STATE_IDLE`: The reels are stationary. The screen prompts the user to tap. Waiting for GPIO touch interrupt.
* `STATE_SPINNING`: The user has tapped the button. The reel graphics are scrolling vertically at high speed. A timer is started.
* `STATE_STOPPING`: The timer has elapsed. The reels are instructed to snap to their pre-calculated random endpoints with a slight bounce-back animation. They stop sequentially (Reel 1, then Reel 2, then Reel 3) for suspense.
* `STATE_RESULT`: The reels have fully stopped. The game logic evaluates the result. If it's a win, the UI flashes or displays a winning message. After a short delay, it returns to `STATE_IDLE`.

## 5. Game Logic & Math
* **Randomization:** We will use the ESP32's hardware random number generator (`esp_random()`) to calculate the stopping index (0-4) for each of the 3 reels the moment the spin begins.
* **Win Conditions:**
  * **Jackpot:** All 3 reels land on the exact same symbol.
  * **Minor Win:** 2 reels land on the same symbol.
  * **Loss:** No reels match.
* **Animations:** We will leverage LVGL's native `lv_anim_t` to handle the easing and momentum of the reels to ensure smooth motion without having to manually calculate pixel offsets in the main loop.
