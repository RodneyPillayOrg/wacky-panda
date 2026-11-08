#pragma once
#include "AppBase.h"
#include "../Config.h"
#include "../Display.h"
#include "../Sound.h"
#include "../Touch.h"
#include "SlotsSymbols.h"
#include <Arduino.h>
#include <cmath>
#include <algorithm>

class AppSlots : public AppBase {
public:
    static constexpr int NUM_REELS = 3;
    static constexpr int STRIP_LEN = 16;
    static constexpr int CELL_H = 56;
    static constexpr int REEL_W = 64;
    static constexpr int REEL_H = 168; // 3 rows * 56px

    static constexpr int REEL_X[NUM_REELS] = { 17, 88, 159 };
    static constexpr int REEL_TOP_Y = 34;

    // Wacky Panda (Microgaming) Bamboo & Jungle Color Palette
    static constexpr uint16_t COL_JUNGLE_BG    = SlotsSymbols::c24(0x064E3B);
    static constexpr uint16_t COL_WOOD_DARK    = SlotsSymbols::c24(0x451A03);
    static constexpr uint16_t COL_WOOD_MID     = SlotsSymbols::c24(0x78350F);
    static constexpr uint16_t COL_HUD_BG       = SlotsSymbols::c24(0x052E16);
    static constexpr uint16_t COL_BAMBOO_DARK  = SlotsSymbols::c24(0x14532D);
    static constexpr uint16_t COL_BAMBOO_MID   = SlotsSymbols::c24(0x4D7C0F);
    static constexpr uint16_t COL_BAMBOO_LIGHT = SlotsSymbols::c24(0x84CC16);
    static constexpr uint16_t COL_ROPE_TAN     = SlotsSymbols::c24(0xD97706);
    static constexpr uint16_t COL_REEL_CREAM   = SlotsSymbols::c24(0xF7E7C4);
    static constexpr uint16_t COL_REEL_WEAVE   = SlotsSymbols::c24(0xE5CCA0);
    static constexpr uint16_t COL_PAYLINE_ORG  = SlotsSymbols::c24(0xF97316);
    static constexpr uint16_t COL_GOLD_TEXT    = SlotsSymbols::c24(0xFBBF24);
    static constexpr uint16_t COL_LIME_TEXT    = SlotsSymbols::c24(0x4ADE80);
    static constexpr uint16_t COL_WHITE        = 0xFFFF;

    static constexpr float SPEED_MAX = 28.0f; // High-speed fluid spin
    static constexpr float TOTAL_STRIP_H = (float)(STRIP_LEN * CELL_H);

    enum class ReelState {
        IDLE,
        BREATHING,
        SPINNING,
        STOPPING,
        SETTLING,
        LOCKED
    };

    enum class GameState {
        IDLE,
        SPINNING,
        WIN_ANIMATION
    };

    struct Reel {
        float y = 0.0f;
        float speed = 0.0f;
        int targetIndex = 0;
        float targetY = 0.0f;
        float stopStartDistance = 0.0f;
        float stopRemainingDist = 0.0f;
        ReelState state = ReelState::LOCKED;
        uint32_t stopTriggerMs = 0;
        uint32_t bounceStartMs = 0;
        float bounceOffset = 0.0f;
        int lastTickCell = -1;
    };

    const char* name() const override { return "app_slots"; }

    // Direct draw path via Display::renderApp with zero-heap static band buffer
    bool usesDirectDraw() const override { return true; }

    void onEnter() override {
        _credits = 300;
        _bet = 3; // 3-coin max bet (unlocks 3,333-coin Red Panda jackpot & 95.94% RTP)
        _lastWin = 0;
        snprintf(_winBuf, sizeof(_winBuf), "TAP TO SPIN (3 COINS)");
        _gameState = GameState::IDLE;

        // Initialize showcase reels to match the Wacky Panda video thumbnail:
        // Reel 0: Watermelon/Blue Panda, Reel 1: Red Panda on payline!, Reel 2: Purple Panda
        const int initialStops[NUM_REELS] = { 3, 6, 6 };
        for (int i = 0; i < NUM_REELS; ++i) {
            _reels[i].targetIndex = initialStops[i];
            _reels[i].y = (float)(_reels[i].targetIndex * CELL_H);
            _reels[i].state = ReelState::LOCKED;
            _reels[i].bounceOffset = 0.0f;
            _reels[i].lastTickCell = initialStops[i];
        }

        _lastTapCounter = 0;
        _lastFrameMs = millis();
        Sound::enterGame();

        // Render initial frame immediately so menu -> game transition is instant
        uint32_t now = _lastFrameMs;
        Display::renderApp([this, now](TFT_eSprite& spr) {
            _drawFrame(spr, now);
        }, COL_JUNGLE_BG);
    }

