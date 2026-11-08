#pragma once
#include <Arduino.h>
#include <cmath>

#ifndef HAS_I2S_AUDIO
#define HAS_I2S_AUDIO 0
#endif

#if HAS_I2S_AUDIO
#include <Wire.h>
#include <driver/i2s_std.h>
#include <driver/gpio.h>
#include "WackyPandaAudio.h"

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
  INTRO_AMBIENT,
  SPIN_START,
  REEL_STOP,
  WIN_REDPANDA,
  WIN_SMALL,
  WIN_PANDA,
  WIN_JACKPOT
};

class Sound {
public:
  static void begin() {
#if HAS_I2S_AUDIO
    // Keep strapping pins GPIO 45 (VDD_SPI) and GPIO 46 (ROM boot) LOW by default
    pinMode(AUDIO_PA_PIN, OUTPUT);
    digitalWrite(AUDIO_PA_PIN, LOW);

    // 1. Initialize I2S Master Channel (16,000 Hz, with MCLK on GPIO 16 for ES8311)
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

      // 2. Initialize ES8311 Codec over I2C (SDA=15, SCL=14, 7-bit addr 0x18) while MCLK is active
      _initEs8311Codec();

      _ready = true;
      _sfxQueue = xQueueCreate(8, sizeof(SfxType));
      xTaskCreatePinnedToCore(_audioTask, "sfx", 4096, nullptr, 2, nullptr, 0);
      play(SfxType::INTRO_AMBIENT);
      Serial.printf("[sound] ES8311 + I2S ready @ %u Hz (MCLK=%d, BCLK=%d, WS=%d, DOUT=%d, PA=%d)\n",
                    (unsigned)SAMPLE_RATE, I2S_SPK_MCLK, I2S_SPK_BCLK, I2S_SPK_LRCK, I2S_SPK_DOUT, AUDIO_PA_PIN);
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

  static void playIntro()     { play(SfxType::INTRO_AMBIENT); }
  static void playSpinStart() { play(SfxType::SPIN_START); }
  static void playReelStop()  { play(SfxType::REEL_STOP); }

  static void playWin(bool isJackpot, bool isPandaWin, bool isRedPandaPartial = false) {
    if (isJackpot || isPandaWin) {
      play(SfxType::WIN_JACKPOT);
    } else if (isRedPandaPartial) {
      play(SfxType::WIN_REDPANDA);
    } else {
      play(SfxType::WIN_SMALL);
    }
  }

private:
#if HAS_I2S_AUDIO
  static constexpr uint32_t SAMPLE_RATE = WACKY_AUDIO_SAMPLE_RATE;
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
    _writeEs8311Reg(0x31, 0x00); // Unmute DAC state machine
    _writeEs8311Reg(0x32, 0xBF); // DAC 0dB digital volume
    _writeEs8311Reg(0x37, 0x08); // Unmute DAC
  }

  // Stream authentic 16kHz 16-bit mono PCM clip from Flash (PROGMEM),
  // immediately preempting if a newer SFX arrives in _sfxQueue.
  static void _playPcm(const int16_t* pcm, size_t totalSamples) {
    if (!_ready || !_txHandle || !pcm || totalSamples == 0) return;

    // Enable PA (GPIO 46 HIGH) only during active playback
    digitalWrite(AUDIO_PA_PIN, HIGH);

    int16_t buf[256];
    size_t pos = 0;
    while (pos < totalSamples) {
      if (uxQueueMessagesWaiting(_sfxQueue) > 0) {
        return; // Preempt immediately for newer SFX (PA stays HIGH for next clip)
      }
      size_t chunk = totalSamples - pos;
      if (chunk > 256) chunk = 256;
      memcpy_P(buf, pcm + pos, chunk * sizeof(int16_t));
      size_t written = 0;
      i2s_channel_write(_txHandle, buf, chunk * sizeof(int16_t), &written, pdMS_TO_TICKS(50));
      pos += chunk;
    }

    // Write 128 zero samples after clip completes to prevent DAC pop
    memset(buf, 0, 128 * sizeof(int16_t));
    size_t written = 0;
    i2s_channel_write(_txHandle, buf, 128 * sizeof(int16_t), &written, pdMS_TO_TICKS(20));

    // Return PA (GPIO 46 strapping pin) to LOW when idle
    if (uxQueueMessagesWaiting(_sfxQueue) == 0) {
      digitalWrite(AUDIO_PA_PIN, LOW);
    }
  }

  static void _audioTask(void*) {
    SfxType sfx;
    for (;;) {
      if (xQueueReceive(_sfxQueue, &sfx, portMAX_DELAY) == pdTRUE) {
        // Drain any older queued events so only the newest triggered SFX plays
        while (uxQueueMessagesWaiting(_sfxQueue) > 0) {
          xQueueReceive(_sfxQueue, &sfx, 0);
        }
        switch (sfx) {
          case SfxType::INTRO_AMBIENT:
            _playPcm(SFX_PCM_INTRO, SFX_PCM_INTRO_LEN);
            break;
          case SfxType::SPIN_START:
            _playPcm(SFX_PCM_SPIN, SFX_PCM_SPIN_LEN);
            break;
          case SfxType::REEL_STOP:
            _playPcm(SFX_PCM_STOP, SFX_PCM_STOP_LEN);
            break;
          case SfxType::WIN_REDPANDA:
            _playPcm(SFX_PCM_WIN_REDPANDA, SFX_PCM_WIN_REDPANDA_LEN);
            break;
          case SfxType::WIN_SMALL:
            _playPcm(SFX_PCM_WIN_SMALL, SFX_PCM_WIN_SMALL_LEN);
            break;
          case SfxType::WIN_PANDA:
          case SfxType::WIN_JACKPOT:
            _playPcm(SFX_PCM_WIN_BIG, SFX_PCM_WIN_BIG_LEN);
            break;
          default:
            break;
        }
      }
    }
  }
#endif
};
