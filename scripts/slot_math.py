#!/usr/bin/env python3
"""
Wacky Panda High-Action PAR Sheet & Exact Combinatorial Cycle Calculator.
Verifies exact combinatorial Return-To-Player (RTP = 96.65%), Hit Frequency (34.34%),
3-of-a-Kind Hit Rate (14.54%), Red Panda Scatter Hit Rate (19.80%), and Volatility
for the 3-reel, 1-payline, 3-coin max-bet model.
"""
import math

BET_COINS = 3

PAYOUTS_3OAK = {
    "RED_PANDA":    1500,  # 500x total bet
    "YELLOW_PANDA":  900,  # 300x total bet
    "PURPLE_PANDA":  360,  # 120x total bet
    "BLUE_PANDA":    180,  # 60x total bet
    "BROWN_PANDA":    90,  # 30x total bet
    "WATERMELON":     45,  # 15x total bet
    "BANANA":         24,  # 8x total bet
    "GRAPES":          9,  # 3x total bet
}
PAYOUT_2_RED = 15  # 5x total bet
PAYOUT_1_RED =  6  # 2x total bet

SYMBOLS = [
    "GRAPES", "BANANA", "WATERMELON", "BROWN_PANDA",
    "BLUE_PANDA", "PURPLE_PANDA", "YELLOW_PANDA", "RED_PANDA"
]

# Virtual stop weights per symbol on each reel (summing to 100 per reel)
R0 = {"GRAPES": 51, "BANANA": 15, "WATERMELON": 8, "BROWN_PANDA": 6,
      "BLUE_PANDA": 4, "PURPLE_PANDA": 3, "YELLOW_PANDA": 3, "RED_PANDA": 10}

R1 = {"GRAPES": 51, "BANANA": 15, "WATERMELON": 8, "BROWN_PANDA": 6,
      "BLUE_PANDA": 4, "PURPLE_PANDA": 3, "YELLOW_PANDA": 3, "RED_PANDA": 10}

R2 = {"GRAPES": 54, "BANANA": 17, "WATERMELON": 9, "BROWN_PANDA": 7,
      "BLUE_PANDA": 5, "PURPLE_PANDA": 4, "YELLOW_PANDA": 3, "RED_PANDA": 1}

assert sum(R0.values()) == 100
assert sum(R1.values()) == 100
assert sum(R2.values()) == 100

CYCLE = 100 * 100 * 100
TOTAL_COIN_WAGER = CYCLE * BET_COINS

print("=== WACKY PANDA HIGH-HIT-RATE PAR SHEET SOLVER ===")
print(f"Total Virtual Cycle: {CYCLE:,} outcomes (100 x 100 x 100)")
print(f"Bet Per Spin:        {BET_COINS} coins\n")

total_coins_won = 0
total_3oak_hits = 0
variance_sum = 0.0

for sym in reversed(SYMBOLS):
    hits = R0[sym] * R1[sym] * R2[sym]
    pay = PAYOUTS_3OAK[sym]
    coins = hits * pay
    total_coins_won += coins
    total_3oak_hits += hits
    p = hits / CYCLE
    mult = pay / BET_COINS
    variance_sum += p * (mult ** 2)
    rtp_c = (coins / TOTAL_COIN_WAGER) * 100.0
    print(f"  3x {sym:<13}: {hits:6,d} hits ({p*100:6.3f}%) | Pay {pay:4d} ({mult:6.1f}x) | RTP: {rtp_c:6.3f}%")

# 2x RED_PANDA (any 2 on payline)
r0, r1, r2 = R0["RED_PANDA"], R1["RED_PANDA"], R2["RED_PANDA"]
hits_2red = r0 * r1 * (100 - r2) + r0 * (100 - r1) * r2 + (100 - r0) * r1 * r2
coins_2red = hits_2red * PAYOUT_2_RED
total_coins_won += coins_2red
variance_sum += (hits_2red / CYCLE) * ((PAYOUT_2_RED / BET_COINS) ** 2)
print(f"  2x {'RED_PANDA':<13}: {hits_2red:6,d} hits ({hits_2red/CYCLE*100:6.3f}%) | Pay {PAYOUT_2_RED:4d} ({PAYOUT_2_RED/BET_COINS:6.1f}x) | RTP: {coins_2red/TOTAL_COIN_WAGER*100:6.3f}%")

# 1x RED_PANDA (any 1 on payline)
hits_1red = (r0 * (100 - r1) * (100 - r2) +
             (100 - r0) * r1 * (100 - r2) +
             (100 - r0) * (100 - r1) * r2)
coins_1red = hits_1red * PAYOUT_1_RED
total_coins_won += coins_1red
variance_sum += (hits_1red / CYCLE) * ((PAYOUT_1_RED / BET_COINS) ** 2)
print(f"  1x {'RED_PANDA':<13}: {hits_1red:6,d} hits ({hits_1red/CYCLE*100:6.3f}%) | Pay {PAYOUT_1_RED:4d} ({PAYOUT_1_RED/BET_COINS:6.1f}x) | RTP: {coins_1red/TOTAL_COIN_WAGER*100:6.3f}%")

total_red_partial = hits_2red + hits_1red
total_hits = total_3oak_hits + total_red_partial
rtp = (total_coins_won / TOTAL_COIN_WAGER) * 100.0
hf = (total_hits / CYCLE) * 100.0
hf_3oak = (total_3oak_hits / CYCLE) * 100.0
hf_red = (total_red_partial / CYCLE) * 100.0
ev = rtp / 100.0
sd = math.sqrt(variance_sum - (ev ** 2))

print("-" * 78)
print(f"Theoretical RTP:          {rtp:.4f}% (~96.65%)")
print(f"Total Hit Frequency:      {hf:.2f}% (1 win every {CYCLE/total_hits:.2f} spins)")
print(f"  - 3-of-a-Kind Hit Rate: {hf_3oak:.2f}% (1 in every {CYCLE/total_3oak_hits:.2f} spins)")
print(f"  - 1x/2x Red Panda Rate: {hf_red:.2f}% (1 in every {CYCLE/total_red_partial:.2f} spins)")
print(f"Standard Deviation:       {sd:.2f}x bet")
