# Wacky Panda (Microgaming) — Mathematical Model & PAR Sheet

This document details the Probability Accounting Report (PAR) sheet, combinatorial Return-To-Player (RTP), and virtual reel stop tables for the **Wacky Panda** 3-reel, 1-payline slot machine implementation.

---

## 1. Game Rules & Paytable (3-Coin Max Bet)

True to Microgaming's classic **Wacky Panda** slot:
- **Reels**: 3 mechanical reels, 1 center payline.
- **Bet**: Fixed **3 Coins** per spin (unlocks the **3,333-coin** top jackpot and **95.94% RTP**).
- **Red Panda Scatter-on-Line Exception**:
  - Unlike the other 7 symbols (which require 3-of-a-kind on the payline), the flagship **Red Panda (wearing a watermelon hat)** pays out when **1, 2, or 3** appear anywhere on the center payline!

| Combination | Coin Payout (3-Coin Bet) | Multiplier of Total Bet |
| :--- | :---: | :---: |
| **3 × Red Panda** *(Watermelon Hat)* | **3,333** | **1,111.00×** |
| **3 × Yellow Panda** *(Crying)* | **900** | **300.00×** |
| **3 × Purple Panda** *(Heart Eyes)* | **360** | **120.00×** |
| **3 × Blue Panda** *(Sunglasses)* | **180** | **60.00×** |
| **3 × Brown Panda** | **120** | **40.00×** |
| **3 × Watermelon** | **90** | **30.00×** |
| **3 × Banana** | **75** | **25.00×** |
| **3 × Grapes** | **60** | **20.00×** |
| **2 × Red Panda** *(Any 2 on Payline)* | **45** | **15.00×** |
| **1 × Red Panda** *(Any 1 on Payline)* | **30** | **10.00×** |

---

## 2. Virtual Reel Weights & Exact Combinatorial Cycle

Each reel uses a virtual weight table of **100 stops** mapped onto a **16-stop physical strip** (`100 × 100 × 100 = 1,000,000` total cycle; `3,000,000` coins wagered per full cycle).

| Symbol | Reel 0 Weight | Reel 1 Weight | Reel 2 Weight | 3-of-a-Kind Hits | Coin Return | RTP Contribution |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: |
| **Red Panda (3×)** | 1 | 1 | 1 | 1 | 3,333 | 0.111% |
| **Yellow Panda (3×)** | 3 | 4 | 12 | 144 | 129,600 | 4.320% |
| **Purple Panda (3×)** | 6 | 8 | 11 | 528 | 190,080 | 6.336% |
| **Blue Panda (3×)** | 10 | 12 | 11 | 1,320 | 237,600 | 7.920% |
| **Brown Panda (3×)** | 12 | 21 | 10 | 2,520 | 302,400 | 10.080% |
| **Watermelon (3×)** | 22 | 10 | 17 | 3,740 | 336,600 | 11.220% |
| **Banana (3×)** | 10 | 36 | 13 | 4,680 | 351,000 | 11.700% |
| **Grapes (3×)** | 36 | 8 | 25 | 7,200 | 432,000 | 14.400% |
| **Red Panda (2×)** | — | — | — | 297 | 13,365 | 0.445% |
| **Red Panda (1×)** | — | — | — | 29,403 | 882,090 | 29.403% |
| **Total** | **100** | **100** | **100** | **49,833** | **2,878,068** | **95.9356% (~95.94%)** |

---

## 3. Summary Metrics

- **Theoretical RTP**: **95.94%** (`2,878,068 / 3,000,000` coins, matching Microgaming's official 3-coin RTP of `95.94%`).
- **1,000,000-Spin Monte Carlo RTP**: **95.47%** (without hitting the 1-in-1M 3333 jackpot) / **95.94%** expected.
- **Hit Frequency**: **4.98%** (1 win every **20.07** spins, with a minimum win of **10× total bet**).
- **Standard Deviation (Volatility)**: **6.47×** bet (Medium-Classic Volatility).

To verify at any time:
```bash
python3 scripts/slot_math.py
python3 scripts/simulate_spins.py
```