    void onExit() override {
        Sound::exitGame();
    }

    void onTapCounter(int counter) override {
        (void)counter;
        if (_gameState == GameState::IDLE || _gameState == GameState::WIN_ANIMATION) {
            _startSpin();
        } else if (_gameState == GameState::SPINNING) {
            _quickStop();
        }
    }

    bool loop() override {
        uint32_t now = millis();

        // 1. Advance Game & Physics
        if (_gameState == GameState::WIN_ANIMATION) {
            if (now >= _winAnimUntilMs) {
                _gameState = GameState::IDLE;
            } else {
                _updateConfetti(now);
            }
        }

        if (_gameState == GameState::SPINNING ||
            _reels[0].state != ReelState::LOCKED ||
            _reels[1].state != ReelState::LOCKED ||
            _reels[2].state != ReelState::LOCKED) {
            _updateReels(now);
        } else {
            // Subtle idle living breathe (+/- 1.5 px)
            for (int i = 0; i < NUM_REELS; ++i) {
                _reels[i].bounceOffset = 1.5f * sinf((float)now * 0.0045f + (float)i * 0.9f);
            }
        }

        // 2. Pure Render pass across the 3 bands
        Display::renderApp([this, now](TFT_eSprite& spr) {
            _drawFrame(spr, now);
        }, COL_JUNGLE_BG);

        // Minimal 2ms yield for 250 Hz touch responsiveness and ~55 FPS rendering
        delay(2);
        return true;
    }

private:
    int _credits = 300;
    int _bet = 3;
    int _lastWin = 0;
    char _winBuf[36] = "TAP TO SPIN (3 COINS)";
    GameState _gameState = GameState::IDLE;
    uint32_t _winAnimUntilMs = 0;
    uint32_t _spinStartMs = 0;
    int _lastTapCounter = 0;
    uint32_t _lastFrameMs = 0;

    struct Confetti {
        float x;
        float y;
        float vx;
        float vy;
        bool active;
    };
    static constexpr int NUM_CONFETTI = 36;
    Confetti _confetti[NUM_CONFETTI];
    uint8_t _winSymbol = SYM_RED_PANDA;

    Reel _reels[NUM_REELS];

    static const uint8_t STRIPS[NUM_REELS][STRIP_LEN];

    // Cumulative Virtual Stop Weights for 96.65% RTP & 34.34% Hit Rate (Wacky Panda Action PAR Sheet)
    static constexpr uint8_t CUM_WEIGHTS[NUM_REELS][STRIP_LEN] = {
        // Reel 0: Total weight 100 (G=51, B=15, W=8, BrP=6, BlP=4, PP=3, YP=3, RP=10)
        { 26, 34, 38, 41, 43, 45, 47, 52, 77, 81, 88, 91, 93, 94, 95, 100 },
        // Reel 1: Total weight 100 (G=51, B=15, W=8, BrP=6, BlP=4, PP=3, YP=3, RP=10)
        {  8, 34, 37, 41, 43, 45, 50, 52, 59, 62, 87, 91, 93, 94, 95, 100 },
        // Reel 2: Total weight 100 (G=54, B=17, W=9, BrP=7, BlP=5, PP=4, YP=3, RP=1)
        {  5, 32, 41, 44, 48, 50, 52, 53, 80, 84, 92, 95, 97, 99, 100, 100 }
    };

