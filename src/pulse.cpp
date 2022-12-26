#include "pulse.h"

#include <driver/rmt.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

static const char *TAG = "pulse";

typedef struct {
  uint8_t pin;
  float ms;
} pulse_t;

QueueHandle_t pulse_queue;

void execute_pulse(pulse_t &pulse) {
  rmt_item32_t item;

  rmt_set_gpio(RMT_CHANNEL_0, RMT_MODE_TX, (gpio_num_t)pulse.pin, false);

  // Convert ms to us
  item.duration0 = pulse.ms / 0.001;
  item.level0 = 1;
  item.duration1 = 0;
  item.level1 = 0;

  ESP_LOGI(TAG, "Executing pulse of %.2fms on pin %d", pulse.ms, pulse.pin);
  ESP_ERROR_CHECK(rmt_write_items(RMT_CHANNEL_0, &item, 1, true));
}

void pulse_task(void *arg) {
  pulse_queue = xQueueCreate(8, sizeof(pulse_t));
  assert(pulse_queue != nullptr);

  rmt_config_t config;
  memset(&config, 0, sizeof(config));

  config.rmt_mode = RMT_MODE_TX;
  config.channel = RMT_CHANNEL_0;
  // Clocked from APB 80MHz so this should result in 1MHz, giving us a max pulse width of 2^15 ms
  config.clk_div = 80;
  config.mem_block_num = 1;
  config.tx_config.loop_en = 0;
  config.tx_config.carrier_en = 0;
  config.tx_config.idle_output_en = true;
  config.tx_config.idle_level = RMT_IDLE_LEVEL_LOW;

  ESP_ERROR_CHECK(rmt_config(&config));
  ESP_ERROR_CHECK(rmt_driver_install(config.channel, 0, 0));

  while (1) {
    pulse_t pulse;
    if (xQueueReceive(pulse_queue, &pulse, portMAX_DELAY) != pdTRUE) {
      continue;
    }

    execute_pulse(pulse);
  }
}

void pulse_add(uint8_t pin, float ms) {
  pulse_t pulse = {
      .pin = pin,
      .ms = ms};

  if (xQueueSend(pulse_queue, &pulse, 0) != pdTRUE) {
    ESP_LOGW(TAG, "Failed to push to pulse queue!");
  };
}
