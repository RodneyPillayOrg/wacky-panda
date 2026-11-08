# Wacky Panda (High-Action Edition) — Mathematical Model & PAR Sheet

This document details the Probability Accounting Report (PAR) sheet, combinatorial Return-To-Player (RTP), and virtual reel stop tables for the **Wacky Panda** 3-reel, 1-payline slot machine implementation, tuned for high-frequency action during live hardware gameplay.

---

## 1. Game Rules & Paytable (3-Coin Max Bet)

- **Reels**: 3 mechanical reels, 1 center payline (`16` physical stops per reel, `100` virtual stop weights per reel).
- **Bet**: Fixed **3 Coins** per spin.
- **Red Panda Scatter-on-Line Mechanic**:
  - Unlike the other 7 symbols (which require 3-of-a-kind on the payline), the flagship **Red Panda (wearing a watermelon hat)** pays out when **1, 2, or 3** appear anywhere on the center payline!
- **Every Win Is a Profit**: Even the smallest win (`1 × Red Panda`) pays **6 coins (2× total bet)**, and `3 × Grapes` pays **9 coins (3× total bet)**.

| Combination | Coin Payout (3-Coin Bet) | Multiplier of Total Bet |
| :--- | :---: | :---: |
| **3 × Red Panda** *(Watermelon Hat)* | **1,500** | **500.00×** |
| **3 × Yellow Panda** *(Crying)* | **900** | **300.00×** |
| **3 × Purple Panda** *(Heart Eyes)* | **360** | **120.00×** |
| **3 × Blue Panda** *(Sunglasses)* | **180** | **60.00×** |
| **3 × Brown Panda** | **90** | **30.00×** |
| **3 × Watermelon** | **45** | **15.00×** |
| **3 × Banana** | **24** | **8.00×** |
| **3 × Grapes** | **9** | **3.00×** |
| **2 × Red Panda** *(Any 2 on Payline)* | **15** | **5.00×** |
| **1 × Red Panda** *(Any 1 on Payline)* | **6** | **2.00×** |

---

## 2. Virtual Reel Weights & Exact Combinatorial Cycle

Each reel uses a virtual weight table of **100 stops** mapped onto a **16-stop physical strip** (`100 × 100 × 100 = 1,000,000` total cycle; `3,000,000` coins wagered per full cycle).

| Symbol | Reel 0 Weight | Reel 1 Weight | Reel 2 Weight | Cycle Hits | Hit Rate | Coin Return | RTP Contribution |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **Red Panda (3×)** | 10 | 10 | 1 | 100 | 0.010% | 150,000 | 5.000% |
| **Yellow Panda (3×)** | 3 | 3 | 3 | 27 | 0.003% | 24,300 | 0.810% |
| **Purple Panda (3×)** | 3 | 3 | 4 | 36 | 0.004% | 12,960 | 0.432% |
| **Blue Panda (3×)** | 4 | 4 | 5 | 80 | 0.008% | 14,400 | 0.480% |
| **Brown Panda (3×)** | 6 | 6 | 7 | 252 | 0.025% | 22,680 | 0.756% |
| **Watermelon (3×)** | 8 | 8 | 9 | 576 | 0.058% | 25,920 | 0.864% |
| **Banana (3×)** | 15 | 15 | 17 | 3,825 | 0.383% | 91,800 | 3.060% |
| **Grapes (3×)** | 51 | 51 | 54 | 140,454 | 14.045% | 1,264,086 | 42.136% |
| **Red Panda (2×)** | — | — | — | 11,700 | 1.170% | 175,500 | 5.850% |
| **Red Panda (1×)** | — | — | — | 186,300 | 18.630% | 1,117,800 | 37.260% |
| **Total** | **100** | **100** | **100** | **343,350** | **34.335%** | **2,899,446** | **96.6482% (~96.65%)** |

---

## 3. Asymmetric Near-Miss & Hit Rate Breakdown

- **Total Hit Frequency**: **34.34%** (`343,350 / 1,000,000` — **1 win every 2.91 spins**, a **6.9× increase** over the original 4.98% hit rate).
  - **3-of-a-Kind Hit Rate**: **14.54%** (`145,350 / 1,000,000` — **1 in every 6.88 spins**, a **7.2× increase** over the original 2.01% 3-of-a-kind rate).
  - **Any-1 / Any-2 Red Panda Hit Rate**: **19.80%** (`198,000 / 1,000,000` — **1 in every 5.05 spins**).
- **Red Panda Jackpot Tease**:
  - Reels 0 and 1 each allocate **10%** weight to `SYM_RED_PANDA`, while Reel 2 allocates **1%**.
  - Players land `[ Red Panda ] [ Red Panda ]` on the first two reels once every **100 spins** (`10% × 10% = 1.00%`), creating a high-tension jackpot sweat before Reel 2 locks in (and even when Reel 2 misses, it still awards a **15-coin (5× bet)** `2× Red Panda` win!).
- **Theoretical RTP**: **96.65%** (`2,899,446 / 3,000,000` coins).

To verify at any time:
```bash
python3 scripts/slot_math.py
python3 scripts/simulate_spins.py
```