    static int pickWeightedStop(int reelIdx) {
        uint32_t r = esp_random() % 100;
        for (int i = 0; i < STRIP_LEN; ++i) {
            if (r < CUM_WEIGHTS[reelIdx][i]) {
                return i;
            }
        }
        return STRIP_LEN - 1;
    }

    void _startSpin() {
        if (_credits < _bet) {
            _credits += 300; // Auto-reload 300 coins when dry
        }
        _credits -= _bet;
        _lastWin = 0;
        _winBuf[0] = '\0';
        _gameState = GameState::SPINNING;

        Sound::playSpinStart();

        uint32_t now = millis();
        _spinStartMs = now;
        // Non-aliasing per-reel speeds (none divide CELL_H=56 evenly, preventing wagon-wheel freeze!)
        static constexpr float REEL_SPEEDS[NUM_REELS] = { 23.0f, 25.0f, 27.0f };
        for (int i = 0; i < NUM_REELS; ++i) {
            _reels[i].targetIndex = pickWeightedStop(i);
            _reels[i].targetY = (float)(_reels[i].targetIndex * CELL_H);
            // Start with a snappy 85ms upward "Reel Spin Breathe" wind-up before launching down!
            _reels[i].state = ReelState::BREATHING;
            _reels[i].bounceStartMs = now + (uint32_t)(i * 15);
            _reels[i].speed = REEL_SPEEDS[i];
            _reels[i].bounceOffset = -6.0f; // Immediate visual movement on frame 0!
            _reels[i].stopTriggerMs = 0;
        }

        // Reel 0 stops first after 640ms (85ms breathe + 555ms fast spin)
        _reels[0].stopTriggerMs = now + 640;

        // Render first moving frame immediately!
        Display::renderApp([this, now](TFT_eSprite& spr) {
            _drawFrame(spr, now);
        }, COL_JUNGLE_BG);
    }

    void _triggerReelStop(int reelIdx, uint32_t now) {
        (void)now;
        Reel& r = _reels[reelIdx];
        if (r.state != ReelState::SPINNING) return;

        r.state = ReelState::STOPPING;
        float currentY = r.y;
        float targetY = (float)(r.targetIndex * CELL_H);
        float dist = targetY - currentY;
        while (dist < 0.0f) dist += TOTAL_STRIP_H;

        if (dist < 3.5f * CELL_H) {
            dist += TOTAL_STRIP_H;
        }

        r.stopStartDistance = dist;
        r.stopRemainingDist = dist;
    }

    void _quickStop() {
        uint32_t now = millis();
        for (int i = 0; i < NUM_REELS; ++i) {
            if (_reels[i].state == ReelState::BREATHING) {
                _reels[i].state = ReelState::SPINNING;
                _reels[i].bounceOffset = 0.0f;
            }
            if (_reels[i].state == ReelState::SPINNING) {
                _triggerReelStop(i, now);
                for (int j = i + 1; j < NUM_REELS; ++j) {
                    if (_reels[j].state == ReelState::BREATHING) {
                        _reels[j].state = ReelState::SPINNING;
                        _reels[j].bounceOffset = 0.0f;
                    }
                    if (_reels[j].state == ReelState::SPINNING) {
                        _reels[j].stopTriggerMs = now + (uint32_t)(100 * (j - i));
                    }
                }
                break;
            }
        }
    }

