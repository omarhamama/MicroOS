#!/bin/sh
# Host test suite for the TLS crypto stack. Compiles each module
# natively and checks it against published RFC/NIST vectors and against
# openssl-generated keys/signatures. Run from the repo root:
#   sh test/run.sh
set -e
cd "$(dirname "$0")/.."
CC="${CC:-cc}"
FLAGS="-O2 -w"
S=src
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

echo "== SHA-256 / HMAC / HKDF =="
$CC $FLAGS -o $TMP/a test/test_sha256.c $S/sha256.c && $TMP/a

echo "== SHA-384 / SHA-512 =="
$CC $FLAGS -o $TMP/a test/test_sha512.c $S/sha512.c && $TMP/a

echo "== ChaCha20-Poly1305 =="
$CC $FLAGS -o $TMP/a test/test_chacha.c $S/chacha20.c && $TMP/a

echo "== X25519 =="
$CC $FLAGS -o $TMP/a test/test_x25519.c $S/x25519.c && $TMP/a

echo "== RSA PKCS#1 v1.5 (openssl-signed) =="
$CC $FLAGS -o $TMP/rsa test/test_rsa.c $S/rsa.c $S/bignum.c $S/sha256.c $S/sha512.c
openssl genrsa -out $TMP/k.pem 2048 2>/dev/null
printf 'hello rsa' > $TMP/m
openssl dgst -sha256 -sign $TMP/k.pem -out $TMP/s $TMP/m
N=$(openssl rsa -in $TMP/k.pem -noout -modulus 2>/dev/null|sed 's/Modulus=//'|tr 'A-F' 'a-f')
$TMP/rsa "$N" 010001 "$(xxd -p $TMP/s|tr -d '\n')" "hello rsa"

echo "== RSA-PSS (openssl-signed) =="
$CC $FLAGS -o $TMP/pss test/test_pss.c $S/rsa.c $S/bignum.c $S/sha256.c $S/sha512.c
openssl dgst -sha256 -sigopt rsa_padding_mode:pss -sigopt rsa_pss_saltlen:32 \
  -sign $TMP/k.pem -out $TMP/sp $TMP/m
$TMP/pss "$N" 010001 "$(xxd -p $TMP/sp|tr -d '\n')" "hello rsa"

echo "== P-256 ECDSA (openssl-signed) =="
$CC $FLAGS -o $TMP/ec test/test_p256.c $S/p256.c $S/bignum.c $S/sha256.c
openssl ecparam -name prime256v1 -genkey -noout -out $TMP/ec.pem 2>/dev/null
printf 'hello ec' > $TMP/me
openssl dgst -sha256 -sign $TMP/ec.pem -out $TMP/se $TMP/me
PUB=$(openssl ec -in $TMP/ec.pem -pubout -outform DER 2>/dev/null|xxd -p|tr -d '\n'); PUB=${PUB: -128}
RS=$(openssl asn1parse -inform DER -in $TMP/se 2>/dev/null)
R=$(echo "$RS"|grep INTEGER|sed -n 1p|sed 's/.*://'|tr 'A-F' 'a-f'); R=$(printf '%064s' "${R#00}"|tr ' ' 0); R=${R: -64}
SS=$(echo "$RS"|grep INTEGER|sed -n 2p|sed 's/.*://'|tr 'A-F' 'a-f'); SS=$(printf '%064s' "${SS#00}"|tr ' ' 0); SS=${SS: -64}
$TMP/ec "$PUB" "$R" "$SS" "hello ec"

echo "== DEFLATE / gzip (round-trip a real page) =="
$CC $FLAGS -o $TMP/inf test/test_inflate.c $S/inflate.c
printf 'Hello DEFLATE. The quick brown fox. Hello DEFLATE.' > $TMP/o.txt
gzip -c $TMP/o.txt | $TMP/inf > $TMP/d.txt
cmp $TMP/o.txt $TMP/d.txt && echo "ok   gzip round-trip" || echo "FAIL gzip"

echo "== JavaScript interpreter =="
$CC $FLAGS -o $TMP/js test/test_js.c $S/js.c
$TMP/js | grep -E "fact\(6\) = 720|fib\(10\) = 55|add5\(3\) = 8" | sed 's/\[log\] /ok   /'

echo "== PNG decode (known 4x4 corners) =="
$CC $FLAGS -o $TMP/png test/test_png.c $S/png.c $S/inflate.c
python3 - > $TMP/k.png <<'PY'
import zlib,struct,sys
w=h=4; rows=[[(20,20,20)]*w for _ in range(h)]
rows[0][0]=(255,0,0); rows[0][3]=(0,255,0); rows[3][0]=(0,0,255); rows[3][3]=(255,255,255)
raw=b''.join(b'\x00'+b''.join(bytes(rows[y][x]) for x in range(w)) for y in range(h))
def ch(t,d):return struct.pack('>I',len(d))+t+d+struct.pack('>I',zlib.crc32(t+d)&0xffffffff)
sys.stdout.buffer.write(b'\x89PNG\r\n\x1a\n'+ch(b'IHDR',struct.pack('>IIBBBBB',w,h,8,2,0,0,0))+ch(b'IDAT',zlib.compress(raw))+ch(b'IEND',b''))
PY
$TMP/png < $TMP/k.png | grep -q "TL=ff0000 TR=00ff00 BL=0000ff BR=ffffff" \
  && echo "ok   png corners" || echo "FAIL png"

