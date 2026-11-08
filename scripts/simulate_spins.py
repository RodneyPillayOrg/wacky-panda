#!/usr/bin/env python3
"""
1,000,000-Spin Monte Carlo Simulation for Wacky Panda (High-Hit-Rate Action Edition).
Uses the exact 16-stop physical strips and cumulative weight lookup tables
compiled into src/apps/AppSlots.h.
"""
import random

strip0 = [
    "GRAPES", "BANANA", "WATERMELON", "BROWN_PANDA",
    "BLUE_PANDA", "PURPLE_PANDA", "YELLOW_PANDA", "RED_PANDA",
    "GRAPES", "WATERMELON", "BANANA", "BROWN_PANDA",
    "BLUE_PANDA", "PURPLE_PANDA", "YELLOW_PANDA", "RED_PANDA"
]

strip1 = [
    "BANANA", "GRAPES", "BROWN_PANDA", "WATERMELON",
    "BLUE_PANDA", "PURPLE_PANDA", "RED_PANDA", "YELLOW_PANDA",
    "BANANA", "BROWN_PANDA", "GRAPES", "WATERMELON",
    "BLUE_PANDA", "PURPLE_PANDA", "YELLOW_PANDA", "RED_PANDA"
]

strip2 = [
    "WATERMELON", "GRAPES", "BANANA", "BLUE_PANDA",
    "BROWN_PANDA", "YELLOW_PANDA", "PURPLE_PANDA", "RED_PANDA",
    "GRAPES", "WATERMELON", "BANANA", "BROWN_PANDA",
    "BLUE_PANDA", "PURPLE_PANDA", "YELLOW_PANDA", "RED_PANDA"
]

# 16 stop weights for each reel (sum = 100 each)
weights0 = [
    26, # 0: GRAPES
     8, # 1: BANANA
     4, # 2: WATERMELON
     3, # 3: BROWN_PANDA
     2, # 4: BLUE_PANDA
     2, # 5: PURPLE_PANDA
     2, # 6: YELLOW_PANDA
     5, # 7: RED_PANDA
    25, # 8: GRAPES
     4, # 9: WATERMELON
     7, # 10: BANANA
     3, # 11: BROWN_PANDA
     2, # 12: BLUE_PANDA
     1, # 13: PURPLE_PANDA
     1, # 14: YELLOW_PANDA
     5  # 15: RED_PANDA
]

weights1 = [
     8, # 0: BANANA
    26, # 1: GRAPES
     3, # 2: BROWN_PANDA
     4, # 3: WATERMELON
     2, # 4: BLUE_PANDA
     2, # 5: PURPLE_PANDA
     5, # 6: RED_PANDA
     2, # 7: YELLOW_PANDA
     7, # 8: BANANA
     3, # 9: BROWN_PANDA
    25, # 10: GRAPES
     4, # 11: WATERMELON
     2, # 12: BLUE_PANDA
     1, # 13: PURPLE_PANDA
     1, # 14: YELLOW_PANDA
     5  # 15: RED_PANDA
]

weights2 = [
     5, # 0: WATERMELON
    27, # 1: GRAPES
     9, # 2: BANANA
     3, # 3: BLUE_PANDA
     4, # 4: BROWN_PANDA
     2, # 5: YELLOW_PANDA
     2, # 6: PURPLE_PANDA
     1, # 7: RED_PANDA
    27, # 8: GRAPES
     4, # 9: WATERMELON
     8, # 10: BANANA
     3, # 11: BROWN_PANDA
     2, # 12: BLUE_PANDA
     2, # 13: PURPLE_PANDA
     1, # 14: YELLOW_PANDA
     0  # 15: RED_PANDA (visual near-miss stop)
]

assert sum(weights0) == 100, f"sum(weights0)={sum(weights0)}"
assert sum(weights1) == 100, f"sum(weights1)={sum(weights1)}"
assert sum(weights2) == 100, f"sum(weights2)={sum(weights2)}"

BET_COINS = 3
PAYOUTS_3OAK = {
    "RED_PANDA":    1500,
    "YELLOW_PANDA":  900,
    "PURPLE_PANDA":  360,
    "BLUE_PANDA":    180,
    "BROWN_PANDA":    90,
    "WATERMELON":     45,
    "BANANA":         24,
    "GRAPES":          9,
}

def make_cum(weights):
    cum = []
    c = 0
    for w in weights:
        c += w
        cum.append(c)
    return cum

cum0 = make_cum(weights0)
cum1 = make_cum(weights1)
cum2 = make_cum(weights2)

print("C++ CUM_WEIGHTS arrays:")
print("  Reel 0:", cum0)
print("  Reel 1:", cum1)
print("  Reel 2:", cum2)
print()

def pick_stop(cum):
    r = random.randint(0, 99)
    for idx, limit in enumerate(cum):
        if r < limit:
            return idx
    return 15

N = 1000000
total_coins_won = 0
hits = 0
hit_counts = {
    "3x RED_PANDA": 0,
    "3x YELLOW_PANDA": 0,
    "3x PURPLE_PANDA": 0,
    "3x BLUE_PANDA": 0,
    "3x BROWN_PANDA": 0,
    "3x WATERMELON": 0,
    "3x BANANA": 0,
    "3x GRAPES": 0,
    "2x RED_PANDA": 0,
    "1x RED_PANDA": 0,
}

random.seed(42)
for _ in range(N):
    s0 = strip0[pick_stop(cum0)]
    s1 = strip1[pick_stop(cum1)]
    s2 = strip2[pick_stop(cum2)]

    red_count = (1 if s0 == "RED_PANDA" else 0) + (1 if s1 == "RED_PANDA" else 0) + (1 if s2 == "RED_PANDA" else 0)

    if red_count == 3:
        hits += 1
        total_coins_won += 1500
        hit_counts["3x RED_PANDA"] += 1
    elif red_count == 2:
        hits += 1
        total_coins_won += 15
        hit_counts["2x RED_PANDA"] += 1
    elif red_count == 1:
        hits += 1
        total_coins_won += 6
        hit_counts["1x RED_PANDA"] += 1
    elif s0 == s1 and s1 == s2:
        hits += 1
        pay = PAYOUTS_3OAK[s0]
        total_coins_won += pay
        hit_counts[f"3x {s0}"] += 1

total_wager = N * BET_COINS
sim_rtp = (total_coins_won / total_wager) * 100.0
sim_hf = (hits / N) * 100.0

print(f"=== 1,000,000 SPIN WACKY PANDA MONTE CARLO SIMULATION ===")
print(f"Simulated RTP:           {sim_rtp:.2f}% (Theoretical: 96.65%)")
print(f"Simulated Hit Frequency: {sim_hf:.2f}% (1 in every {N/hits:.2f} spins)")
print("\nHit Breakdown:")
for label, count in hit_counts.items():
    print(f"  {label:<16}: {count:6,d} hits ({count/N*100:6.3f}%)")