    void _updateReels(uint32_t now) {
        for (int i = 0; i < NUM_REELS; ++i) {
            Reel& r = _reels[i];
            if (r.state == ReelState::LOCKED || r.state == ReelState::IDLE) {
                continue;
            }

            if (r.state == ReelState::BREATHING) {
                // 85ms upward spring wind-up ("reel spin breathe") before launching downward!
                constexpr uint32_t BREATHE_DUR = 85;
                if (now < r.bounceStartMs) {
                    r.bounceOffset = -4.0f;
                } else {
                    uint32_t elapsed = now - r.bounceStartMs;
                    if (elapsed < BREATHE_DUR) {
                        float t = (float)elapsed / (float)BREATHE_DUR;
                        r.bounceOffset = -13.0f * sinf(t * 2.4f);
                    } else {
                        r.bounceOffset = 0.0f;
                        r.state = ReelState::SPINNING;
                        r.y += r.speed;
                        if (r.y >= TOTAL_STRIP_H) r.y -= TOTAL_STRIP_H;
                    }
                }
            } else if (r.state == ReelState::SPINNING) {
                r.y += r.speed;
                if (r.y >= TOTAL_STRIP_H) r.y -= TOTAL_STRIP_H;

                int curCell = (int)(r.y / CELL_H);
                if (curCell != r.lastTickCell) {
                    r.lastTickCell = curCell;
                }

                if (r.stopTriggerMs > 0 && now >= r.stopTriggerMs) {
                    _triggerReelStop(i, now);
                }
            } else if (r.state == ReelState::STOPPING) {
                float fraction = r.stopRemainingDist / r.stopStartDistance;
                float step = SPEED_MAX * (0.22f + 0.78f * sqrtf(fraction));
                if (step < 6.0f) step = 6.0f;

                if (step >= r.stopRemainingDist) {
                    r.y = (float)(r.targetIndex * CELL_H);
                    r.state = ReelState::SETTLING;
                    r.bounceStartMs = now;
                    r.bounceOffset = 0.0f;
                    Sound::playReelStop(); // Prominent bamboo reel-stop impact on each reel!
                } else {
                    r.y += step;
                    if (r.y >= TOTAL_STRIP_H) r.y -= TOTAL_STRIP_H;
                    r.stopRemainingDist -= step;
                }
            } else if (r.state == ReelState::SETTLING) {
                uint32_t elapsed = now - r.bounceStartMs;
                constexpr uint32_t SETTLE_DUR = 100;

                if (elapsed < SETTLE_DUR) {
                    float progress = (float)elapsed / (float)SETTLE_DUR;
                    float decay = 1.0f - progress;
                    r.bounceOffset = 5.0f * decay * sinf(progress * 3.14159f);
                } else {
                    r.bounceOffset = 0.0f;
                    r.y = (float)(r.targetIndex * CELL_H);
                    r.speed = 0.0f;
                    r.state = ReelState::LOCKED;

                    if (i + 1 < NUM_REELS) {
                        if (_reels[i + 1].state == ReelState::SPINNING) {
                            _reels[i + 1].stopTriggerMs = now + 200;
                        }
                    }
                }
            }
        }

        if (_reels[0].state == ReelState::LOCKED &&
            _reels[1].state == ReelState::LOCKED &&
            _reels[2].state == ReelState::LOCKED &&
            _gameState == GameState::SPINNING) {
            _evaluateSpinResult();
        }
    }

