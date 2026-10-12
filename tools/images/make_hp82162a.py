"""Builds hp82162a.bmp (the Printer view picture) and prtparts.bmp (its
moving parts) from a photo of an HP 82162A, in the same size, place and
colours as hp82161a.bmp (the Tape view), so the two views look like the
same family -- as the real devices are.

    python3 make_hp82162a.py hp82162a-photo.png ../../resources/hp82161a.bmp \
            ../../resources/leds.bmp ../../resources

Needs numpy, opencv-python and Pillow, and the FreeSans Bold font
(Ubuntu: fonts-freefont-ttf). Prints the sprite table -- the Printer view's
coordinates in src/plotterview.cpp (kPrt*) must match it.

  upper body  : the printer photo, warped row by row into the drive's
                silhouette, colour-graded to the drive's lighting
  paper window: the photo's own colours, with the old printout removed
  front panel : the drive's own panel, cleaned, with the printer's
                controls rebuilt from the drive's switch, LEDs and button
                and crisp labels
"""
import sys
import numpy as np, cv2
from PIL import Image, ImageDraw, ImageFont

SRC_PHOTO, DRIVE_BMP, LEDS_BMP, OUT_DIR = sys.argv[1:5]
H, W = 600, 1024
FONT = '/usr/share/fonts/truetype/freefont/FreeSansBold.ttf'

D = np.array(Image.open(DRIVE_BMP).convert('RGB')).astype(np.float32)
P = np.array(Image.open(SRC_PHOTO).convert('RGB')).astype(np.float32)
LEDS = np.array(Image.open(LEDS_BMP).convert('RGB')).astype(np.float32)
YY, XX = np.mgrid[0:H, 0:W].astype(np.float32)

# ── drive silhouette ────────────────────────────────────────────────────
lum = D.mean(axis=2)
m = (lum < 175).astype(np.uint8)
m = cv2.morphologyEx(m, cv2.MORPH_CLOSE, np.ones((9, 9), np.uint8))
n, lab, st, _ = cv2.connectedComponentsWithStats(m)
m = (lab == 1 + np.argmax(st[1:, cv2.CC_STAT_AREA])).astype(np.uint8)
ff = m.copy(); fm = np.zeros((H + 2, W + 2), np.uint8)
cv2.floodFill(ff, fm, (0, 0), 1)
sil = (m | (1 - ff)).astype(bool)
sil[:5] = False                                   # (the cable above the body)

# ── printer photo warped into the drive's outline ──────────────────────
# Front panel border lines: drive 66..807 x 414..547, photo 40.5..371.5 x
# 209..268 -> vertical scale; horizontally each row is fitted edge to edge.
PL = P.mean(axis=2)
SY, Y0D, Y0P = 133 / 59, 414, 209
# The edges, row by row, are noisy (threshold on a photo): fitted with
# straight lines, so the printer's sides come out straight
rows = np.arange(H, dtype=np.float32)
py = Y0P + (rows - Y0D) / SY
raw = np.zeros((H, 4), np.float32)
for Y in range(H):
    r = np.nonzero(PL[int(round(min(max(py[Y], 31), 291)))] < 200)[0]
    r = r[(r > 15) & (r < 395)]
    d = np.nonzero(sil[Y])[0]
    dl0, dr0 = (d.min(), d.max()) if len(d) else (30, 845)
    raw[Y] = (r.min(), r.max(), dl0, dr0)
def smooth(col, y0, y1, deg=3):
    yy = rows[y0:y1]
    c = np.polyfit(yy, raw[y0:y1, col], deg)
    out = raw[:, col].copy()
    out[:] = np.polyval(c, rows)
    return out
# Straight lines (degree 1): a straight line in the photo -- the window
# frame, the paper's edges -- must stay straight (a curve bent them)
pl, pr = smooth(0, 12, 400, 1), smooth(1, 12, 400, 1)
dl, dr = smooth(2, 12, 400, 1), smooth(3, 12, 400, 1)
mapx = (pl[:, None] + (XX - dl[:, None]) * (pr - pl)[:, None] / np.maximum(dr - dl, 1)[:, None]).astype(np.float32)
mapy = np.repeat(py[:, None], W, axis=1).astype(np.float32)
Pw = np.clip(cv2.remap(P, mapx, mapy, cv2.INTER_LANCZOS4, borderMode=cv2.BORDER_REPLICATE), 0, 255)

