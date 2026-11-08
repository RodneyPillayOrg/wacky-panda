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
  ENTER_GAME,
  EXIT_GAME,
  SPIN_START,
  SPIN_END,
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
    pinMode(AUDIO_PA_PIN, OUTPUT);
    digitalWrite(AUDIO_PA_PIN, LOW); // Keep speaker amp OFF in Launcher menu & Settings!

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

      // 2. Initialize ES8311 Codec over I2C (SDA=15, SCL=14, 7-bit addr 0x18)
      _initEs8311Codec();

      _ready = true;
      _sfxQueue = xQueueCreate(12, sizeof(SfxType));
      xTaskCreatePinnedToCore(_audioTask, "sfx", 4096, nullptr, 1, nullptr, 0);
      Serial.printf("[sound] ES8311 + I2S 3-voice mixer ready @ %u Hz\n", (unsigned)SAMPLE_RATE);
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

  static void enterGame()     { play(SfxType::ENTER_GAME); }
  static void exitGame()      { play(SfxType::EXIT_GAME); }
  static void playIntro()     { play(SfxType::ENTER_GAME); }
  static void playSpinStart() { play(SfxType::SPIN_START); }
  static void onSpinEnd()     { play(SfxType::SPIN_END); }
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

  static void _selectClip(SfxType sfx, const int16_t*& ptr, size_t& len) {
    switch (sfx) {
      case SfxType::SPIN_START:
        ptr = SFX_PCM_SPIN;
        len = SFX_PCM_SPIN_LEN;
        break;
      case SfxType::REEL_STOP:
        ptr = SFX_PCM_STOP;
        len = SFX_PCM_STOP_LEN;
        break;
      case SfxType::WIN_REDPANDA:
        ptr = SFX_PCM_WIN_REDPANDA;
        len = SFX_PCM_WIN_REDPANDA_LEN;
        break;
      case SfxType::WIN_SMALL:
        ptr = SFX_PCM_WIN_SMALL;
        len = SFX_PCM_WIN_SMALL_LEN;
        break;
      case SfxType::WIN_PANDA:
      case SfxType::WIN_JACKPOT:
        ptr = SFX_PCM_WIN_BIG;
        len = SFX_PCM_WIN_BIG_LEN;
        break;
      default:
        ptr = nullptr;
        len = 0;
        break;
    }
  }

  // Real-time 3-voice integer audio mixer (active ONLY inside Wacky Panda, silent in menu/settings):
  // - Voice 1: Subtle Wacky Panda bamboo forest idle ambient loop (SFX_PCM_INTRO, ~18% idle / ~8% active)
  // - Voice 2: Foreground SFX (Spin Gong + Reel Roll sustained across the spin, plus Win fanfares)
  // - Voice 3: Prominent, punchy Reel Stop impact (SFX_PCM_STOP) overlaid as each reel locks!
  static void _audioTask(void*) {
    bool inGame = false;
    size_t ambPos = 0;
    int32_t ambGainQ8 = 46; // 46/256 (~18%) subtle background ambient

    const int16_t* fgPtr = nullptr;
    size_t fgLen = 0;
    size_t fgPos = 0;
    bool spinActive = false;
    bool spinFadeOut = false;
    size_t spinFadePos = 0;
    static constexpr size_t SPIN_FADEOUT_LEN = 4000; // 250ms smooth release when 3rd reel locks

    // Dedicated prominent Reel-Stop impact voice (plays on top of spin melody as each reel locks)
    bool stopActive = false;
    size_t stopPos = 0;

    // Crossfade state for seamless foreground transitions (400 samples = 25ms)
    static constexpr size_t XFADE_LEN = 400;
    const int16_t* prevFgPtr = nullptr;
    size_t prevFgLen = 0;
    size_t prevFgPos = 0;
    size_t xfadePos = XFADE_LEN;

    // Loop points inside SFX_PCM_SPIN (57,600 samples = 3.60s)
    static constexpr size_t SPIN_SUSTAIN_START = 12000; // 0.75s (after initial gong strike)
    static constexpr size_t SPIN_SUSTAIN_END   = 50000; // 3.12s

    int16_t mixBuf[256];

    for (;;) {
      SfxType cmd;
      while (xQueueReceive(_sfxQueue, &cmd, 0) == pdTRUE) {
        if (cmd == SfxType::ENTER_GAME) {
          if (!inGame) {
            inGame = true;
            ambPos = 0;
            ambGainQ8 = 46;
            digitalWrite(AUDIO_PA_PIN, HIGH);
          }
          continue;
        }
        if (cmd == SfxType::EXIT_GAME) {
          inGame = false;
          fgPtr = nullptr;
          fgLen = 0;
          fgPos = 0;
          prevFgPtr = nullptr;
          spinActive = false;
          spinFadeOut = false;
          stopActive = false;
          stopPos = 0;
          digitalWrite(AUDIO_PA_PIN, LOW);
          memset(mixBuf, 0, sizeof(mixBuf));
          size_t written = 0;
          i2s_channel_write(_txHandle, mixBuf, sizeof(mixBuf), &written, pdMS_TO_TICKS(20));
          continue;
        }
        if (!inGame) {
          continue;
        }
        if (cmd == SfxType::REEL_STOP) {
          // Trigger prominent Reel-Stop impact on Voice 3 without interrupting the spin melody!
          stopActive = true;
          stopPos = 0;
          continue;
        }
        if (cmd == SfxType::SPIN_END) {
          // 3rd reel locked on a non-winning spin: begin smooth 250ms release decay
          if (fgPtr == SFX_PCM_SPIN) {
            spinActive = false;
            spinFadeOut = true;
            spinFadePos = 0;
          }
          continue;
        }

        const int16_t* nextPtr = nullptr;
        size_t nextLen = 0;
        _selectClip(cmd, nextPtr, nextLen);
        if (nextPtr && nextLen > 0) {
          if (fgPtr && fgPos < fgLen) {
            prevFgPtr = fgPtr;
            prevFgLen = fgLen;
            prevFgPos = fgPos;
            xfadePos  = 0;
          }
          fgPtr = nextPtr;
          fgLen = nextLen;
          fgPos = 0;
          spinActive  = (cmd == SfxType::SPIN_START);
          spinFadeOut = false;
          spinFadePos = 0;
        }
      }

      if (!inGame) {
        vTaskDelay(pdMS_TO_TICKS(15));
        continue;
      }

      // Subtle background ambient: 46/256 (~18%) when idle, 20/256 (~8%) when spinning/winning
      ambGainQ8 = (fgPtr != nullptr) ? 20 : 46;

      for (size_t i = 0; i < 256; ++i) {
        // 1. Subtle Idle Bamboo Forest Ambient Sample (Q8 integer math)
        int32_t ambSample = (int16_t)pgm_read_word(&SFX_PCM_INTRO[ambPos]);
        if (++ambPos >= SFX_PCM_INTRO_LEN) {
          ambPos = 0;
        }
        int32_t mixed = (ambSample * ambGainQ8) >> 8;

        // 2. Foreground SFX Sample (Spin Gong/Melody or Win Fanfare)
        int32_t fgSample = 0;
        if (fgPtr) {
          if (spinActive && fgPtr == SFX_PCM_SPIN && fgPos >= SPIN_SUSTAIN_END) {
            fgPos = SPIN_SUSTAIN_START;
          }
          if (fgPos < fgLen) {
            // Slightly duck spin melody (192/256 = 75%) while a reel-stop impact is punching through
            int32_t rawFg = (int16_t)pgm_read_word(&fgPtr[fgPos++]);
            if (fgPtr == SFX_PCM_SPIN && stopActive) {
              rawFg = (rawFg * 180) >> 8;
            }
            if (spinFadeOut) {
              if (spinFadePos < SPIN_FADEOUT_LEN) {
                int32_t rem = (int32_t)(SPIN_FADEOUT_LEN - spinFadePos);
                rawFg = (rawFg * rem) / (int32_t)SPIN_FADEOUT_LEN;
                ++spinFadePos;
              } else {
                rawFg = 0;
                fgPtr = nullptr;
                fgLen = 0;
                fgPos = 0;
                spinFadeOut = false;
              }
            }
            fgSample = rawFg;
          } else {
            fgPtr = nullptr;
            fgLen = 0;
            fgPos = 0;
            spinActive = false;
            spinFadeOut = false;
          }
        }

        // 3. Smooth 25ms Crossfade from previous foreground sound if preempted
        if (xfadePos < XFADE_LEN && prevFgPtr && prevFgPos < prevFgLen) {
          int32_t prevSample = (int16_t)pgm_read_word(&prevFgPtr[prevFgPos++]);
          int32_t wNew = (int32_t)xfadePos;
          int32_t wOld = (int32_t)(XFADE_LEN - xfadePos);
          fgSample = (prevSample * wOld + fgSample * wNew) / (int32_t)XFADE_LEN;
          ++xfadePos;
        } else {
          prevFgPtr = nullptr;
        }

        // 4. Prominent Reel-Stop Impact Voice (115% boost = 294/256)
        int32_t stopSample = 0;
        if (stopActive) {
          if (stopPos < SFX_PCM_STOP_LEN) {
            int32_t rawStop = (int16_t)pgm_read_word(&SFX_PCM_STOP[stopPos++]);
            stopSample = (rawStop * 294) >> 8;
          } else {
            stopActive = false;
            stopPos = 0;
          }
        }

        int32_t outVal = mixed + fgSample + stopSample;
        if (outVal > 32767)  outVal = 32767;
        if (outVal < -32768) outVal = -32768;
        mixBuf[i] = (int16_t)outVal;
      }

      size_t written = 0;
      i2s_channel_write(_txHandle, mixBuf, sizeof(mixBuf), &written, pdMS_TO_TICKS(50));
    }
  }
#endif
};
