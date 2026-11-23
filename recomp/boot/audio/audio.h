#ifndef AUDIO_H
#define AUDIO_H

// host output (audio_out.c)
void audio_out_push(const short* stereo, int samples);
void audio_out_stats(void);
int audio_out_buffered(void);  // samples queued in the device, -1 if no device

// AX DSP replacement (ax_mix.c)
void ax_mix_stats(void);
// Called often by the time module: requests AX frames so the device stays
// ~80 ms ahead (the sound card is the master clock). `wallclock_due` is the
// number of 5 ms periods elapsed, used only when there is no device.
void audio_pump(int wallclock_due);

#endif
