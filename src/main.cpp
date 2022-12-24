#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>

#include "credentials.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "index_html.h"
#include "packets.h"
#include "pins.h"

const char *TAG = "main";

const char *WIFI_AP_SSID = "main";
const char *WIFI_AP_PASSWORD = "outbound operable zips";

typedef struct {
  uint8_t status;
  uint8_t target_status[64];
  int16_t distance[64];
} sensor_data_t;

sensor_data_t sensor_data[3];

packet_t pkt_rx;
SemaphoreHandle_t data_rx;

Preferences prefs;

AsyncWebServer server(80);

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

void setup() {
  data_rx = xSemaphoreCreateBinary();
  assert(data_rx != nullptr);

  prefs.begin("prefs", false);

  xTaskCreate(uart_task, "uart", 8192, nullptr, 10, nullptr);

  WiFi.mode(WIFI_STA);
  ESP_LOGI(TAG, "Trying to connect to %s...", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  if (WiFi.waitForConnectResult() != WL_CONNECTED) {
    ESP_LOGW(TAG, "WiFi connection failed!");
    ESP_LOGI(TAG, "Starting AP %s", WIFI_AP_SSID);
    WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASSWORD);
  }

  MDNS.begin("main");

  ESP_LOGI(TAG, "IP: %s", WiFi.localIP().toString());

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send_P(200, "text/html", index_html);
  });

  server.on("/sensor", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("sensor")) {
      AsyncWebServerResponse *response = request->beginResponse(400, "text/plain", "missing sensor");
      response->addHeader("Access-Control-Allow-Origin", "*");
      request->send(response);
      return;
    }
    const char *_sensor = request->getParam("sensor")->value().c_str();

    int sensor_id;
    if (sscanf(_sensor, "%d", &sensor_id) != 1) {
      AsyncWebServerResponse *response = request->beginResponse(400, "text/plain", "bad sensor");
      response->addHeader("Access-Control-Allow-Origin", "*");
      request->send(response);
      return;
    }

    char buf[1024];
    uint16_t len = 0;

    len = sprintf(buf, "[%d,", sensor_data[sensor_id].status);

    for (uint8_t i = 0; i < 64; i++) {
      len += snprintf(buf + len, sizeof(buf) - len, "[%d,%d],", sensor_data[sensor_id].target_status[i], sensor_data[sensor_id].distance[i]);
    }

    // Replace last , with ]
    buf[len - 1] = ']';

    AsyncWebServerResponse *response = request->beginResponse(200, "application/json", buf);
    response->addHeader("Access-Control-Allow-Origin", "*");
    request->send(response);
  });

  server.onNotFound([](AsyncWebServerRequest *request) {
    request->send(404, "text/plain", "Not found");
  });

  server.begin();
}

bool send_request(uint8_t destination, packet_type type) {
  constexpr int REQUEST_TIMEOUT = 100;
  packet_t pkt;
  pkt.destination = destination;
  pkt.type = request;
  pkt.request.type = type;

  uart_write_bytes(UART_NUM, &pkt, PACKET_COMMON_LEN + sizeof(packet_request_t));
  uart_write_bytes(UART_NUM, &PACKET_TRAILER, sizeof(PACKET_TRAILER));

  return xSemaphoreTake(data_rx, pdMS_TO_TICKS(REQUEST_TIMEOUT)) == pdTRUE;
}

void loop() {
  int sensor_id = 0;
  if (send_request(10 + sensor_id, data)) {
    sensor_data[sensor_id].status = pkt_rx.data.status;
    memcpy(&sensor_data[sensor_id].target_status, &pkt_rx.data.target_status, sizeof(sensor_data[sensor_id].target_status));
    memcpy(&sensor_data[sensor_id].distance, &pkt_rx.data.distances, sizeof(sensor_data[sensor_id].distance));
  } else {
    ESP_LOGI(TAG, "timeout");
    sensor_data[sensor_id].status = timeout;
  }

  delay(100);
}