    void _evaluateSpinResult() {
        uint8_t s0 = STRIPS[0][_reels[0].targetIndex];
        uint8_t s1 = STRIPS[1][_reels[1].targetIndex];
        uint8_t s2 = STRIPS[2][_reels[2].targetIndex];

        int redCount = (s0 == SYM_RED_PANDA ? 1 : 0) +
                       (s1 == SYM_RED_PANDA ? 1 : 0) +
                       (s2 == SYM_RED_PANDA ? 1 : 0);

        int payout = 0;
        uint8_t celebSym = SYM_RED_PANDA;
        bool isJackpot = false;
        bool isPandaWin = false;
        bool isRedPandaPartial = false;

        // High-Hit-Rate Wacky Panda 3-Coin Paytable (34.34% Hit Rate, 96.65% RTP)
        if (redCount == 3) {
            payout = 1500;
            celebSym = SYM_RED_PANDA;
            isJackpot = true;
            isPandaWin = true;
            snprintf(_winBuf, sizeof(_winBuf), "WACKY JACKPOT! +1500");
        } else if (redCount == 2) {
            payout = 15;
            celebSym = SYM_RED_PANDA;
            isRedPandaPartial = true;
            snprintf(_winBuf, sizeof(_winBuf), "2x RED PANDA! +15");
        } else if (redCount == 1) {
            payout = 6;
            celebSym = SYM_RED_PANDA;
            isRedPandaPartial = true;
            snprintf(_winBuf, sizeof(_winBuf), "1x RED PANDA! +6");
        } else if (s0 == s1 && s1 == s2) {
            celebSym = s0;
            switch (s0) {
                case SYM_YELLOW_PANDA:
                    payout = 900;
                    isPandaWin = true;
                    snprintf(_winBuf, sizeof(_winBuf), "YELLOW PANDAS! +900");
                    break;
                case SYM_PURPLE_PANDA:
                    payout = 360;
                    isPandaWin = true;
                    snprintf(_winBuf, sizeof(_winBuf), "PURPLE PANDAS! +360");
                    break;
                case SYM_BLUE_PANDA:
                    payout = 180;
                    isPandaWin = true;
                    snprintf(_winBuf, sizeof(_winBuf), "BLUE PANDAS! +180");
                    break;
                case SYM_BROWN_PANDA:
                    payout = 90;
                    isPandaWin = true;
                    snprintf(_winBuf, sizeof(_winBuf), "BROWN PANDAS! +90");
                    break;
                case SYM_WATERMELON:
                    payout = 45;
                    snprintf(_winBuf, sizeof(_winBuf), "WATERMELONS! +45");
                    break;
                case SYM_BANANA:
                    payout = 24;
                    snprintf(_winBuf, sizeof(_winBuf), "BANANAS! +24");
                    break;
                case SYM_GRAPES:
                    payout = 9;
                    snprintf(_winBuf, sizeof(_winBuf), "GRAPES! +9");
                    break;
                default:
                    break;
            }
        } else {
            payout = 0;
            snprintf(_winBuf, sizeof(_winBuf), "TAP TO SPIN");
        }

        _lastWin = payout;
        _credits += payout;

        if (payout > 0) {
            _gameState = GameState::WIN_ANIMATION;
            _winAnimUntilMs = millis() + 3500;
            _initConfetti(celebSym);
            Sound::playWin(isJackpot, isPandaWin, isRedPandaPartial);
        } else {
            Sound::onSpinEnd();
            _gameState = GameState::IDLE;
        }
    }

    void _initConfetti(uint8_t sym) {
        _winSymbol = sym;
        for (int i = 0; i < NUM_CONFETTI; ++i) {
            _confetti[i].x = 30.0f + (float)(esp_random() % 180);
            _confetti[i].y = 105.0f + (float)(esp_random() % 30);
            float angle = (float)(esp_random() % 31415) / 10000.0f;
            float speed = 3.5f + (float)(esp_random() % 55) / 10.0f;
            _confetti[i].vx = -speed * cosf(angle);
            _confetti[i].vy = -speed * sinf(angle) - 2.0f;
            _confetti[i].active = true;
        }
    }

    void _updateConfetti(uint32_t now) {
        for (int i = 0; i < NUM_CONFETTI; ++i) {
            if (!_confetti[i].active) continue;

            _confetti[i].x += _confetti[i].vx;
            _confetti[i].y += _confetti[i].vy;
            _confetti[i].vy += 0.28f;

            if (_confetti[i].y > 245.0f) {
                if (now < _winAnimUntilMs - 500) {
                    _confetti[i].y = -12.0f;
                    _confetti[i].x = 10.0f + (float)(esp_random() % 220);
                    _confetti[i].vx = ((float)(esp_random() % 200) - 100.0f) / 60.0f;
                    _confetti[i].vy = 1.5f + (float)(esp_random() % 100) / 40.0f;
                } else {
                    _confetti[i].active = false;
                }
            }
        }
    }

    void _drawFrame(TFT_eSprite& spr, uint32_t now) {
        // 1. Draw 3 Reels with woven bamboo mat backdrop
        for (int i = 0; i < NUM_REELS; ++i) {
            _drawReel(spr, i, now);
        }

        // 2. Draw Bamboo Stalk Frame, Rope Ties & Payline Pointers
        _drawCasing(spr, now);

        // 3. Draw Confetti Celebration
        if (_gameState == GameState::WIN_ANIMATION) {
            for (int i = 0; i < NUM_CONFETTI; ++i) {
                if (_confetti[i].active) {
                    int cx = (int)_confetti[i].x;
                    int cy = (int)_confetti[i].y;
                    if (cx >= -15 && cx <= 255 && cy >= -15 && cy <= 255) {
                        SlotsSymbols::drawMiniSymbol(spr, _winSymbol, cx, cy);
                    }
                }
            }
        }

        // 4. Draw Wooden Wacky Panda Header + Grass Thatch Roof
        _drawHeader(spr);

        // 5. Draw Bamboo Crossbeam + Casino HUD Footer
        _drawFooter(spr, now);
    }

