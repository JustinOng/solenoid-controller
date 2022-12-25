#include "uart.h"

#include <Arduino.h>

static const char *TAG = "uart";

packet_t pkt_rx;
SemaphoreHandle_t data_rx;

void uart_task(void *arg) {
  QueueHandle_t uart_queue;

  uart_config_t uart_config = {
      .baud_rate = UART_BAUD,
      .data_bits = UART_DATA_8_BITS,
      .parity = UART_PARITY_DISABLE,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .rx_flow_ctrl_thresh = 122,
      .source_clk = UART_SCLK_APB,
  };

  ESP_ERROR_CHECK(uart_driver_install(UART_NUM, UART_RX_BUFFER_LEN, 0, 16, &uart_queue, 0));
  ESP_ERROR_CHECK(uart_param_config(UART_NUM, &uart_config));
  ESP_ERROR_CHECK(uart_set_pin(UART_NUM, PIN_TXD, PIN_RXD, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
  ESP_ERROR_CHECK(uart_set_mode(UART_NUM, UART_MODE_RS485_HALF_DUPLEX));
  uart_enable_pattern_det_baud_intr(UART_NUM, UART_RX_PAT_CHR, UART_RX_PAT_COUNT, 9, 0, 0);

  while (1) {
    char buf[UART_RX_BUFFER_LEN];
    packet_t *pkt;
    packet_t pktr;

    uart_event_t evt;
    if (xQueueReceive(uart_queue, (void *)&evt, portMAX_DELAY) == pdTRUE) {
      if (evt.type == UART_PATTERN_DET) {
        int pos = uart_pattern_pop_pos(UART_NUM);
        if (pos == -1) {
          ESP_LOGE(TAG, "uart queue overflow");
          uart_flush_input(UART_NUM);
          continue;
        }

        uart_read_bytes(UART_NUM, buf, pos + UART_RX_PAT_COUNT, 0);
        ESP_LOGD(TAG, "read %d bytes", pos);

        pkt = (packet_t *)buf;
        if (pkt->header != PACKET_HEADER) {
          ESP_LOGW(TAG, "expected header of %08x but got %08x instead", PACKET_HEADER, pkt->header);
          continue;
        }

        if (pkt->destination != 0xFF) {
          ESP_LOGW(TAG, "expected destination of 0xFF but got %02X instead", pkt->destination);
          continue;
        }

        memcpy(&pkt_rx, pkt, sizeof(pkt_rx));
        xSemaphoreGive(data_rx);
      }
    }
  }
}
