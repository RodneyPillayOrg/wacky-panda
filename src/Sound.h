#pragma once
#include <Arduino.h>
#include <cmath>

#ifndef HAS_I2S_AUDIO
#define HAS_I2S_AUDIO 0
#endif

#if HAS_I2S_AUDIO
#include <driver/i2s_std.h>

// Default Xiaozhi ESP32-S3 I2S speaker pins (configurable via platformio.ini build_flags)
#ifndef I2S_SPK_BCLK
#define I2S_SPK_BCLK 15
#endif
#ifndef I2S_SPK_LRCK
#define I2S_SPK_LRCK 16
#endif
#ifndef I2S_SPK_DOUT
#define I2S_SPK_DOUT 7
#endif
#endif

enum class SfxType : uint8_t {
  NONE = 0,
  SPIN_START,
  REEL_TICK,
  REEL_STOP_0,
  REEL_STOP_1,
  REEL_STOP_2,
  RED_PANDA_DING,
  WIN_SMALL,
  WIN_PANDA,
  WIN_JACKPOT
};

class Sound {
public:
  static void begin() {
#if HAS_I2S_AUDIO
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    if (i2s_new_channel(&chan_cfg, &_txHandle, nullptr) != ESP_OK) {
      Serial.println("[sound] i2s_new_channel failed");
      return;
    }

    i2s_std_config_t std_cfg = {
      .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
      .gpio_cfg = {
        .mclk = I2S_GPIO_UNUSED,
        .bclk = (gpio_num_t)I2S_SPK_BCLK,
        .ws   = (gpio_num_t)I2S_SPK_LRCK,
        .dout = (gpio_num_t)I2S_SPK_DOUT,
        .din  = I2S_GPIO_UNUSED,
        .invert_flags = { false, false, false }
      }
    };

    if (i2s_channel_init_std_mode(_txHandle, &std_cfg) == ESP_OK) {
      i2s_channel_enable(_txHandle);
      _ready = true;
      _sfxQueue = xQueueCreate(8, sizeof(SfxType));
      xTaskCreatePinnedToCore(_audioTask, "sfx", 4096, nullptr, 2, nullptr, 0);
      Serial.printf("[sound] I2S audio initialized (BCLK=%d, LRCK=%d, DOUT=%d)\n",
                    I2S_SPK_BCLK, I2S_SPK_LRCK, I2S_SPK_DOUT);
    }
#endif
  }

  static void play(SfxType sfx) {
#if HAS_I2S_AUDIO
    if (_ready && _sfxQueue) {
      xQueueSend(_sfxQueue, &sfx, 0);
    }
#else
    (void)sfx;
#endif
  }

  static void playSpinStart() { play(SfxType::SPIN_START); }
  static void playReelTick()  { play(SfxType::REEL_TICK); }

  static void playReelStop(int reelIdx, bool hasRedPanda) {
    if (hasRedPanda) {
      play(SfxType::RED_PANDA_DING);
    } else if (reelIdx == 0) {
      play(SfxType::REEL_STOP_0);
    } else if (reelIdx == 1) {
      play(SfxType::REEL_STOP_1);
    } else {
      play(SfxType::REEL_STOP_2);
    }
  }

  static void playWin(bool isJackpot, bool isPandaWin) {
    if (isJackpot) {
      play(SfxType::WIN_JACKPOT);
    } else if (isPandaWin) {
      play(SfxType::WIN_PANDA);
    } else {
      play(SfxType::WIN_SMALL);
    }
  }

private:
#if HAS_I2S_AUDIO
  static constexpr uint32_t SAMPLE_RATE = 22050;
  static inline i2s_chan_handle_t _txHandle = nullptr;
  static inline QueueHandle_t _sfxQueue = nullptr;
  static inline bool _ready = false;

