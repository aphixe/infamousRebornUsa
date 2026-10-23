"""Xbox labels in the existing inFamous glyph cells (Pillow asset tooling)."""
import numpy as np
from PIL import Image, ImageDraw
from kbm_glyphs_draw import CELLS, font

LABELS = {0xF080:'B',0xF081:'X',0xF082:'Y',0xF083:'A',0xF084:'LB',
          0xF085:'LT',0xF086:'RB',0xF087:'RT',0xF088:'MENU',0xF089:'VIEW',
          0xF08B:'LS',0xF08D:'RS'}
COLORS = {'A':(125,210,80,255),'B':(245,90,85,255),
          'X':(90,170,255,255),'Y':(245,210,65,255)}

def icon(label, size, symbol=False):
    w,h=size; scale=8
    im=Image.new('RGBA',(w*scale,h*scale));dr=ImageDraw.Draw(im)
    color=COLORS.get(label,(245,245,245,255))
    if label in COLORS:
        side=min(w,h)*scale-4*scale
        x=(w*scale-side)/2;y=(h*scale-side)/2
        box=(x,y,x+side,y+side)
        if not symbol:
            dr.ellipse(box,fill=(25,32,43,255),outline=(140,155,170,255),width=scale)
    else:
        box=(scale*2,scale*2,(w-2)*scale,(h-2)*scale)
        dr.rounded_rectangle(box,radius=scale*3,fill=(25,32,43,255),outline=(140,155,170,255),width=scale)
    px=int(h*scale*.64)
    while px>6:
        f=font(px);b=dr.textbbox((0,0),label,font=f)
        if b[2]-b[0] <= (box[2]-box[0])*.84 and b[3]-b[1] <= (box[3]-box[1])*.70:break
        px-=1
    dr.text(((w*scale-b[2]+b[0])/2-b[0],(h*scale-b[3]+b[1])/2-b[1]),label,font=f,fill=color)
    return im.resize(size,Image.Resampling.LANCZOS)

def build(original):
    out=original.copy()
    for code,label in LABELS.items():
        x,y,w,h=CELLS[code]
        out[y:y+h,x:x+w]=np.array(icon(label,(w,h)))
    return out
