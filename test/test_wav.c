/* Host test: pipe a .wav in on stdin, get 44.1k S16 stereo PCM on stdout.
 * The Python harness in run.sh re-implements wav.c's exact nearest-sample
 * resample and checks our output byte-for-byte. */
#include <stdio.h>
#include <stdint.h>
#include "../src/wav.h"

int main(void)
{
    static uint8_t file[1 << 23];
    size_t n = fread(file, 1, sizeof file, stdin);
    struct wav w;
    if (wav_open(&w, file, (uint32_t)n) < 0) {
        fprintf(stderr, "wav_open failed\n");
        return 1;
    }
    static int16_t buf[2048];
    for (;;) {
        int got = wav_fill(&w, buf, 1024);
        if (got > 0)
            fwrite(buf, sizeof(int16_t) * 2, (size_t)got, stdout);
        if (got < 1024)
            break;
    }
    return 0;
}
