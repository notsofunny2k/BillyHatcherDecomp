#ifndef HLE_ALARM_H
#define HLE_ALARM_H

#include <stdint.h>

uint64_t hle_alarm_next(void);
void hle_alarm_poll(uint64_t now);
void hle_alarm_stats(void);

#endif
