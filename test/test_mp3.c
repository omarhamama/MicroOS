/* Host test: read an .mp3 on stdin, decode via src/mp3.c, write 44.1k
 * S16 stereo PCM on stdout. The Python harness in run.sh checks the
 * decoded tone's frequency/purity (and compares to ffmpeg). */
#define MP3_HOST
#include "../src/mp3.c"
#include <stdio.h>

int main(void){
    static uint8_t in[8 << 20];
    size_t n = fread(in, 1, sizeof in, stdin);
    struct mp3dec *m = mp3_open(in, (uint32_t)n);
    if(!m){ fprintf(stderr, "mp3_open failed\n"); return 1; }
    fprintf(stderr, "mp3: %ld Hz, %d ch, %d kbps, ~%u out frames\n",
            (long)mp3_rate(m), mp3_channels(m), mp3_bitrate(m), mp3_total(m));
    static int16_t buf[4096 * 2];
    for(;;){
        int got = mp3_fill(m, buf, 4096);
        if(got > 0) fwrite(buf, sizeof(int16_t) * 2, (size_t)got, stdout);
        if(got < 4096) break;
    }
    mp3_close(m);
    return 0;
}
