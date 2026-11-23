#ifndef INPUT_H
#define INPUT_H

// 1 if a controller is plugged into GameCube port `chan` (0-3).
int input_connected(int chan);
// Fills the 8-byte SI status response for `chan`; returns 0 if unplugged.
int input_read(int chan, unsigned char out[8]);
// Implemented by the window module.
int input_window_focused(void);

#endif
