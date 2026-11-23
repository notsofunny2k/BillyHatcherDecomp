#ifndef HLE_TIME_H
#define HLE_TIME_H

// Called by the scheduler when no guest thread can run. Delivers the next
// timed event (alarm, VI retrace...) and returns 1, or returns 0 if nothing
// is pending (deadlock).
int hle_time_idle(void);
// Raises timed interrupts that are due (cheap; call at interrupt-enable points).
void hle_time_poll(void);

#endif