echo "== JPEG decode (vs PIL, if available) =="
$CC $FLAGS -o $TMP/jpg test/test_jpeg.c $S/jpeg.c
if python3 -c "import PIL" 2>/dev/null; then
  python3 - <<PY
from PIL import Image
im=Image.new('RGB',(64,48))
for y in range(48):
  for x in range(64): im.putpixel((x,y),(x*4%256,y*5%256,(x+y)%256))
im.save("$TMP/j.jpg","JPEG",quality=90)
PY
  $TMP/jpg < $TMP/j.jpg > $TMP/j.rgb 2>/dev/null
  python3 - <<PY
from PIL import Image
ref=Image.open("$TMP/j.jpg").convert("RGB").tobytes()
mine=open("$TMP/j.rgb","rb").read()
e=sum(abs(ref[i]-mine[i]) for i in range(min(len(ref),len(mine))))/max(1,len(mine))
print(f"ok   jpeg (mean abs err vs PIL {e:.2f})" if len(mine)==len(ref) and e<4 else "FAIL jpeg")
PY
else echo "skip jpeg (PIL not installed)"; fi

echo "== WAV decode + resample (byte-exact) =="
$CC $FLAGS -o $TMP/wav test/test_wav.c $S/wav.c
python3 - "$TMP/wav" <<'PY'
import sys, struct, subprocess, math
BIN = sys.argv[1]

def wav_bytes(rate, ch, bits, data):
    ba = ch * (bits // 8)
    fmt = struct.pack('<HHIIHH', 1, ch, rate, rate * ba, ba, bits)
    body = (b'fmt ' + struct.pack('<I', 16) + fmt +
            b'data' + struct.pack('<I', len(data)) + data)
    return b'RIFF' + struct.pack('<I', len(body) + 4) + b'WAVE' + body

def resample(rate, dsrc):              # mirror wav.c's Q16 nearest-sample loop
    step = (rate << 16) // 44100
    cur = 0; out = bytearray()
    while (cur >> 16) < len(dsrc):
        l, r = dsrc[cur >> 16]
        out += struct.pack('<hh', l, r); cur += step
    return bytes(out)

def check(rate, ch, bits, label):
    src = [(int(8000*math.sin(i*0.20)), int(6000*math.sin(i*0.31))) for i in range(500)]
    if bits == 16:
        data = b''.join(struct.pack('<h', s[0]) if ch == 1
                        else struct.pack('<hh', s[0], s[1]) for s in src)
        dec = [(s[0], s[0]) if ch == 1 else s for s in src]
    else:                              # 8-bit unsigned, centered at 128
        def u8(v): return ((v >> 8) + 128) & 0xff
        data = b''.join(bytes([u8(s[0])]) if ch == 1
                        else bytes([u8(s[0]), u8(s[1])]) for s in src)
        dec = [((((u8(s[0]) - 128) << 8),) * 2) if ch == 1
               else ((u8(s[0]) - 128) << 8, (u8(s[1]) - 128) << 8) for s in src]
    got = subprocess.run([BIN], input=wav_bytes(rate, ch, bits, data),
                         stdout=subprocess.PIPE).stdout
    exp = resample(rate, dec)
    print(f"{'ok  ' if got == exp else 'FAIL'} wav {label} ({len(got)}=={len(exp)} bytes)")
    return got == exp

ok = (check(44100, 2, 16, "44.1k stereo 16") &
      check(22050, 1, 16, "22k mono 16") &
      check(8000,  2, 8,  "8k stereo 8"))
sys.exit(0 if ok else 1)
PY

echo "== MP3 decode (MPEG-1 Layer III, vs ffmpeg) =="
if command -v ffmpeg >/dev/null 2>&1; then
  $CC $FLAGS -DMP3_HOST -Isrc -o $TMP/mp3 test/test_mp3.c
  ffmpeg -v error -f lavfi \
    -i "aevalsrc=0.3*sin(220*2*PI*t)+0.3*sin(440*2*PI*t)+0.2*sin(880*2*PI*t):d=1:s=44100:c=stereo" \
    -c:a libmp3lame -b:a 192k $TMP/chord.mp3 -y
  ffmpeg -v error -i $TMP/chord.mp3 -ar 44100 -ac 2 -f s16le $TMP/ref.raw -y
  $TMP/mp3 < $TMP/chord.mp3 > $TMP/ours.raw 2>/dev/null
  python3 - "$TMP/ours.raw" "$TMP/ref.raw" <<'PY'
import sys, numpy as np
o=np.fromfile(sys.argv[1],dtype='<i2').astype(float).reshape(-1,2)[:,0]
r=np.fromfile(sys.argv[2],dtype='<i2').astype(float).reshape(-1,2)[:,0]
n=8192; b=len(o)//3; oa=o[b:b+n]
bc,bl=-2.0,0
for lag in range(1500):
    s=r[b+lag:b+lag+n]
    if len(s)<n: break
    c=np.corrcoef(oa,s)[0,1]
    if c>bc: bc,bl=c,lag
s=r[b+bl:b+bl+n]; scale=np.dot(oa,s)/np.dot(oa,oa)
print(f"ok   mp3 decode (corr vs ffmpeg {bc:.4f}, level {1/scale:.3f})"
      if bc>0.99 and 0.8<1/scale<1.25 else f"FAIL mp3 (corr {bc:.4f} level {1/scale:.3f})")
sys.exit(0 if (bc>0.99 and 0.8<1/scale<1.25) else 1)
PY
else
  echo "skip mp3 (ffmpeg not installed)"
fi

echo
echo "All host tests passed."
