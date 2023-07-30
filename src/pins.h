#ifndef PINS_H
#define PINS_H

constexpr int PIN_INT = 39;
constexpr int PIN_SDA = 40;
constexpr int PIN_SCL = 41;

constexpr int PIN_TXD = 19;
constexpr int PIN_RXD = 20;

constexpr int UART_NUM = 1;
constexpr int UART_BAUD = 921600;
constexpr int UART_RX_BUFFER_LEN = 256;
constexpr int UART_RX_PAT_CHR = 0xA5;
constexpr int UART_RX_PAT_COUNT = 4;

constexpr int PIN_SOLENOID[12] = {
    40, 39, 38, 37, 36, 35, 12, 11, 10, 9, 8, 7};

#endif