  // Synthesize a bamboo marimba / chime note with exponential decay
  static void _synthTone(float freqHz, uint32_t durationMs, float volume = 0.35f, float pitchSlide = 0.0f) {
    if (!_ready || !_txHandle) return;
    uint32_t totalSamples = (SAMPLE_RATE * durationMs) / 1000;
    int16_t buf[256];
    uint32_t pos = 0;
    float phase = 0.0f;

    while (pos < totalSamples) {
      uint32_t chunk = totalSamples - pos;
      if (chunk > 256) chunk = 256;

      for (uint32_t i = 0; i < chunk; ++i) {
        float t = (float)(pos + i) / (float)totalSamples;
        float env = expf(-4.5f * t); // snappy bamboo marimba envelope
        float curFreq = freqHz + pitchSlide * t;
        phase += (2.0f * 3.14159265f * curFreq) / (float)SAMPLE_RATE;
        if (phase > 6.2831853f) phase -= 6.2831853f;
        // Fundamental + woody 3rd harmonic overtone
        float wave = 0.80f * sinf(phase) + 0.20f * sinf(phase * 3.0f);
        buf[i] = (int16_t)(wave * env * volume * 32767.0f);
      }

      size_t written = 0;
      i2s_channel_write(_txHandle, buf, chunk * sizeof(int16_t), &written, pdMS_TO_TICKS(50));
      pos += chunk;
    }
  }

  static void _audioTask(void*) {
    SfxType cmd;
    for (;;) {
      if (xQueueReceive(_sfxQueue, &cmd, portMAX_DELAY) == pdTRUE) {
        switch (cmd) {
          case SfxType::SPIN_START:
            // Upbeat pentatonic bamboo riff: C5 - D5 - E5 - G5 - A5
            _synthTone(523.25f, 45, 0.30f);
            _synthTone(587.33f, 45, 0.30f);
            _synthTone(659.25f, 45, 0.32f);
            _synthTone(783.99f, 50, 0.35f);
            _synthTone(880.00f, 70, 0.38f);
            break;

          case SfxType::REEL_TICK:
            // Crisp hollow bamboo woodblock tick
            _synthTone(920.0f, 10, 0.16f, -300.0f);
            break;

          case SfxType::REEL_STOP_0:
            _synthTone(220.0f, 55, 0.40f, -80.0f);
            break;

          case SfxType::REEL_STOP_1:
            _synthTone(261.63f, 55, 0.40f, -80.0f);
            break;

          case SfxType::REEL_STOP_2:
            _synthTone(329.63f, 65, 0.42f, -90.0f);
            break;

          case SfxType::RED_PANDA_DING:
            // Cute excited panda chime!
            _synthTone(783.99f, 50, 0.38f, 150.0f);
            _synthTone(1046.50f, 95, 0.42f, 80.0f);
            break;

          case SfxType::WIN_SMALL: {
            // Cheerful fruit win jingle (C5 - E5 - G5 - C6)
            static const float notes[] = { 523.25f, 659.25f, 783.99f, 1046.50f };
            for (int i = 0; i < 4; ++i) {
              _synthTone(notes[i], (i == 3) ? 160 : 80, 0.38f);
            }
            break;
          }

          case SfxType::WIN_PANDA: {
            // Wacky Panda pentatonic celebration melody!
            static const float notes[] = {
              523.25f, 587.33f, 659.25f, 783.99f, 880.00f, 1046.50f, 880.00f, 1046.50f
            };
            for (int i = 0; i < 8; ++i) {
              _synthTone(notes[i], (i == 7) ? 220 : 85, 0.42f);
            }
            break;
          }

          case SfxType::WIN_JACKPOT: {
            // 3,333-Coin Red Panda Grand Jackpot Fanfare!
            static const float notes[] = {
              523.25f, 659.25f, 783.99f, 1046.50f,
              587.33f, 783.99f, 880.00f, 1174.66f,
              659.25f, 783.99f, 1046.50f, 1318.51f,
              1567.98f, 1318.51f, 1567.98f, 2093.00f
            };
            for (int i = 0; i < 16; ++i) {
              _synthTone(notes[i], (i == 15) ? 350 : 90, 0.45f);
            }
            break;
          }

          default:
            break;
        }
      }
    }
  }
#endif
};
