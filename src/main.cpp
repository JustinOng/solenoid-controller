#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>

#include "credentials.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "index_html.h"
#include "packets.h"
#include "pins.h"
#include "uart.h"

const char *TAG = "main";

const char *WIFI_AP_SSID = "main";
const char *WIFI_AP_PASSWORD = "outbound operable zips";

typedef struct {
  uint8_t status;
  uint8_t target_status[64];
  int16_t distance[64];
} sensor_data_t;

sensor_data_t sensor_data[3];

Preferences prefs;

AsyncWebServer server(80);

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

  server.on("/config/transform", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("id")) {
      AsyncWebServerResponse *response = request->beginResponse(400, "text/plain", "missing id");
      response->addHeader("Access-Control-Allow-Origin", "*");
      request->send(response);
      return;
    }
    const char *raw_id = request->getParam("id")->value().c_str();

    int id;
    if (sscanf(raw_id, "transform%d", &id) != 1) {
      AsyncWebServerResponse *response = request->beginResponse(400, "text/plain", "bad id");
      response->addHeader("Access-Control-Allow-Origin", "*");
      request->send(response);
      return;
    }

    AsyncWebServerResponse *response = request->beginResponse(200, "text/plain", prefs.getString(raw_id, ""));
    response->addHeader("Access-Control-Allow-Origin", "*");
    request->send(response);
  });

  server.on("/config/transform", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("id") || !request->hasParam("value", true)) {
      AsyncWebServerResponse *response = request->beginResponse(400, "text/plain", "missing id/value");
      response->addHeader("Access-Control-Allow-Origin", "*");
      request->send(response);
      return;
    }
    const char *raw_id = request->getParam("id")->value().c_str();
    const char *raw_value = request->getParam("value", true)->value().c_str();

    int id;
    if (sscanf(raw_id, "transform%d", &id) != 1) {
      AsyncWebServerResponse *response = request->beginResponse(400, "text/plain", "bad id");
      response->addHeader("Access-Control-Allow-Origin", "*");
      request->send(response);
      return;
    }

    prefs.putString(raw_id, raw_value);

    AsyncWebServerResponse *response = request->beginResponse(200, "text/plain", "ok");
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
  for (int sensor_id = 0; sensor_id < 3; sensor_id++) {
    if (send_request(10 + sensor_id, data)) {
      sensor_data[sensor_id].status = pkt_rx.data.status;
      memcpy(&sensor_data[sensor_id].target_status, &pkt_rx.data.target_status, sizeof(sensor_data[sensor_id].target_status));
      memcpy(&sensor_data[sensor_id].distance, &pkt_rx.data.distances, sizeof(sensor_data[sensor_id].distance));
    } else {
      sensor_data[sensor_id].status = timeout;
    }
  }

  delay(100);
}