# ── colour grading: the drive's lighting, the printer's shapes ──────────
dm = np.zeros((H, W), bool)
dm[25:300, 45:425] = True; dm[25:140, 515:820] = True; dm[300:390, 345:820] = True
dm &= sil
Dm = cv2.medianBlur(D.astype(np.uint8), 9).astype(np.float32)
def design(x, y):
    x = x / W; y = y / H
    return np.stack([np.ones_like(x), x, y, x * x, x * y, y * y], -1)
A = design(XX[dm], YY[dm]); Af = design(XX, YY)
fit = np.zeros((H, W, 3), np.float32)
for c in range(3):
    coef, *_ = np.linalg.lstsq(A, Dm[..., c][dm], rcond=None)
    fit[..., c] = Af @ coef
PwL = Pw.mean(axis=2)
ref = np.median(PwL[30:300, 60:440])
G = fit * (PwL / ref)[..., None] ** 1.15

# ── paper window: photo colours, old printout removed ──────────────────
# The paper: the bright columns of the window (found, not fixed -- they
# follow the warp), from under the cover's top to the print head
lumw = np.median(Pw[60:280].mean(axis=2), axis=0)
bright = np.nonzero(lumw[440:800] > 190)[0] + 440
WX0, WX1, WY0, WY1 = int(bright.min()), int(bright.max()) + 1, 24, 347
print(f'// paper: x {WX0}..{WX1 - 1}, y {WY0}..{WY1 - 1}')
win = Pw[WY0:WY1, WX0:WX1]
rowmed = np.median(win, axis=1)
gain = np.median(win[16:240] / np.maximum(rowmed[16:240, None, :], 1), axis=0)
gain = cv2.GaussianBlur(gain[None], (1, 9), 0)[0]
Pc = Pw.copy()
Pc[WY0:WY1, WX0:WX1] = rowmed[:, None, :] * gain[None, :, :]
Pc[WY0:WY1, WX0:WX1] += np.random.default_rng(1).normal(0, 1.2, (WY1 - WY0, WX1 - WX0, 3))
# Below the tear-off bar the paper bends towards the print head: in the
# photo it runs a little to the left there. Each row from the bar down to
# the paper's lower edge moves left by slant(y) -- a gentle curve, 0 at the
# bar, SLANT_PX at the edge. The Printer view shifts the ink the same way
# (src/plotterview.cpp, inkShift()): keep the two in step.
SLANT_Y0, SLANT_Y1, SLANT_PX = 310, 347, 8
def slant(y):
    t = (y - SLANT_Y0 + 1) / (SLANT_Y1 - SLANT_Y0)
    return int(round(SLANT_PX * t ** 1.6)) if SLANT_Y0 <= y < SLANT_Y1 else 0
for y in range(SLANT_Y0, SLANT_Y1):
    k = slant(y)
    if k <= 0: continue
    row = Pc[y].copy()
    Pc[y, WX0 - k:WX1 - k] = row[WX0:WX1]                 # the paper, moved left
    Pc[y, WX1 - k:WX1] = row[WX1 + 1]                       # the gap it leaves: the frame's edge
print('// slant: ' + ' '.join(str(slant(y)) for y in range(SLANT_Y0, SLANT_Y1)))

# slightly warmer, to sit in the drive's light
Pc = Pc * np.array([1.0, 0.985, 0.955], np.float32)

def feather_rect(x0, y0, x1, y1, f):
    a = np.zeros((H, W), np.float32); a[y0:y1, x0:x1] = 1
    return cv2.GaussianBlur(a, (0, 0), f)

wmask = feather_rect(468, 4, 772, 380, 2.0)[..., None]
upper = G * (1 - wmask) + Pc * wmask

