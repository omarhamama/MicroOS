#!/bin/sh
# Regenerate src/sample_img.h — a sample PNG + baseline JPEG embedded in
# /pictures so the Preview app has images to open. Needs python3 + PIL.
set -e; cd "$(dirname "$0")/.."; T=$(mktemp -d)
python3 - "$T" <<'PY'
from PIL import Image, ImageDraw
import sys; T=sys.argv[1]; W,H=160,120
im=Image.new('RGB',(W,H))
for y in range(H):
    for x in range(W): im.putpixel((x,y),((x*255)//W,(y*255)//H,128))
d=ImageDraw.Draw(im)
d.ellipse([20,20,80,80],fill=(255,80,40),outline=(255,255,255))
d.rectangle([95,30,140,90],fill=(40,120,255),outline=(255,255,255))
d.line([0,H-1,W-1,0],fill=(255,255,0),width=3)
im.save(T+"/sample.png")
jm=Image.new('RGB',(W,H))
for y in range(H):
    for x in range(W): jm.putpixel((x,y),((x+y)*255//280,(W-x)*255//W,(H-y)*255//H))
jm.save(T+"/photo.jpg","JPEG",quality=85)
PY
{ echo "#pragma once"; echo "#include <stdint.h>"
  echo "static const unsigned char sample_png[] = {"; xxd -i < "$T/sample.png"; echo "};"
  echo "static const unsigned int sample_png_len = sizeof sample_png;"
  echo "static const unsigned char sample_jpg[] = {"; xxd -i < "$T/photo.jpg"; echo "};"
  echo "static const unsigned int sample_jpg_len = sizeof sample_jpg;"
} > src/sample_img.h
rm -rf "$T"; echo "wrote src/sample_img.h"