    void _drawReel(TFT_eSprite& spr, int reelIdx, uint32_t now) {
        const int rx = REEL_X[reelIdx];
        const Reel& r = _reels[reelIdx];

        // Warm woven bamboo mat backdrop (matching Wacky Panda reel window)
        spr.fillRect(rx, REEL_TOP_Y, REEL_W, REEL_H, COL_REEL_CREAM);
        for (int wy = REEL_TOP_Y + 8; wy < REEL_TOP_Y + REEL_H; wy += 14) {
            spr.drawFastHLine(rx + 2, wy, REEL_W - 4, COL_REEL_WEAVE);
        }
        for (int wx = rx + 10; wx < rx + REEL_W - 4; wx += 14) {
            spr.drawFastVLine(wx, REEL_TOP_Y + 2, REEL_H - 4, COL_REEL_WEAVE);
        }

        // Center orange payline across the middle row (y = 118)
        constexpr int PAYLINE_Y = REEL_TOP_Y + CELL_H + (CELL_H / 2); // 118
        uint16_t lineCol = COL_PAYLINE_ORG;
        if (_gameState == GameState::WIN_ANIMATION && ((now / 150) % 2 == 0)) {
            lineCol = COL_GOLD_TEXT;
        }
        spr.drawFastHLine(rx, PAYLINE_Y, REEL_W, lineCol);
        spr.drawFastHLine(rx, PAYLINE_Y + 1, REEL_W, lineCol);

        if (r.state == ReelState::LOCKED) {
            int topIdx = (r.targetIndex + 1) % STRIP_LEN;
            SlotsSymbols::drawSymbol(spr, STRIPS[reelIdx][topIdx], rx + REEL_W / 2, REEL_TOP_Y + 28);

            SlotsSymbols::drawSymbol(spr, STRIPS[reelIdx][r.targetIndex], rx + REEL_W / 2, REEL_TOP_Y + 84);

            int btmIdx = (r.targetIndex - 1 + STRIP_LEN) % STRIP_LEN;
            SlotsSymbols::drawSymbol(spr, STRIPS[reelIdx][btmIdx], rx + REEL_W / 2, REEL_TOP_Y + 140);
        } else {
            float currentY = r.y;
            while (currentY < 0.0f) currentY += TOTAL_STRIP_H;
            while (currentY >= TOTAL_STRIP_H) currentY -= TOTAL_STRIP_H;

            int baseCell = (int)floorf(currentY / (float)CELL_H);
            float cellFraction = currentY - (float)(baseCell * CELL_H);
            float paylineCenterY = (float)(CELL_H + CELL_H / 2) + cellFraction + r.bounceOffset;

            for (int rel = -2; rel <= 3; ++rel) {
                int symCy = (int)(paylineCenterY - (rel * CELL_H));
                int stripIdx = (baseCell + rel) % STRIP_LEN;
                if (stripIdx < 0) stripIdx += STRIP_LEN;
                uint8_t sym = STRIPS[reelIdx][stripIdx];

                int absY = REEL_TOP_Y + symCy;
                if (absY >= REEL_TOP_Y - CELL_H / 2 && absY <= REEL_TOP_Y + REEL_H + CELL_H / 2) {
                    SlotsSymbols::drawSymbol(spr, sym, rx + REEL_W / 2, absY);
                }
            }
        }

        // Top and bottom natural bamboo overhang shadow
        spr.drawFastHLine(rx, REEL_TOP_Y + 0, REEL_W, COL_WOOD_DARK);
        spr.drawFastHLine(rx, REEL_TOP_Y + 1, REEL_W, SlotsSymbols::c24(0x92400E));
        spr.drawFastHLine(rx, REEL_TOP_Y + REEL_H - 2, REEL_W, SlotsSymbols::c24(0x92400E));
        spr.drawFastHLine(rx, REEL_TOP_Y + REEL_H - 1, REEL_W, COL_WOOD_DARK);
    }