# ── upper printer + lower drive panel, drive background outside ────────
seam = np.clip((YY - 396) / 10, 0, 1)[..., None]         # 396..406
out = upper * (1 - seam) + D * seam
s = cv2.GaussianBlur(sil.astype(np.float32), (0, 0), 0.8)[..., None]
out = out * s + D * (1 - s)

# ── the print head: cut out, and the slot under the paper mended ───────
# The photo shows it parked on the right, in the slot just under the
# paper: a small grey carriage with two bright points. The view moves it
# (a sprite); in the picture the slot is continued from its left part.
# x, y, w, h -- under the paper's edge, 78 px in from its right side
# (just the carriage: not the window frame's shadow beside it)
HEAD = (WX1 - 1 - 78, 347, 66, 11)
print(f'// head: x {HEAD[0]}, y {HEAD[1]}, {HEAD[2]} x {HEAD[3]}')
hx, hy, hw, hh = HEAD
head_img = out[hy:hy + hh, hx:hx + hw].copy()
patch = out[hy:hy + hh, hx - 110:hx - 110 + hw].copy()
fade = np.ones((hh, hw, 1), np.float32)
fade[:, :4] = np.linspace(0, 1, 4)[None, :, None]   # blend in at the left
out[hy:hy + hh, hx:hx + hw] = out[hy:hy + hh, hx:hx + hw] * (1 - fade) + patch * fade

# ── front panel: clean the drive's, keep its border lines ──────────────
FX0, FX1, FY0, FY1 = 70, 803, 419, 543                    # inside the border
panel = D[FY0:FY1, FX0:FX1]
pm = np.ones(panel.shape[:2], bool)
for (x0, y0, x1, y1) in [(110, 450, 210, 500), (250, 450, 375, 500), (448, 450, 500, 500),
                         (535, 445, 615, 500), (665, 445, 745, 500), (95, 503, 620, 528),
                         (630, 503, 720, 528)]:
    pm[max(0, y0 - FY0):y1 - FY0, max(0, x0 - FX0):x1 - FX0] = False
py, px = np.mgrid[FY0:FY1, FX0:FX1].astype(np.float32)
Ap = design(px[pm], py[pm]); Apf = design(px, py)
smooth = np.zeros_like(panel)
for c in range(3):
    coef, *_ = np.linalg.lstsq(Ap, panel[..., c][pm], rcond=None)
    smooth[..., c] = Apf @ coef
resid = panel - smooth
sd = resid[pm].std(axis=0)
# grain: mostly brightness, a little colour (like the photo)
rng = np.random.default_rng(2)
nl = rng.normal(0, 1, panel.shape[:2]).astype(np.float32)[..., None]
nc = rng.normal(0, 1, panel.shape).astype(np.float32) * 0.35
noise = cv2.GaussianBlur(nl + nc, (0, 0), 0.7)
noise *= sd.mean() / noise.reshape(-1, 3).std(axis=0)
clean_panel = smooth + noise
pa = np.zeros((H, W), np.float32); pa[FY0 + 2:FY1 - 2, FX0 + 2:FX1 - 2] = 1
pa = cv2.GaussianBlur(pa, (0, 0), 1.2)[..., None]
full = np.zeros_like(out); full[FY0:FY1, FX0:FX1] = clean_panel
out = out * (1 - pa) + full * pa
base_panel = out.copy()          # panel without controls (for the sprites)

# ── parts taken from the drive picture ─────────────────────────────────
sw_off = D[460:492, 120:200]                     # 3-position switch, knob left
sw_on = LEDS[0:32, 80:160]                       # knob right
# Track without knob: the left cap from the ON picture, the plain middle
# of the OFF picture stretched, its right cap
track = np.concatenate([sw_on[:, :5], cv2.resize(sw_off[:, 34:72], (69, 32), interpolation=cv2.INTER_LINEAR),
                        sw_off[:, 74:80]], axis=1)
