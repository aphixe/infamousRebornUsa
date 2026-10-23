"""v193: build assets/kbm_icons.bin (keyboard/mouse button glyphs for the in-game prompt font).
Inputs: _xfer/bak_v189/font_icons_l0.bin = raw level 0 of font_icons_00.dds (512x256 DXT5, oid 120625 in
lang_english.xpp; dump with _xfer/bak_v189/dump_icon_tex.py). The glyph cells (font_icons table) are redrawn by
kbm_glyphs_draw.py (key caps / mouse icons, bindings of cellPad.c v150); logo and '?' stay. GPU textures are stored
bottom-up, so the drawn (upright) image is flipped before DXT5 encoding.
Output: "IFKBMIC1", u32 w, h, fmt(0x88), nbytes, tex_csum(original) ; original level 0 ; KBM level 0.
Needs numpy + Pillow + DejaVuSans-Bold.ttf (falls back to Arial Bold on Windows; set KBM_FONT to use another .ttf).
usage (from infamous-recomp/tools): python make_kbm_icons.py"""
import os, sys, struct, numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from kbm_glyphs_dxt import dxt5_decode, dxt5_encode, tex_csum
from kbm_glyphs_draw import build
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_src = os.path.join(ROOT, '_xfer', 'bak_v189', 'font_icons_l0.bin')
if not os.path.exists(_src):   # dump_icon_tex.py writes it to assets/
    _src = os.path.join(ROOT, 'assets', 'font_icons_l0.bin')
orig = open(_src, 'rb').read()[:131072]
vis = dxt5_decode(orig, 512, 256)[::-1]
kbm = build(vis)
from xbox_glyphs import build as build_xbox
xbox = build_xbox(vis)
xbenc = dxt5_encode(np.ascontiguousarray(xbox[::-1]))
open(os.path.join(ROOT,'assets','xbox_icons.bin'),'wb').write(
    b'IFKBMIC1' + struct.pack('<5I',512,256,0x88,131072,tex_csum(orig,131072)) + orig + xbenc)
enc = dxt5_encode(np.ascontiguousarray(kbm[::-1]))
out = os.path.join(ROOT, 'assets', 'kbm_icons.bin')
open(out, 'wb').write(b'IFKBMIC1' + struct.pack('<5I', 512, 256, 0x88, 131072, tex_csum(orig, 131072)) + orig + enc)
print('wrote', out)

# System dialogs have a separate RGBA font atlas. Add PC glyphs in its unused
# lower-right quadrant, retaining the original controller glyphs for switching.
from pathlib import Path
ui_path = Path(ROOT) / 'assets/ui_font.bin'
if ui_path.exists():
    from kbm_glyphs_draw import CELLS
    ui = ui_path.read_bytes()
    assert ui[:8] == b'IFUIFNT1'
    aw, ah, nt, tl, ni, il, wx, wy = struct.unpack_from('<8I', ui, 8)
    assert (aw, ah) == (1024, 1024)
    records = [struct.unpack_from('<Ihhhhhhhh', ui, 40 + i*20) for i in range(nt+ni)]
    pixels = np.frombuffer(ui, np.uint8, offset=40+(nt+ni)*20).reshape(ah,aw,4).copy()
    # Idempotent when rebuilding the prompt art without re-extracting the font.
    records = [g for g in records if not (0xF180 <= g[0] <= 0xF194 or 0xF280 <= g[0] <= 0xF294)]
    for code, (x,y,w,h) in CELLS.items():
        original = next(g for g in records if g[0] == code)
        assert tuple(original[1:5]) == (x,y+512,w,h)
        pixels[y+512:y+512+h,x+512:x+512+w] = kbm[y:y+h,x:x+w]
        records.append((code+0x100,x+512,y+512,w,h,*original[5:]))
        pixels[y+768:y+768+h,x:x+w] = xbox[y:y+h,x:x+w]
        records.append((code+0x200,x,y+768,w,h,*original[5:]))
    header = b'IFUIFNT1' + struct.pack('<8I',aw,ah,nt,tl,len(records)-nt,il,wx,wy)
    ui_path.write_bytes(header + b''.join(struct.pack('<Ihhhhhhhh',*g) for g in records) + pixels.tobytes())
    print('updated', ui_path, 'with', len(CELLS), 'PC dialog glyphs')