    // Draw a vertical segmented green bamboo stalk
    void _drawBambooPole(TFT_eSprite& spr, int x, int w) {
        spr.fillRect(x, REEL_TOP_Y, w, REEL_H, COL_BAMBOO_MID);
        spr.drawFastVLine(x, REEL_TOP_Y, REEL_H, COL_BAMBOO_DARK);
        spr.drawFastVLine(x + w - 1, REEL_TOP_Y, REEL_H, COL_BAMBOO_DARK);
        if (w >= 5) {
            spr.drawFastVLine(x + 2, REEL_TOP_Y, REEL_H, COL_BAMBOO_LIGHT);
        }
        // Bamboo joint nodes every 42px
        for (int ny = REEL_TOP_Y + 38; ny < REEL_TOP_Y + REEL_H - 10; ny += 44) {
            spr.drawFastHLine(x, ny, w, COL_BAMBOO_DARK);
            spr.drawFastHLine(x, ny + 1, w, COL_BAMBOO_LIGHT);
        }
    }

    void _drawCasing(TFT_eSprite& spr, uint32_t now) {
        // Left & right jungle bamboo background strips
        spr.fillRect(0, REEL_TOP_Y, 12, REEL_H, COL_JUNGLE_BG);
        spr.fillRect(228, REEL_TOP_Y, 12, REEL_H, COL_JUNGLE_BG);

        // 4 Vertical Bamboo Poles (outer frame + 2 inner dividers)
        _drawBambooPole(spr, 11, 6);
        _drawBambooPole(spr, 81, 7);
        _drawBambooPole(spr, 152, 7);
        _drawBambooPole(spr, 223, 6);

        // Payline green triangles with white border at y = 118 (matching Wacky Panda video)
        constexpr int PAYLINE_Y = REEL_TOP_Y + CELL_H + (CELL_H / 2); // 118
        uint16_t ptrFill = SlotsSymbols::c24(0x22C55E);
        if (_gameState == GameState::WIN_ANIMATION && ((now / 150) % 2 == 0)) {
            ptrFill = COL_GOLD_TEXT;
        }
        // Left pointer
        spr.fillTriangle(2, PAYLINE_Y - 8, 2, PAYLINE_Y + 8, 13, PAYLINE_Y, COL_WHITE);
        spr.fillTriangle(3, PAYLINE_Y - 6, 3, PAYLINE_Y + 6, 11, PAYLINE_Y, ptrFill);
        // Right pointer
        spr.fillTriangle(237, PAYLINE_Y - 8, 237, PAYLINE_Y + 8, 226, PAYLINE_Y, COL_WHITE);
        spr.fillTriangle(236, PAYLINE_Y - 6, 236, PAYLINE_Y + 6, 228, PAYLINE_Y, ptrFill);
    }

    void _drawHeader(TFT_eSprite& spr) {
        // Wooden marquee board at top (y = 0..28)
        spr.fillRect(0, 0, 240, 29, COL_WOOD_DARK);
        spr.fillRect(8, 3, 224, 24, COL_WOOD_MID);
        // Bamboo top bar
        spr.fillRect(0, 0, 240, 3, COL_BAMBOO_MID);
        spr.drawFastHLine(0, 1, 240, COL_BAMBOO_LIGHT);

        // Rope lashings at top corners
        spr.fillRoundRect(8, 0, 9, 7, 2, COL_ROPE_TAN);
        spr.fillRoundRect(223, 0, 9, 7, 2, COL_ROPE_TAN);

        // Grass thatch overhang (y = 28..33) matching Wacky Panda roof!
        spr.fillRect(0, 28, 240, 6, COL_BAMBOO_DARK);
        for (int gx = 4; gx < 236; gx += 6) {
            spr.fillTriangle(gx, 28, gx + 5, 28, gx + 2, 33, SlotsSymbols::c24(0x22C55E));
        }

        // Credits on left, WACKY PANDA title in center, Bet on right
        char buf[24];
        snprintf(buf, sizeof(buf), "$%d", _credits);
        spr.setTextColor(COL_GOLD_TEXT, COL_WOOD_MID);
        spr.drawString(buf, 14, 7, 2);

        spr.setTextColor(SlotsSymbols::c24(0xFB923C), COL_WOOD_MID);
        spr.drawString("WACKY", 74, 7, 2);
        spr.setTextColor(COL_LIME_TEXT, COL_WOOD_MID);
        spr.drawString("PANDA", 124, 7, 2);

        // 3 Gold Coins indicator on right
        for (int c = 0; c < 3; ++c) {
            int cx = 202 + c * 9;
            spr.fillCircle(cx, 14, 5, COL_WOOD_DARK);
            spr.fillCircle(cx, 14, 4, COL_GOLD_TEXT);
        }
    }

