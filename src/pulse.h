#ifndef PULSE_H
#define PULSE_H

#include <stdint.h>

void pulse_task(void *arg);
void pulse_add(uint8_t pin, float ms);

#endif
