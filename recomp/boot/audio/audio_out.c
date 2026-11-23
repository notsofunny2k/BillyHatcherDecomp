// Host audio output: 32 kHz stereo 16-bit through waveOut.
// AX frames (160 samples, 5 ms) are batched into 20 ms blocks; at most
// MAX_QUEUED blocks are queued so latency can't build up.
#include <stdio.h>
#include <string.h>

#include <windows.h>
#include <mmsystem.h>

#include "audio.h"

#define RATE 32000
#define BLOCK_SAMPLES 640  // 20 ms
#define NBUF 16
#define MAX_QUEUED 5

static HWAVEOUT wo;
static WAVEHDR hdr[NBUF];
static short buf[NBUF][BLOCK_SAMPLES * 2];
static int fill_buf, fill_pos;
static int opened, failed;
static unsigned long long frames_out, blocks_dropped;

static int open_device(void) {
    if (opened || failed) return opened;
    WAVEFORMATEX f = {0};
    f.wFormatTag = WAVE_FORMAT_PCM;
    f.nChannels = 2;
    f.nSamplesPerSec = RATE;
    f.wBitsPerSample = 16;
    f.nBlockAlign = 4;
    f.nAvgBytesPerSec = RATE * 4;
    if (waveOutOpen(&wo, WAVE_MAPPER, &f, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        fprintf(stderr, "audio: could not open output device\n");
        failed = 1;
        return 0;
    }
    for (int i = 0; i < NBUF; i++) {
        hdr[i].lpData = (LPSTR)buf[i];
        hdr[i].dwBufferLength = sizeof(buf[i]);
        hdr[i].dwFlags = WHDR_DONE;
    }
    opened = 1;
    return 1;
}

static int queued(void) {
    int n = 0;
    for (int i = 0; i < NBUF; i++)
        if ((hdr[i].dwFlags & WHDR_PREPARED) && !(hdr[i].dwFlags & WHDR_DONE)) n++;
    return n;
}

void audio_out_push(const short* stereo, int samples) {
    if (!open_device()) return;
    frames_out++;
    for (int i = 0; i < samples; i++) {
        if (fill_pos == 0) {
            WAVEHDR* h = &hdr[fill_buf];
            if (!(h->dwFlags & WHDR_DONE)) return;  // all buffers busy: drop
            if (h->dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(wo, h, sizeof(*h));
        }
        buf[fill_buf][fill_pos * 2] = stereo[i * 2];
        buf[fill_buf][fill_pos * 2 + 1] = stereo[i * 2 + 1];
        if (++fill_pos == BLOCK_SAMPLES) {
            fill_pos = 0;
            WAVEHDR* h = &hdr[fill_buf];
            if (queued() >= MAX_QUEUED) {
                blocks_dropped++;  // producer ahead of the device: skip this block
            } else {
                h->dwFlags = 0;
                waveOutPrepareHeader(wo, h, sizeof(*h));
                waveOutWrite(wo, h, sizeof(*h));
            }
            fill_buf = (fill_buf + 1) % NBUF;
        }
    }
}

void audio_out_stats(void) {
    fprintf(stderr, "audio: %llu AX frames mixed, %llu blocks dropped\n", frames_out, blocks_dropped);
}