knob = sw_off[:, 5:31].copy()
led_off = D[456:496, 257:297]                    # POWER LED, off
led_on = LEDS[0:40, 0:40]                        # lit
button = D[449:498, 670:742]                     # OPEN button (dark)

def make_switch(length, pos):
    """Switch image (length x 32) with the knob at pos 0..1."""
    t = track
    if length != 80:
        mid = cv2.resize(t[:, 12:68], (length - 24, 32), interpolation=cv2.INTER_LINEAR)
        t = np.concatenate([t[:, :12], mid, t[:, 68:]], axis=1)
    img = t.copy()
    x = int(round(5 + pos * (length - 10 - 26)))
    img[:, x:x + 26] = knob
    return img

def paste(dst, img, cx, cy, alpha):
    h, w = img.shape[:2]
    x0, y0 = int(round(cx - w / 2)), int(round(cy - h / 2))
    a = alpha[..., None]
    dst[y0:y0 + h, x0:x0 + w] = dst[y0:y0 + h, x0:x0 + w] * (1 - a) + img * a
    return (x0, y0, w, h)

def rect_alpha(w, h, inset, f):
    a = np.zeros((h, w), np.float32); a[inset:h - inset, inset:w - inset] = 1
    return cv2.GaussianBlur(a, (0, 0), f)

def disc_alpha(size, r, f):
    yy, xx = np.mgrid[0:size, 0:size].astype(np.float32) - (size - 1) / 2
    return np.clip((r - np.hypot(xx, yy)) / f + 0.5, 0, 1)

def pressed(img):
    """The button pushed in: darker, a pixel lower, no top highlight."""
    p = img.copy()
    p[2:] = img[:-2]
    p[:4] = img[:4] * 0.85
    return p * 0.72

# Control layout (screen = picture coordinates), measured on the photo
POWER_SW = (170, 452, 58)        # cx, cy, length
INT_SW = (331, 451, 80)
MODE_SW = (331, 512, 80)
LEDS_AT = {'POWER': (140, 504), 'BAT': (201, 504)}
PRINT_BTN = (534, 465)
ADV_BTN = (690, 467)

sw_alpha = {L: rect_alpha(L, 32, 2, 1.0) for L in (58, 80)}
led_alpha_off = disc_alpha(40, 15, 2.0)
led_alpha_on = disc_alpha(40, 19, 3.0)
btn_alpha = rect_alpha(72, 49, 3, 1.2)

def draw_controls(img, power=0, intensity=2, mode=0, led=False, prt=False, adv=False):
    paste(img, make_switch(58, power), *POWER_SW[:2], sw_alpha[58])
    paste(img, make_switch(80, intensity / 4), *INT_SW[:2], sw_alpha[80])
    paste(img, make_switch(80, mode / 2), *MODE_SW[:2], sw_alpha[80])
    paste(img, led_on if led else led_off, *LEDS_AT['POWER'], led_alpha_on if led else led_alpha_off)
    paste(img, led_off, *LEDS_AT['BAT'], led_alpha_off)
    paste(img, pressed(button) if prt else button, *PRINT_BTN, btn_alpha)
    paste(img, pressed(button) if adv else button, *ADV_BTN, btn_alpha)

# ── crisp labels (like the Tape picture's) ─────────────────────────────
SS = 4
font = ImageFont.truetype(FONT, 12 * SS)
INK = (226, 228, 228)