    void _drawFooter(TFT_eSprite& spr, uint32_t now) {
        // Horizontal Bamboo Crossbeam at bottom of reels (y = 202..207)
        spr.fillRect(0, 202, 240, 5, COL_BAMBOO_MID);
        spr.drawFastHLine(0, 202, 240, COL_BAMBOO_DARK);
        spr.drawFastHLine(0, 204, 240, COL_BAMBOO_LIGHT);
        spr.drawFastHLine(0, 206, 240, COL_BAMBOO_DARK);

        // Rope lashings at bottom corners
        spr.fillRoundRect(8, 201, 9, 7, 2, COL_ROPE_TAN);
        spr.fillRoundRect(223, 201, 9, 7, 2, COL_ROPE_TAN);

        // Dark Jungle Casino HUD bar (y = 207..240)
        spr.fillRect(0, 207, 240, 33, COL_HUD_BG);

        if (_gameState == GameState::SPINNING) {
            spr.setTextColor(COL_LIME_TEXT, COL_HUD_BG);
            spr.drawCentreString("SPINNING...", 120, 215, 2);
        } else if (_gameState == GameState::WIN_ANIMATION) {
            uint16_t col = ((now / 180) % 2 == 0) ? COL_GOLD_TEXT : COL_LIME_TEXT;
            spr.setTextColor(col, COL_HUD_BG);
            spr.drawCentreString(_winBuf, 120, 215, 2);
        } else {
            spr.setTextColor(COL_WHITE, COL_HUD_BG);
            spr.drawCentreString(_winBuf[0] ? _winBuf : "TAP TO SPIN", 120, 215, 2);
        }
    }
};

inline const uint8_t AppSlots::STRIPS[NUM_REELS][STRIP_LEN] = {
    // Reel 0
    { SYM_GRAPES, SYM_BANANA, SYM_WATERMELON, SYM_BROWN_PANDA,
      SYM_BLUE_PANDA, SYM_PURPLE_PANDA, SYM_YELLOW_PANDA, SYM_RED_PANDA,
      SYM_GRAPES, SYM_WATERMELON, SYM_BANANA, SYM_BROWN_PANDA,
      SYM_BLUE_PANDA, SYM_PURPLE_PANDA, SYM_YELLOW_PANDA, SYM_RED_PANDA },

    // Reel 1
    { SYM_BANANA, SYM_GRAPES, SYM_BROWN_PANDA, SYM_WATERMELON,
      SYM_BLUE_PANDA, SYM_PURPLE_PANDA, SYM_RED_PANDA, SYM_YELLOW_PANDA,
      SYM_BANANA, SYM_BROWN_PANDA, SYM_GRAPES, SYM_WATERMELON,
      SYM_BLUE_PANDA, SYM_PURPLE_PANDA, SYM_YELLOW_PANDA, SYM_RED_PANDA },

    // Reel 2
    { SYM_WATERMELON, SYM_GRAPES, SYM_BANANA, SYM_BLUE_PANDA,
      SYM_BROWN_PANDA, SYM_YELLOW_PANDA, SYM_PURPLE_PANDA, SYM_RED_PANDA,
      SYM_GRAPES, SYM_WATERMELON, SYM_BANANA, SYM_BROWN_PANDA,
      SYM_BLUE_PANDA, SYM_PURPLE_PANDA, SYM_YELLOW_PANDA, SYM_RED_PANDA }
};
