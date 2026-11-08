#!/usr/bin/env python3
"""
Generates the 128x128 4-bit indexed LVGL launcher icon for Wacky Panda (src/LvSlotsIcon.h).
Depicts the flagship Red Panda wearing its green watermelon hat inside a bamboo frame.
"""
from PIL import Image, ImageDraw

def create_wacky_panda_icon():
    img = Image.new("RGBA", (128, 128), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)

    # 1. Bamboo Jungle Rounded Backdrop
    draw.rounded_rectangle([12, 12, 116, 116], radius=18, fill=(6, 78, 59, 255), outline=(77, 124, 15, 255), width=6)
    draw.rounded_rectangle([16, 16, 112, 112], radius=14, fill=(247, 231, 196, 255), outline=(132, 204, 22, 255), width=3)

    # Corner rope lashings
    for (rx, ry) in [(10, 10), (104, 10), (10, 104), (104, 104)]:
        draw.rounded_rectangle([rx, ry, rx + 14, ry + 14], radius=4, fill=(217, 119, 6, 255), outline=(120, 53, 15, 255), width=2)

    # Golden halo circle behind Red Panda
    draw.ellipse([24, 24, 104, 104], fill=(251, 191, 36, 255), outline=(217, 119, 6, 255), width=3)

    # 2. Chubby Red Panda Lower Body & Raised Arms
    draw.rounded_rectangle([42, 76, 86, 108], radius=10, fill=(239, 68, 68, 255), outline=(28, 25, 23, 255), width=3)
    # White belly stripe
    draw.rectangle([46, 88, 82, 95], fill=(255, 251, 235, 255))
    # Raised arms
    draw.ellipse([30, 74, 46, 90], fill=(239, 68, 68, 255), outline=(28, 25, 23, 255), width=3)
    draw.ellipse([82, 74, 98, 90], fill=(239, 68, 68, 255), outline=(28, 25, 23, 255), width=3)

    # 3. Red Panda Ears
    draw.ellipse([26, 28, 48, 50], fill=(239, 68, 68, 255), outline=(28, 25, 23, 255), width=3)
    draw.ellipse([80, 28, 102, 50], fill=(239, 68, 68, 255), outline=(28, 25, 23, 255), width=3)

    # 4. Big Creamy White Panda Head
    draw.ellipse([30, 36, 98, 88], fill=(255, 251, 235, 255), outline=(28, 25, 23, 255), width=3)

    # 5. Green Striped Watermelon Helmet/Hat on Top of Head
    draw.pieslice([32, 24, 96, 68], start=180, end=360, fill=(22, 163, 74, 255), outline=(5, 46, 22, 255), width=3)
    # Light green watermelon stripes on hat
    draw.line([50, 27, 46, 46], fill=(134, 239, 172, 255), width=4)
    draw.line([64, 25, 64, 46], fill=(134, 239, 172, 255), width=4)
    draw.line([78, 27, 82, 46], fill=(134, 239, 172, 255), width=4)
    # Little stem on top of hat
    draw.ellipse([54, 18, 64, 26], fill=(22, 163, 74, 255), outline=(5, 46, 22, 255), width=2)

    # 6. Eye Patches, Sparkling Eyes & Big Joyful Grin
    draw.ellipse([42, 50, 58, 66], fill=(127, 29, 29, 255))
    draw.ellipse([70, 50, 86, 66], fill=(127, 29, 29, 255))
    draw.ellipse([47, 53, 55, 61], fill=(255, 255, 255, 255))
    draw.ellipse([73, 53, 81, 61], fill=(255, 255, 255, 255))
    draw.ellipse([49, 55, 53, 59], fill=(28, 25, 23, 255))
    draw.ellipse([75, 55, 79, 59], fill=(28, 25, 23, 255))

    # Big open laughing mouth
    draw.rounded_rectangle([52, 66, 76, 80], radius=5, fill=(127, 29, 29, 255), outline=(28, 25, 23, 255), width=2)
    draw.rectangle([55, 67, 73, 72], fill=(255, 255, 255, 255))
    draw.ellipse([56, 73, 72, 79], fill=(251, 113, 133, 255))

    return img

def encode_lvgl_indexed_4bit(img):
    w, h = img.size
    alpha = img.split()[3]
    pal_img = img.convert("RGB").quantize(colors=15, method=Image.Quantize.MEDIANCUT)
    pal = pal_img.getpalette()[: 15 * 3]
    idx = list(pal_img.getdata())
    a = list(alpha.getdata())

    out = bytearray()
    for i in range(15):
        r, g, b = pal[i * 3: i * 3 + 3]
        out += bytes((b, g, r, 0xFF))  # lv_color32_t: B,G,R,A
    out += bytes((0, 0, 0, 0))         # index 15 = transparent

    rows = bytearray()
    for y in range(h):
        for x in range(0, w, 2):
            n = []
            for dx in (0, 1):
                p = y * w + x + dx
                n.append(15 if (x + dx < w and a[p] < 128) else idx[p] if x + dx < w else 15)
            rows.append(((n[0] & 0xF) << 4) | (n[1] & 0xF))
    return bytes(out + rows)

if __name__ == "__main__":
    img = create_wacky_panda_icon()
    img.save("scripts/slots_icon.png")
    data = encode_lvgl_indexed_4bit(img)
    print(f"Encoded icon data size: {len(data)} bytes (expected 8256)")

    with open("src/LvSlotsIcon.h", "w") as f:
        f.write("#pragma once\n")
        f.write("#include <lvgl.h>\n\n")
        f.write("// Custom 128x128 4-bit Indexed LVGL Icon for Wacky Panda\n")
        f.write("static const LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST uint8_t APP_SLOTS_data[] = {\n")
        for i in range(0, len(data), 16):
            chunk = data[i:i+16]
            f.write("    " + ", ".join(f"0x{b:02x}" for b in chunk) + ",\n")
        f.write("};\n\n")
        f.write("extern const lv_img_dsc_t IMG_APP_SLOTS;\n")
        f.write("const lv_img_dsc_t IMG_APP_SLOTS = {\n")
        f.write("    .header = {\n")
        f.write("        .cf = LV_IMG_CF_INDEXED_4BIT,\n")
        f.write("        .always_zero = 0,\n")
        f.write("        .reserved = 0,\n")
        f.write("        .w = 128,\n")
        f.write("        .h = 128,\n")
        f.write("    },\n")
        f.write(f"    .data_size = {len(data)},\n")
        f.write("    .data = APP_SLOTS_data,\n")
        f.write("};\n")
    print("Generated src/LvSlotsIcon.h successfully!")
