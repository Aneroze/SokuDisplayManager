import numpy as np
from PIL import Image

A = Image.open(r'F:/Games/Touhou/screenshots/2026-09-23 19_03_17-1280p-start-up.png').convert('RGB')  # 1280-start
B = Image.open(r'F:/Games/Touhou/screenshots/2026-09-23 19_03_47-640p-start-up.png').convert('RGB')   # 640-start
a = np.asarray(A, dtype=np.int32)   # (960,1280,3)
b = np.asarray(B, dtype=np.int32)

# 1) Is the 640-start truly NN-2x? Fraction of uniform 2x2 blocks.
def uniform2x2_frac(img):
    h,w,_ = img.shape
    h-=h%2; w-=w%2
    im=img[:h,:w]
    blk=im.reshape(h//2,2,w//2,2,3)
    tl=blk[:,0:1,:,0:1,:]
    same=np.all(blk==tl,axis=(1,3,4))
    return same.mean()
print("uniform 2x2-block fraction:")
print("  640-start :", round(float(uniform2x2_frac(b)),4), "(≈1.0 => nearest-neighbor 2x)")
print("  1280-start:", round(float(uniform2x2_frac(a)),4), "(low => resampled)")

# 2) Recover true 640 render from the 640-start (phase 0 = top-left of each 2x2 block)
src640 = B.resize((640,480), Image.NEAREST)  # take representative; but better take exact TL:
src640_exact = Image.fromarray(np.asarray(B)[::2, ::2, :])  # (480,640,3) exact top-left samples

# 3) Upscale src640_exact to 1280x960 with each filter
filters = {
    'NEAREST': Image.NEAREST,
    'BILINEAR': Image.BILINEAR,
    'BICUBIC': Image.BICUBIC,
    'LANCZOS': Image.LANCZOS,
    'BOX': Image.BOX,
    'HAMMING': Image.HAMMING,
}
cands = {name: np.asarray(src640_exact.resize((1280,960), f), dtype=np.int32) for name,f in filters.items()}

# 4) Static regions (avoid animated flame/wind bg behind the two big portraits, avoid center gears)
# rects as (y0,y1,x0,x1)
rects = {
    'bottom_menu_bar': (918,958, 10,1270),
    'roster_left'    : (372,610, 60,540),
    'roster_right'   : (372,610, 740,1220),
    'name_1p_area'   : (150,210, 1000,1270),
}
def mad(x,y,r):
    y0,y1,x0,x1=r
    return float(np.abs(x[y0:y1,x0:x1]-y[y0:y1,x0:x1]).mean())

print("\nMean abs pixel diff vs the ACTUAL 1280-start shot (lower = closer), per static region:")
hdr = "region".ljust(18) + "".join(n.ljust(10) for n in filters)
print(hdr)
for rn,r in rects.items():
    row = rn.ljust(18)
    for n in filters:
        row += (f"{mad(cands[n],a,r):.2f}").ljust(10)
    print(row)

# Also: direct 640-start vs 1280-start (how different the two look), same regions
print("\nDirect 640-start vs 1280-start MAD (how different the two startups are):")
for rn,r in rects.items():
    print("  ", rn.ljust(18), f"{mad(b,a,r):.2f}")
