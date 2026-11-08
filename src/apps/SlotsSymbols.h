#pragma once
#include <TFT_eSPI.h>
#include <cstdint>

// Microgaming "Wacky Panda" 8-symbol set (ordered lowest to highest payout)
enum SymbolType : uint8_t {
  SYM_GRAPES       = 0,  // 3x = 3x bet (9 coins @ 3 coins)
  SYM_BANANA       = 1,  // 3x = 8x bet (24 coins @ 3 coins)
  SYM_WATERMELON   = 2,  // 3x = 15x bet (45 coins @ 3 coins)
  SYM_BROWN_PANDA  = 3,  // 3x = 30x bet (90 coins @ 3 coins)
  SYM_BLUE_PANDA   = 4,  // 3x = 60x bet (180 coins @ 3 coins)
  SYM_PURPLE_PANDA = 5,  // 3x = 120x bet (360 coins @ 3 coins)
  SYM_YELLOW_PANDA = 6,  // 3x = 300x bet (900 coins @ 3 coins)
  SYM_RED_PANDA    = 7,  // 1x = 2x (6c), 2x = 5x (15c), 3x = 500x bet (1500 coins @ 3 coins)
  NUM_SYMBOLS      = 8
};

namespace SlotsSymbols {

// Convert 24-bit 0xRRGGBB color to 16-bit RGB565 at compile-time
constexpr uint16_t c24(uint32_t c) {
  return (uint16_t)(((c >> 19) & 0x1F) << 11 | ((c >> 10) & 0x3F) << 5 | ((c >> 3) & 0x1F));
}

// Names for display/HUD
inline const char* getName(uint8_t id) {
  switch (id) {
    case SYM_GRAPES:       return "GRAPES";
    case SYM_BANANA:       return "BANANAS";
    case SYM_WATERMELON:   return "MELONS";
    case SYM_BROWN_PANDA:  return "BROWN PANDA";
    case SYM_BLUE_PANDA:   return "BLUE PANDA";
    case SYM_PURPLE_PANDA: return "PURPLE PANDA";
    case SYM_YELLOW_PANDA: return "YELLOW PANDA";
    case SYM_RED_PANDA:    return "WACKY PANDA";
    default:               return "???";
  }
}

// Helper: draw a cute chubby panda base (ears, torso with stripes, head with dark outline)
inline void drawPandaBase(TFT_eSPI& g, int cx, int cy, uint16_t colDark, uint16_t colMain, uint16_t colLight) {
  const uint16_t colOutline = c24(0x1C1917);
  const uint16_t colWhite   = c24(0xFFFBEB);
  const uint16_t colShadow  = c24(0xE7E5E4);

  // Chubby lower body & feet
  g.fillRoundRect(cx - 10, cy + 5, 20, 14, 6, colOutline);
  g.fillRoundRect(cx - 9, cy + 6, 18, 12, 5, colMain);
  // White belly stripe (classic Wacky Panda striped sweater/belly look)
  g.fillRect(cx - 7, cy + 10, 14, 3, colWhite);
  // Feet pads
  g.fillCircle(cx - 6, cy + 17, 4, colOutline);
  g.fillCircle(cx + 6, cy + 17, 4, colOutline);
  g.fillCircle(cx - 6, cy + 17, 3, colDark);
  g.fillCircle(cx + 6, cy + 17, 3, colDark);

  // Little raised arms
  g.fillCircle(cx - 12, cy + 8, 4, colOutline);
  g.fillCircle(cx + 12, cy + 8, 4, colOutline);
  g.fillCircle(cx - 12, cy + 8, 3, colMain);
  g.fillCircle(cx + 12, cy + 8, 3, colMain);

  // Round Panda Ears
  g.fillCircle(cx - 11, cy - 12, 6, colOutline);
  g.fillCircle(cx + 11, cy - 12, 6, colOutline);
  g.fillCircle(cx - 11, cy - 12, 5, colMain);
  g.fillCircle(cx + 11, cy - 12, 5, colMain);
  g.fillCircle(cx - 11, cy - 12, 2, colLight);
  g.fillCircle(cx + 11, cy - 12, 2, colLight);

  // Big Round Head
  g.fillEllipse(cx, cy - 3, 15, 12, colOutline);
  g.fillEllipse(cx, cy - 3, 14, 11, colShadow);
  g.fillEllipse(cx, cy - 4, 13, 10, colWhite);
}

// Draw full Wacky Panda symbol centered at (cx, cy) inside a 64x56 cell
inline void drawSymbol(TFT_eSPI& g, uint8_t id, int cx, int cy) {
  const uint16_t colOutline = c24(0x1C1917);

  switch (id) {
    case SYM_GRAPES: {
      // Woody stem & bright green leaf at top right (matching Wacky Panda grapes)
      g.drawLine(cx + 2, cy - 16, cx + 6, cy - 11, c24(0x78350F));
      g.drawLine(cx + 3, cy - 16, cx + 7, cy - 11, c24(0x78350F));
      g.fillEllipse(cx + 9, cy - 11, 7, 4, c24(0x14532D));
      g.fillEllipse(cx + 9, cy - 11, 6, 3, c24(0x22C55E));
      g.drawLine(cx + 5, cy - 11, cx + 12, cy - 11, c24(0x86EFAC));

      // Cluster of plump purple grapes (4 rows: 3, 4, 3, 2, 1)
      struct Pt { int8_t dx, dy; };
      static constexpr Pt berries[] = {
        {-6, -9}, {0, -10}, {5, -8},
        {-9, -4}, {-3, -4}, {3, -4}, {8, -3},
        {-7,  2}, {-1,  2}, {5,  2},
        {-4,  8}, {2,   8},
        {-1, 13}
      };
      for (const auto& b : berries) {
        g.fillCircle(cx + b.dx, cy + b.dy, 5, c24(0x3B0764));
      }
      for (const auto& b : berries) {
        g.fillCircle(cx + b.dx, cy + b.dy, 4, c24(0x9333EA));
        g.fillCircle(cx + b.dx - 1, cy + b.dy - 1, 2, c24(0xC084FC));
        g.drawPixel(cx + b.dx - 1, cy + b.dy - 2, c24(0xF3E8FF));
      }
      break;
    }

    case SYM_BANANA: {
      // Jaunty bunch of 3 golden bananas with dark tips & bright highlights
      // Back banana
      g.fillEllipse(cx - 2, cy - 2, 16, 8, c24(0x713F12));
      g.fillEllipse(cx - 2, cy - 2, 15, 7, c24(0xEAB308));
      // Middle banana
      g.fillEllipse(cx + 1, cy + 2, 17, 8, c24(0x713F12));
      g.fillEllipse(cx + 1, cy + 2, 16, 7, c24(0xFACC15));
      // Front curved banana
      g.fillEllipse(cx + 2, cy + 6, 16, 7, c24(0x713F12));
      g.fillEllipse(cx + 2, cy + 6, 15, 6, c24(0xFDE047));
      // Inner crescent cut to give banana curvature
      g.fillEllipse(cx - 5, cy - 9, 13, 6, c24(0xF7E7C4));
      // Redraw top stem bunch where bananas join
      g.fillRoundRect(cx - 15, cy - 6, 6, 7, 2, c24(0x78350F));
      g.fillRect(cx - 14, cy - 5, 4, 4, c24(0xA16207));
      // Specular shine curves
      g.drawFastHLine(cx - 5, cy + 6, 12, c24(0xFEF9C3));
      g.drawFastHLine(cx - 4, cy + 1, 10, c24(0xFEF9C3));
      // Brown blossom tip on right
      g.fillCircle(cx + 16, cy + 4, 2, c24(0x451A03));
      g.fillCircle(cx + 15, cy - 1, 2, c24(0x451A03));
      break;
    }

    case SYM_WATERMELON: {
      // Whole round striped green watermelon on left + juicy red wedge on bottom-right
      // 1. Whole melon (left/back)
      int mx = cx - 4, my = cy - 2;
      g.fillCircle(mx, my, 14, c24(0x052E16));
      g.fillCircle(mx, my, 13, c24(0x16A34A));
      // Dark green stripes
      g.drawEllipse(mx, my, 9, 12, c24(0x14532D));
      g.drawEllipse(mx, my, 8, 12, c24(0x14532D));
      g.drawEllipse(mx, my, 4, 12, c24(0x14532D));
      // Specular shine on melon
      g.fillEllipse(mx - 5, my - 5, 4, 3, c24(0x86EFAC));
      // Curly stem at top
      g.drawLine(mx, my - 14, mx + 3, my - 17, c24(0x14532D));
      g.drawLine(mx + 3, my - 17, mx + 6, my - 15, c24(0x14532D));

      // 2. Red slice wedge in foreground (bottom-right)
      int sx = cx + 6, sy = cy + 7;
      // Outer green rind arc (simulated with triangle + ellipse base)
      g.fillEllipse(sx, sy + 4, 13, 6, c24(0x14532D));
      g.fillEllipse(sx, sy + 3, 12, 5, c24(0x4ADE80));
      // Red flesh triangle
      g.fillTriangle(sx - 11, sy + 3, sx + 11, sy + 3, sx + 2, sy - 10, c24(0x991B1B));
      g.fillTriangle(sx - 10, sy + 3, sx + 10, sy + 3, sx + 2, sy - 8, c24(0xEF4444));
      // Black seeds
      g.drawPixel(sx, sy - 2, 0x0000);
      g.drawPixel(sx + 4, sy, 0x0000);
      g.drawPixel(sx - 3, sy + 1, 0x0000);
      g.drawPixel(sx + 2, sy + 2, 0x0000);
      break;
    }

    case SYM_BROWN_PANDA: {
      // Cute Chestnut Brown Panda with warm blush & bashful grin
      drawPandaBase(g, cx, cy, c24(0x451A03), c24(0x78350F), c24(0xB45309));
      // Eye patches
      g.fillEllipse(cx - 6, cy - 5, 4, 5, c24(0x451A03));
      g.fillEllipse(cx + 6, cy - 5, 4, 5, c24(0x451A03));
      // Bright eyes looking slightly sideways
      g.fillCircle(cx - 5, cy - 6, 2, 0xFFFF);
      g.fillCircle(cx + 7, cy - 6, 2, 0xFFFF);
      g.drawPixel(cx - 5, cy - 6, 0x0000);
      g.drawPixel(cx + 7, cy - 6, 0x0000);
      // Nose & cute smile
      g.fillTriangle(cx - 2, cy - 2, cx + 2, cy - 2, cx, cy, c24(0x451A03));
      g.fillEllipse(cx, cy + 2, 4, 2, c24(0x991B1B));
      g.fillEllipse(cx, cy + 3, 3, 1, c24(0xFB7185));
      // Rosy cheeks
      g.fillEllipse(cx - 9, cy - 1, 2, 1, c24(0xFDA4AF));
      g.fillEllipse(cx + 9, cy - 1, 2, 1, c24(0xFDA4AF));
      break;
    }

    case SYM_BLUE_PANDA: {
      // Cool Blue Panda with dark round sunglasses & big toothy grin!
      drawPandaBase(g, cx, cy, c24(0x1E3A8A), c24(0x2563EB), c24(0x60A5FA));
      // Round dark sunglasses + bridge
      g.drawFastHLine(cx - 3, cy - 6, 6, colOutline);
      g.fillCircle(cx - 6, cy - 6, 5, colOutline);
      g.fillCircle(cx + 6, cy - 6, 5, colOutline);
      g.fillCircle(cx - 6, cy - 6, 4, c24(0x1E293B));
      g.fillCircle(cx + 6, cy - 6, 4, c24(0x1E293B));
      // Sunglasses lens glare
      g.drawLine(cx - 8, cy - 8, cx - 5, cy - 5, c24(0x93C5FD));
      g.drawLine(cx + 4, cy - 8, cx + 7, cy - 5, c24(0x93C5FD));
      // Big wide toothy grin (matching Reel 0 bottom symbol in video)
      g.fillRoundRect(cx - 7, cy - 1, 14, 6, 2, colOutline);
      g.fillRoundRect(cx - 6, cy, 12, 4, 1, 0xFFFF);
      g.drawFastHLine(cx - 6, cy + 2, 12, c24(0x94A3B8));
      g.drawFastVLine(cx - 2, cy, 4, c24(0x94A3B8));
      g.drawFastVLine(cx + 2, cy, 4, c24(0x94A3B8));
      break;
    }

    case SYM_PURPLE_PANDA: {
      // Lovestruck Purple Panda with a grape on head & pink heart eyes!
      drawPandaBase(g, cx, cy, c24(0x581C87), c24(0x9333EA), c24(0xD8B4FE));
      // Plump purple grape perched on top of head
      g.fillCircle(cx + 1, cy - 16, 5, c24(0x3B0764));
      g.fillCircle(cx + 1, cy - 16, 4, c24(0x7E22CE));
      g.fillCircle(cx, cy - 17, 1, c24(0xE9D5FF));
      // Heart eyes (left heart & right heart)
      auto drawHeart = [&](int hx, int hy) {
        g.fillCircle(hx - 2, hy - 1, 2, c24(0xE11D48));
        g.fillCircle(hx + 2, hy - 1, 2, c24(0xE11D48));
        g.fillTriangle(hx - 4, hy, hx + 4, hy, hx, hy + 4, c24(0xE11D48));
        g.drawPixel(hx - 2, hy - 2, c24(0xFFE4E6));
      };
      drawHeart(cx - 6, cy - 6);
      drawHeart(cx + 6, cy - 6);
      // Blushing cheeks & cute kissy/puckered mouth with paws on cheeks
      g.fillEllipse(cx - 9, cy - 1, 3, 2, c24(0xF472B6));
      g.fillEllipse(cx + 9, cy - 1, 3, 2, c24(0xF472B6));
      g.fillCircle(cx, cy + 1, 2, c24(0xBe185D));
      break;
    }

    case SYM_YELLOW_PANDA: {
      // Wacky Golden-Yellow Panda laughing/crying with tear splashes!
      drawPandaBase(g, cx, cy, c24(0xB45309), c24(0xF59E0B), c24(0xFDE047));
      // Eye patches
      g.fillEllipse(cx - 6, cy - 6, 4, 4, c24(0xB45309));
      g.fillEllipse(cx + 6, cy - 6, 4, 4, c24(0xB45309));
      // Tightly squinted "><" laughing/crying eyes
      g.drawLine(cx - 8, cy - 8, cx - 4, cy - 6, colOutline);
      g.drawLine(cx - 8, cy - 4, cx - 4, cy - 6, colOutline);
      g.drawLine(cx + 8, cy - 8, cx + 4, cy - 6, colOutline);
      g.drawLine(cx + 8, cy - 4, cx + 4, cy - 6, colOutline);
      // Bright cyan-blue tear drops splashing outward
      g.fillEllipse(cx - 12, cy - 5, 3, 2, c24(0x0284C7));
      g.fillEllipse(cx - 12, cy - 5, 2, 1, c24(0x7DD3FC));
      g.fillEllipse(cx + 12, cy - 5, 3, 2, c24(0x0284C7));
      g.fillEllipse(cx + 12, cy - 5, 2, 1, c24(0x7DD3FC));
      // Wide open laughing/wailing mouth
      g.fillRoundRect(cx - 6, cy - 2, 12, 7, 3, colOutline);
      g.fillRoundRect(cx - 5, cy - 1, 10, 5, 2, c24(0x991B1B));
      g.fillRect(cx - 4, cy - 1, 8, 2, 0xFFFF); // top teeth
      g.fillEllipse(cx, cy + 3, 4, 2, c24(0xFB7185)); // tongue
      break;
    }

    case SYM_RED_PANDA: {
      // FLAGSHIP SYMBOL: Red Panda wearing the iconic green Watermelon Hat! (3333 Jackpot)
      // Golden halo glow behind the flagship Red Panda (matching center reel in video)
      g.drawCircle(cx, cy, 21, c24(0xD97706));
      g.drawCircle(cx, cy, 20, c24(0xFBBF24));

      drawPandaBase(g, cx, cy + 2, c24(0x991B1B), c24(0xEF4444), c24(0xFCA5A5));

      // Green Striped Watermelon Helmet/Hat on top of head!
      g.fillEllipse(cx, cy - 10, 14, 7, c24(0x052E16));
      g.fillEllipse(cx, cy - 10, 13, 6, c24(0x16A34A));
      // Lighter green watermelon stripes on the hat
      g.drawLine(cx - 6, cy - 15, cx - 8, cy - 6, c24(0x86EFAC));
      g.drawLine(cx,     cy - 16, cx,     cy - 6, c24(0x86EFAC));
      g.drawLine(cx + 6, cy - 15, cx + 8, cy - 6, c24(0x86EFAC));
      // Hat brim (clean cut across forehead)
      g.fillRect(cx - 12, cy - 6, 24, 5, c24(0xFFFBEB));
      g.drawFastHLine(cx - 13, cy - 7, 26, c24(0x052E16));
      g.drawFastHLine(cx - 12, cy - 8, 24, c24(0x4ADE80));
      // Little green stem on top of watermelon hat
      g.fillCircle(cx - 4, cy - 17, 2, c24(0x052E16));
      g.drawPixel(cx - 4, cy - 17, c24(0x4ADE80));

      // Red-brown eye patches & sparkling eyes
      g.fillEllipse(cx - 5, cy - 3, 4, 4, c24(0x7F1D1D));
      g.fillEllipse(cx + 5, cy - 3, 4, 4, c24(0x7F1D1D));
      g.fillCircle(cx - 4, cy - 4, 2, 0xFFFF);
      g.fillCircle(cx + 4, cy - 4, 2, 0xFFFF);
      g.drawPixel(cx - 4, cy - 4, 0x0000);
      g.drawPixel(cx + 4, cy - 4, 0x0000);

      // Big joyful open mouth grin
      g.fillTriangle(cx - 1, cy - 1, cx + 1, cy - 1, cx, cy, c24(0x7F1D1D));
      g.fillRoundRect(cx - 5, cy + 1, 10, 5, 2, c24(0x7F1D1D));
      g.fillRect(cx - 4, cy + 1, 8, 2, 0xFFFF);
      g.fillEllipse(cx, cy + 4, 3, 2, c24(0xFB7185));
      // Rosy cheeks
      g.fillCircle(cx - 9, cy + 1, 2, c24(0xFDA4AF));
      g.fillCircle(cx + 9, cy + 1, 2, c24(0xFDA4AF));
      break;
    }

    default:
      break;
  }
}

// Draw mini confetti symbol centered at (cx, cy) - size ~12x12 to 14x14 px
inline void drawMiniSymbol(TFT_eSPI& g, uint8_t id, int cx, int cy) {
  switch (id) {
    case SYM_GRAPES: {
      g.fillCircle(cx - 2, cy - 1, 3, c24(0x9333EA));
      g.fillCircle(cx + 2, cy - 1, 3, c24(0x9333EA));
      g.fillCircle(cx, cy + 3, 3, c24(0x7E22CE));
      g.fillEllipse(cx + 2, cy - 5, 3, 2, c24(0x22C55E));
      break;
    }

    case SYM_BANANA: {
      g.fillEllipse(cx, cy, 6, 3, c24(0xFACC15));
      g.drawEllipse(cx, cy, 6, 3, c24(0xA16207));
      g.drawPixel(cx - 5, cy - 1, c24(0x78350F));
      break;
    }

    case SYM_WATERMELON: {
      g.fillCircle(cx - 2, cy - 1, 5, c24(0x16A34A));
      g.fillTriangle(cx - 2, cy + 4, cx + 6, cy + 4, cx + 2, cy - 2, c24(0xEF4444));
      g.drawFastHLine(cx - 2, cy + 5, 9, c24(0x15803D));
      break;
    }

    case SYM_BROWN_PANDA:
    case SYM_BLUE_PANDA:
    case SYM_PURPLE_PANDA:
    case SYM_YELLOW_PANDA:
    case SYM_RED_PANDA: {
      uint16_t earCol = c24(0xEF4444);
      if (id == SYM_BROWN_PANDA)  earCol = c24(0x78350F);
      if (id == SYM_BLUE_PANDA)   earCol = c24(0x2563EB);
      if (id == SYM_PURPLE_PANDA) earCol = c24(0x9333EA);
      if (id == SYM_YELLOW_PANDA) earCol = c24(0xF59E0B);

      // Mini panda head + colored ears
      g.fillCircle(cx - 4, cy - 4, 2, earCol);
      g.fillCircle(cx + 4, cy - 4, 2, earCol);
      g.fillCircle(cx, cy, 5, c24(0xFFFBEB));
      g.drawCircle(cx, cy, 5, c24(0x1C1917));
      if (id == SYM_RED_PANDA) {
        // Mini green watermelon hat
        g.fillEllipse(cx, cy - 4, 5, 2, c24(0x16A34A));
      }
      g.fillCircle(cx - 2, cy - 1, 1, earCol);
      g.fillCircle(cx + 2, cy - 1, 1, earCol);
      g.drawPixel(cx, cy + 2, c24(0x991B1B));
      break;
    }

    default:
      break;
  }
}

} // namespace SlotsSymbols
