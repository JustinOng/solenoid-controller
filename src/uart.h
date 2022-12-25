#ifndef UART_H
#define UART_H

#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "packets.h"
#include "pins.h"

extern packet_t pkt_rx;
extern SemaphoreHandle_t data_rx;

void uart_task(void *arg);

#endif