def labels(img):
    """White, slightly condensed bold labels, rendered 4x and reduced."""
    layer = Image.new('L', (W * SS, H * SS), 0)
    d = ImageDraw.Draw(layer)
    def text(s, x, y, anchor='mm'):
        # condensed: draw wide, squeeze 0.93 around the anchor
        tmp = Image.new('L', (len(s) * 14 * SS, 20 * SS), 0)
        ImageDraw.Draw(tmp).text((tmp.width / 2, tmp.height / 2), s, font=font, fill=255, anchor='mm')
        bb = tmp.getbbox(); tmp = tmp.crop(bb)
        tmp = tmp.resize((int(tmp.width * 0.93), tmp.height), Image.LANCZOS)
        if anchor == 'mm':   X = x * SS - tmp.width / 2
        elif anchor == 'rm': X = x * SS - tmp.width
        else:                X = x * SS
        layer.paste(255, (int(X), int(y * SS - tmp.height / 2)), tmp)
    text('OFF', 135, 452, 'rm');     text('ON', 205, 452, 'lm')
    text('POWER', 140, 533);         text('BAT', 201, 533)
    text('INTENSITY', 331, 428)
    text('LIGHT', 285, 451, 'rm');   text('DARK', 377, 451, 'lm')
    text('TRACE', 331, 487)
    text('MAN', 285, 512, 'rm');     text('NORM', 377, 512, 'lm')
    text('MODE', 331, 535)
    text('PRINT', 534, 499)
    text('PAPER', 690, 501);         text('ADVANCE', 690, 514)
    # detent ticks under the intensity slider: one per knob position
    for i in range(5):
        cx = (INT_SW[0] - 40 + 5 + 13 + i * (80 - 10 - 26) / 4) * SS
        d.rectangle([cx - 0.6 * SS, 466 * SS, cx + 0.6 * SS, 470 * SS], fill=200)
    a = np.array(layer.resize((W, H), Image.LANCZOS)).astype(np.float32)[..., None] / 255
    return img * (1 - a) + np.array(INK, np.float32) * a

def finish(img):
    return np.clip(np.round(img), 0, 255).astype(np.uint8)

base = out.copy()
draw_controls(base)
base = labels(base)
Image.fromarray(finish(base)).save(f'{OUT_DIR}/hp82162a.bmp')

# ── sprites: the same picture with a control changed, cut out ──────────
def variant(**kw):
    v = base_panel.copy(); draw_controls(v, **kw); return labels(v)

def cut(img, cx, cy, w, h):
    x0, y0 = int(round(cx - w / 2)), int(round(cy - h / 2))
    return img[y0:y0 + h, x0:x0 + w], (x0, y0)

SPRITE_H = 52
cells = []   # (name, image, screen x, screen y)
base_u8 = finish(base)
def add(name, img, cx, cy, w, h):
    c, (x0, y0) = cut(img, cx, cy, w, h)
    # The sprite must hold every pixel the change touches
    diff = np.any(finish(img) != base_u8, axis=2)
    ys, xs = np.nonzero(diff)
    if len(xs):
        assert xs.min() >= x0 and xs.max() < x0 + w and ys.min() >= y0 and ys.max() < y0 + h, \
            f'{name}: changes at x {xs.min()}..{xs.max()}, y {ys.min()}..{ys.max()} outside {x0},{y0} {w}x{h}'
    cells.append((name, c, x0, y0))
add('power_led', variant(led=True), *LEDS_AT['POWER'], 40, 40)
add('power_on', variant(power=1), *POWER_SW[:2], 64, 32)
for m, nm in ((0, 'man'), (1, 'trace'), (2, 'norm')):
    add('mode_' + nm, variant(mode=m), *MODE_SW[:2], 84, 32)
for i in range(5):
    add(f'intensity_{i}', variant(intensity=i), *INT_SW[:2], 84, 32)
add('print_down', variant(prt=True), *PRINT_BTN, 76, 52)
add('adv_down', variant(adv=True), *ADV_BTN, 76, 52)
# The head is not a change of the picture: its own cell (its parked place)
cells.append(('head', head_img, hx, hy))

sheet_w = sum(c[1].shape[1] for c in cells)
sheet_w = (sheet_w + 3) & ~3
sheet = np.zeros((SPRITE_H, sheet_w, 3), np.uint8)
x = 0
print('// prtparts.bmp: name, column in the sheet, w, h, screen x, y')
for name, c, sx, sy in cells:
    h, w = c.shape[:2]
    sheet[:h, x:x + w] = finish(c)
    print(f'{name:12s} {x:4d} {w:3d} {h:3d} {sx:4d} {sy:4d}')
    x += w
Image.fromarray(sheet).save(f'{OUT_DIR}/prtparts.bmp')
print('sheet', sheet.shape)
