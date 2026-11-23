#ifndef AUDIO_H
#define AUDIO_H

// host output (audio_out.c)
void audio_out_push(const short* stereo, int samples);
void audio_out_stats(void);

// AX DSP replacement (ax_mix.c)
void ax_mix_stats(void);
// Called by the time module every 5 ms of real time: AI DMA interrupt.
void audio_tick(void);

#endif
