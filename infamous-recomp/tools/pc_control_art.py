"""Pack ImageGen artwork into the game's existing glyph cells without stretching.

Pillow is used for asset packing; no GUI toolkit is needed. Coordinates are
explicit because generated sheets do not guarantee uniform grid spacing.
"""
from pathlib import Path
from PIL import Image, ImageOps

SOURCE = Path(__file__).with_name('pc-controls') / 'source.png'
BOXES = {
    'C': (45, 85, 255, 287), 'F': (310, 85, 520, 287),
    'E': (565, 85, 775, 287), 'SPACE': (800, 115, 1130, 287),
    'RMB': (1160, 60, 1355, 300), 'Q': (45, 335, 255, 540),
    'LMB': (320, 325, 510, 565), 'R': (565, 335, 775, 540),
    'ESC': (805, 360, 1080, 540), 'TAB': (1095, 360, 1370, 540),
    'WASD': (25, 600, 310, 820), 'V': (330, 605, 535, 815),
    'MOVE': (555, 600, 830, 835), 'MMB': (870, 590, 1055, 835),
    'ARROWS': (1095, 605, 1385, 820), 'RIGHT': (45, 875, 245, 1070),
    'CTRL': (280, 885, 570, 1065), 'LEFT': (600, 875, 800, 1070),
    'DOWN': (860, 875, 1065, 1070),
}
_source = None

def icon(name, size):
    global _source
    if _source is None:
        _source = Image.open(SOURCE).convert('RGBA')
        if _source.size != (1402, 1122):
            raise ValueError('PC control source must be the reviewed 1402x1122 sheet')
    sprite = _source.crop(BOXES[name])
    bounds = sprite.getchannel('A').getbbox()
    if not bounds:
        raise ValueError(f'Empty PC control sprite: {name}')
    sprite = sprite.crop(bounds)
    w, h = size
    # Uniform scaling plus transparent letterboxing preserves every icon ratio.
    sprite = ImageOps.contain(sprite, (max(1, w - 4), max(1, h - 4)), Image.Resampling.LANCZOS)
    canvas = Image.new('RGBA', size)
    canvas.paste(sprite, ((w - sprite.width) // 2, (h - sprite.height) // 2))
    return canvas
