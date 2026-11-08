#pragma once
#include <Arduino.h>
#include <cmath>

#ifndef HAS_I2S_AUDIO
#define HAS_I2S_AUDIO 0
#endif

#if HAS_I2S_AUDIO
#include <Wire.h>
#include <driver/i2s_std.h>

// Spotpear ESP32-S3 1.54" MUMA (sp-esp32-s3-1.54-muma) ES8311 + I2S pins
#ifndef I2S_SPK_MCLK
#define I2S_SPK_MCLK 16
#endif
#ifndef I2S_SPK_BCLK
#define I2S_SPK_BCLK 9
#endif
#ifndef I2S_SPK_LRCK
#define I2S_SPK_LRCK 45
#endif
#ifndef I2S_SPK_DOUT
#define I2S_SPK_DOUT 8
#endif
#ifndef AUDIO_PA_PIN
#define AUDIO_PA_PIN 46
#endif
#ifndef AUDIO_I2C_SDA
#define AUDIO_I2C_SDA 15
#endif
#ifndef AUDIO_I2C_SCL
#define AUDIO_I2C_SCL 14
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
    // 1. Initialize I2S Master Channel (with MCLK on GPIO 16 for ES8311)
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
        .mclk = (gpio_num_t)I2S_SPK_MCLK,
        .bclk = (gpio_num_t)I2S_SPK_BCLK,
        .ws   = (gpio_num_t)I2S_SPK_LRCK,
        .dout = (gpio_num_t)I2S_SPK_DOUT,
        .din  = I2S_GPIO_UNUSED,
        .invert_flags = { false, false, false }
      }
    };

    if (i2s_channel_init_std_mode(_txHandle, &std_cfg) == ESP_OK) {
      i2s_channel_enable(_txHandle);

      // 2. Initialize ES8311 Codec over I2C (SDA=15, SCL=14, 7-bit addr 0x18)
      _initEs8311Codec();

      // 3. Enable Speaker Power Amplifier (GPIO 46 HIGH)
      pinMode(AUDIO_PA_PIN, OUTPUT);
      digitalWrite(AUDIO_PA_PIN, HIGH);

      _ready = true;
      _sfxQueue = xQueueCreate(8, sizeof(SfxType));
      xTaskCreatePinnedToCore(_audioTask, "sfx", 4096, nullptr, 2, nullptr, 0);
      play(SfxType::SPIN_START);
      Serial.printf("[sound] ES8311 + I2S ready (MCLK=%d, BCLK=%d, WS=%d, DOUT=%d, PA=%d)\n",
                    I2S_SPK_MCLK, I2S_SPK_BCLK, I2S_SPK_LRCK, I2S_SPK_DOUT, AUDIO_PA_PIN);
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
  static constexpr uint32_t SAMPLE_RATE = 24000;
  static constexpr uint8_t ES8311_ADDR  = 0x18;
  static inline i2s_chan_handle_t _txHandle = nullptr;
  static inline QueueHandle_t _sfxQueue = nullptr;
  static inline bool _ready = false;

  static void _writeEs8311Reg(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(ES8311_ADDR);
    Wire.write(reg);
    Wire.write(val);
    Wire.endTransmission();
  }

  static void _initEs8311Codec() {
    Wire.begin(AUDIO_I2C_SDA, AUDIO_I2C_SCL, 100000);
    delay(10);
    // Standard Espressif ES8311 Slave I2S 16-bit DAC initialization sequence (256*Fs MCLK)
    _writeEs8311Reg(0x00, 0x1F); // Reset all
    delay(20);
    _writeEs8311Reg(0x00, 0x00);
    _writeEs8311Reg(0x00, 0x80); // CSM power on
    _writeEs8311Reg(0x01, 0x3F); // Enable MCLK, BCLK, DAC clocks
    _writeEs8311Reg(0x02, 0x00); // Clock mult/div for 256*Fs
    _writeEs8311Reg(0x03, 0x10);
    _writeEs8311Reg(0x04, 0x10);
    _writeEs8311Reg(0x05, 0x00);
    _writeEs8311Reg(0x06, 0x03);
    _writeEs8311Reg(0x07, 0x00);
    _writeEs8311Reg(0x08, 0xFF);
    _writeEs8311Reg(0x09, 0x0C); // SDP In: 16-bit I2S
    _writeEs8311Reg(0x0A, 0x0C); // SDP Out: 16-bit I2S
    _writeEs8311Reg(0x0D, 0x01); // Power up analog/VMID
    _writeEs8311Reg(0x0E, 0x02);
    _writeEs8311Reg(0x12, 0x00); // Power up DAC
    _writeEs8311Reg(0x13, 0x10); // Enable HP/Speaker output drive
    _writeEs8311Reg(0x1C, 0x6A);
    _writeEs8311Reg(0x32, 0xBF); // DAC volume ~80% (0xBF)
    _writeEs8311Reg(0x37, 0x08); // Unmute DAC
  }

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
        float env = expf(-4.5f * t);
        float curFreq = freqHz + pitchSlide * t;
        phase += (2.0f * 3.14159265f * curFreq) / (float)SAMPLE_RATE;
        if (phase > 6.2831853f) phase -= 6.2831853f;
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
            _synthTone(523.25f, 45, 0.30f);
            _synthTone(587.33f, 45, 0.30f);
            _synthTone(659.25f, 45, 0.32f);
            _synthTone(783.99f, 50, 0.35f);
            _synthTone(880.00f, 70, 0.38f);
            break;

          case SfxType::REEL_TICK:
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
            _synthTone(783.99f, 50, 0.38f, 150.0f);
            _synthTone(1046.50f, 95, 0.42f, 80.0f);
            break;

          case SfxType::WIN_SMALL: {
            static const float notes[] = { 523.25f, 659.25f, 783.99f, 1046.50f };
            for (int i = 0; i < 4; ++i) {
              _synthTone(notes[i], (i == 3) ? 160 : 80, 0.38f);
            }
            break;
          }

          case SfxType::WIN_PANDA: {
            static const float notes[] = {
              523.25f, 587.33f, 659.25f, 783.99f, 880.00f, 1046.50f, 880.00f, 1046.50f
            };
            for (int i = 0; i < 8; ++i) {
              _synthTone(notes[i], (i == 7) ? 220 : 85, 0.42f);
            }
            break;
          }

          case SfxType::WIN_JACKPOT: {
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
