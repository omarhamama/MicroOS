#!/bin/sh
# Regenerate src/sample_mp3.h — a short synthesized riff encoded to MP3,
# embedded so the player ships a real .mp3 to decode. Needs python3+numpy
# and lame. Run from repo root: sh test/gen_sample_mp3.sh
set -e
cd "$(dirname "$0")/.."
T=$(mktemp -d)
python3 - "$T" <<'PY'
import numpy as np, wave, sys
sr=44100; T=sys.argv[1]
def note(midi,dur,amp=0.35):
    f=440.0*2**((midi-69)/12.0); t=np.arange(int(sr*dur))/sr
    env=np.minimum(1.0,np.minimum(t*40,(dur-t)*8))
    return amp*env*(0.6*np.sin(2*np.pi*f*t)+0.3*np.sin(2*np.pi*2*f*t)+0.1*np.sin(2*np.pi*3*f*t))
seq=[(60,.3),(64,.3),(67,.3),(72,.45),(69,.3),(67,.3),(64,.3),(60,.55)]
pcm=(np.clip(np.concatenate([note(m,d) for m,d in seq]),-1,1)*32767).astype('<i2')
w=wave.open(T+"/s.wav","wb");w.setnchannels(1);w.setsampwidth(2);w.setframerate(sr)
w.writeframes(pcm.tobytes());w.close()
PY
lame --quiet -b 64 -m m "$T/s.wav" "$T/s.mp3"
{ echo "#pragma once"; echo "#include <stdint.h>"
  echo "static const unsigned char sample_mp3[] = {"; xxd -i < "$T/s.mp3"
  echo "};"; echo "static const unsigned int sample_mp3_len = sizeof sample_mp3;"
} > src/sample_mp3.h
rm -rf "$T"; echo "wrote src/sample_mp3.h"
