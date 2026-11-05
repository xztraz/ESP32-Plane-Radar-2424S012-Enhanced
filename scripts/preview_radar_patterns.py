#!/usr/bin/env python3
"""PC preview from the exact 240x240 embedded RGB332 brightness maps.

Compass/range overlay uses the same native pixel-label glyph bitmaps and layout
as the firmware. Aircraft, runway overlay and live tracks intentionally absent.
"""
from pathlib import Path
import re
import math
from PIL import Image, ImageDraw, ImageFont
from generate_radar_patterns import DATA, PATTERNS, SIZE, C, RINGS

ROOT = Path(__file__).resolve().parents[1]
OUT = Path('/mnt/data')
HEADER = (ROOT/'include/ui/pixel_label_font.h').read_text()


def glyph_table(name, height):
    body = re.search(r'k%s\[95\]\[%d\] = \{(.*?)\n\};' % (name, height), HEADER, re.S).group(1)
    rows = re.findall(r'\{([^{}]*)\}', body)
    assert len(rows) == 95, (name, len(rows))
    return [tuple(int(v, 16) for v in re.findall(r'0x[0-9a-fA-F]+', row)) for row in rows]

MAIN = glyph_table('Main', 11)
DETAIL = glyph_table('Detail', 9)

THEMES = {
    'Green': (0, 255, 0),
    'Red': (255, 0, 0),
    'Amber': (255, 176, 0),
    'Cyan': (0, 240, 255),
    'Violet': (178, 105, 255),
    'White': (240, 240, 240),
}

def code_color(level, theme):
    # User-selected RGB888 hue scaled through the existing level 0..7;
    # quantize to RGB332, then expand exactly to 8-bit per pixel.
    r,g,b = THEMES[theme]
    rs = (r*level + 3)//7
    gs = (g*level + 3)//7
    bs = (b*level + 3)//7
    r3 = (rs*7+127)//255
    g3 = (gs*7+127)//255
    b2 = (bs*3+127)//255
    return (r3*255//7, g3*255//7, b2*255//3)


def draw_glyphs(image, x, y, text, theme, level, primary=False):
    pixels = image.load()
    font = MAIN if primary else DETAIL
    step = 7 if primary else 6
    h = 11 if primary else 9
    color = code_color(level, theme)
    for ch in text:
        glyph = font[ord(ch)-32] if 32 <= ord(ch) <= 126 else font[ord('?')-32]
        for gy in range(h):
            for gx in range(step-1):
                if glyph[gy] & (1 << gx):
                    px, py = x+gx, y+gy
                    if 0 <= px < SIZE and 0 <= py < SIZE:
                        pixels[px,py] = color
        x += step


def render(pattern, theme='Green', range_label='10km'):
    file = 'radar_grid.rgb332' if pattern == 'original' else 'radar_%s.rgb332' % pattern
    vals = (DATA/file).read_bytes()
    assert len(vals)==SIZE*SIZE
    lut = [code_color(i,theme) for i in range(8)]
    image = Image.frombytes('RGB', (SIZE,SIZE), bytes().join(bytes(lut[v>>2]) for v in vals))
    # Overlays: same locations as src/ui/radar_display.cpp.
    d = ImageDraw.Draw(image)
    dot_color=code_color(4,theme)
    d.ellipse([C-2,C-2,C+2,C+2],fill=dot_color)
    if pattern != 'super_minimal':
        draw_glyphs(image, C-3, 3, 'N', theme, 4, True)
        draw_glyphs(image, C-3, 226, 'S', theme, 4, True)
        draw_glyphs(image, 3, C-5, 'W', theme, 4, True)
        draw_glyphs(image, 230, C-5, 'E', theme, 4, True)
    # Third-ring label: x = 120+107-6; start at x - text width.
    scale_anchor_x = 221
    left = scale_anchor_x - len(range_label)*6
    top = C - 4
    d = ImageDraw.Draw(image)
    d.rectangle([left-2,top-2,left+len(range_label)*6+1,top+9+1],fill=(0,0,0))
    draw_glyphs(image,left,top,range_label,theme,2 if pattern != 'super_minimal' else 1,False)
    # Crop circular GC9A01; outside physical display is black.
    pixels=image.load()
    for yy in range(SIZE):
        for xx in range(SIZE):
            if (xx-C)**2+(yy-C)**2 >= (SIZE//2)**2:
                pixels[xx,yy]=(0,0,0)
    return image


def main():
    names={'original':'01  Original','classic_ppi':'02  Classic PPI','minimal':'03  Minimal','polar':'04  Polar grid','dashed':'05  Dashed rings','super_minimal':'06  Super Minimal'}
    cell_w=496
    cell_h=540
    image=Image.new('RGB',(cell_w*3,cell_h*2),(19,25,31))
    d=ImageDraw.Draw(image)
    font=ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf',24)
    font_small=ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf',14)
    for idx,pattern in enumerate(PATTERNS):
        disp=render(pattern,'Green')
        disp.save(OUT/('plane_radar_%s_green_native.png'%pattern))
        disp=disp.resize((480,480),Image.Resampling.NEAREST)
        col,row=idx%3,idx//3
        x=col*cell_w+8
        y=row*cell_h+8
        image.paste(disp,(x,y))
        d.text((x+10,y+480+7),names[pattern],font=font,fill=(228,237,239))
        d.text((x+10,y+480+38),'240×240 RGB332 · green',font=font_small,fill=(163,176,181))
    image.save(OUT/'plane_radar_six_pattern_previews.png',optimize=True)
    # Three colors of Classic PPI to show real recoloring of SAME bitmap.
    sheet=Image.new('RGB',(496*3,540),(19,25,31))
    dr=ImageDraw.Draw(sheet)
    for i,theme in enumerate(('Green','Red','Amber')):
        disp=render('classic_ppi',theme)
        disp.save(OUT/f'plane_radar_classic_ppi_{theme.lower()}_native.png')
        sheet.paste(disp.resize((480,480),Image.Resampling.NEAREST),(i*496+8,8))
        dr.text((i*496+18,500),theme,font=font,fill=(228,237,239))
    sheet.save(OUT/'plane_radar_classic_ppi_color_previews.png',optimize=True)
    print('Preview output created at',OUT)

if __name__=='__main__':
    main()
