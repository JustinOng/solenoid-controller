#include <Arduino.h>
#include <AsyncElegantOTA.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>

#include "Update.h"  // Somehow fixes failure to find "Update.h" in AsyncElegantOTA
#include "credentials.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "index_html.h"
#include "packets.h"
#include "pins.h"
#include "pulse.h"
#include "uart.h"

const char *TAG = "main";

const char *WIFI_AP_SSID = "main";
const char *WIFI_AP_PASSWORD = "outbound";

constexpr int SOLENOID_COUNT = 12;
constexpr int SENSOR_COUNT = 4;
constexpr int TRIGGER_COUNT = 8;

typedef struct {
  uint8_t status;
  uint8_t target_status[64];
  int16_t distance[64];
} sensor_data_t;

sensor_data_t sensor_data[SENSOR_COUNT];

typedef struct __attribute__((packed)) {
  int16_t cell;
  int16_t distance_threshold;
} trigger_t;

trigger_t triggers[SOLENOID_COUNT][TRIGGER_COUNT];

typedef struct __attribute__((packed)) {
  float loop;
  float triggered;
} pulse_widths_t;

pulse_widths_t pulses[SOLENOID_COUNT];

Preferences prefs;

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

void bell_loop_task(void *arg) {
  while (1) {
    for (int i = 0; i < SOLENOID_COUNT; i++) {
      while (!prefs.getInt("loop_en", 0)) {
        delay(1000);
      }

      pulse_add(PIN_SOLENOID[i], pulses[i].loop);

      delay(prefs.getInt("loop_delay", 500));
    }
  }
}

void send_response(AsyncWebServerRequest *request, int code, const char *content) {
  AsyncWebServerResponse *response = request->beginResponse(code, "text/plain", content);
  response->addHeader("Access-Control-Allow-Origin", "*");
  request->send(response);
}

void setup() {
  data_rx = xSemaphoreCreateBinary();
  assert(data_rx != nullptr);

  prefs.begin("prefs", false);

  prefs.getBytes("pulses", &pulses, sizeof(pulses));

  xTaskCreate(pulse_task, "pulse", 8192, nullptr, 9, nullptr);
  xTaskCreate(uart_task, "uart", 8192, nullptr, 10, nullptr);

  xTaskCreate(bell_loop_task, "bell", 8192, nullptr, 5, nullptr);

  WiFi.mode(WIFI_STA);
  ESP_LOGI(TAG, "Trying to connect to %s...", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  if (WiFi.waitForConnectResult() != WL_CONNECTED) {
    ESP_LOGW(TAG, "WiFi connection failed!");
    WiFi.disconnect();
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
      send_response(request, 400, "missing sensor");
      return;
    }
    const char *_sensor = request->getParam("sensor")->value().c_str();

    int sensor_id;
    if (sscanf(_sensor, "%d", &sensor_id) != 1 || sensor_id < 0 || sensor_id >= SENSOR_COUNT) {
      send_response(request, 400, "bad sensor");
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

  server.on("/config/string", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("key")) {
      send_response(request, 400, "missing key");
      return;
    }

    const char *key = request->getParam("key")->value().c_str();

    AsyncWebServerResponse *response = request->beginResponse(200, "text/plain", prefs.getString(key, ""));
    response->addHeader("Access-Control-Allow-Origin", "*");
    request->send(response);
  });

  server.on("/config/string", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("key") || !request->hasParam("value", true)) {
      send_response(request, 400, "missing key/value");
      return;
    }

    const char *key = request->getParam("key")->value().c_str();
    const char *value = request->getParam("value", true)->value().c_str();

    prefs.putString(key, value);

    AsyncWebServerResponse *response = request->beginResponse(200, "text/plain", "ok");
    response->addHeader("Access-Control-Allow-Origin", "*");
    request->send(response);
  });

  server.on("/config/int", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("key")) {
      send_response(request, 400, "missing key");
      return;
    }

    const char *key = request->getParam("key")->value().c_str();

    char buf[32];

    snprintf(buf, sizeof(buf), "%d", prefs.getInt(key, 0));

    AsyncWebServerResponse *response = request->beginResponse(200, "text/plain", buf);
    response->addHeader("Access-Control-Allow-Origin", "*");
    request->send(response);
  });

  server.on("/config/int", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("key") || !request->hasParam("value", true)) {
      send_response(request, 400, "missing key/value");
      return;
    }
    const char *key = request->getParam("key")->value().c_str();
    const char *raw_value = request->getParam("value", true)->value().c_str();

    int value;
    if (sscanf(raw_value, "%d", &value) != 1) {
      send_response(request, 400, "bad value");
    }

    prefs.putInt(key, value);

    AsyncWebServerResponse *response = request->beginResponse(200, "text/plain", "ok");
    response->addHeader("Access-Control-Allow-Origin", "*");
    request->send(response);
  });

  server.on("/config/float", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("key")) {
      send_response(request, 400, "missing key");
      return;
    }

    const char *key = request->getParam("key")->value().c_str();

    char buf[32];

    snprintf(buf, sizeof(buf), "%f", prefs.getFloat(key, 0));

    AsyncWebServerResponse *response = request->beginResponse(200, "text/plain", buf);
    response->addHeader("Access-Control-Allow-Origin", "*");
    request->send(response);
  });

  server.on("/config/float", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("key") || !request->hasParam("value", true)) {
      send_response(request, 400, "missing key/value");
      return;
    }
    const char *key = request->getParam("key")->value().c_str();
    const char *raw_value = request->getParam("value", true)->value().c_str();

    float value;
    if (sscanf(raw_value, "%f", &value) != 1) {
      send_response(request, 400, "bad value");
    }

    prefs.putFloat(key, value);

    AsyncWebServerResponse *response = request->beginResponse(200, "text/plain", "ok");
    response->addHeader("Access-Control-Allow-Origin", "*");
    request->send(response);
  });

  server.on("/config/pulses", HTTP_GET, [](AsyncWebServerRequest *request) {
    char buf[1024];
    uint16_t len = 0;

    buf[len++] = '[';

    for (int i = 0; i < SOLENOID_COUNT; i++) {
      len += snprintf(buf + len, sizeof(buf) - len, "[%f,%f],", pulses[i].loop, pulses[i].triggered);
    }

    // Replace last , with ]
    buf[len - 1] = ']';

    AsyncWebServerResponse *response = request->beginResponse(200, "application/json", buf);
    response->addHeader("Access-Control-Allow-Origin", "*");
    request->send(response);
  });

  server.on("/config/pulses", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("pulses")) {
      send_response(request, 400, "missing pulses");
      return;
    }

    const char *raw_pulses = request->getParam("pulses")->value().c_str();

    pulse_widths_t new_pulses[SOLENOID_COUNT];

    int offset = 0;
    for (int i = 0; i < SOLENOID_COUNT; i++) {
      float a, b;
      int read = 0;
      if (sscanf(raw_pulses + offset, "%f,%f,%n", &a, &b, &read) != 2) {
        send_response(request, 400, "bad pulses");
        return;
      }

      offset += read;

      new_pulses[i].loop = a;
      new_pulses[i].triggered = b;
    }

    memcpy(&pulses, &new_pulses, sizeof(pulses));
    prefs.putBytes("pulses", &pulses, sizeof(pulses));
    send_response(request, 200, "ok");
  });

  server.on("/config/triggers", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("id")) {
      send_response(request, 400, "missing id");
      return;
    }

    const char *raw_id = request->getParam("id")->value().c_str();

    int id;

    if (sscanf(raw_id, "%d", &id) != 1 || id < 0 || id >= SOLENOID_COUNT) {
      send_response(request, 400, "bad id");
      return;
    }

    char buf[1024];
    uint16_t len = 0;

    buf[len++] = '[';

    for (int i = 0; i < TRIGGER_COUNT; i++) {
      len += snprintf(buf + len, sizeof(buf) - len, "[%d,%d],", triggers[id][i].cell, triggers[id][i].distance_threshold);
    }

    // Replace last , with ]
    buf[len - 1] = ']';

    AsyncWebServerResponse *response = request->beginResponse(200, "application/json", buf);
    response->addHeader("Access-Control-Allow-Origin", "*");
    request->send(response);
  });

  server.on("/config/triggers", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("id") || !request->hasParam("triggers")) {
      send_response(request, 400, "missing id/triggers");
      return;
    }

    const char *raw_id = request->getParam("id")->value().c_str();
    const char *raw_triggers = request->getParam("triggers")->value().c_str();

    int id;

    if (sscanf(raw_id, "%d", &id) != 1) {
      send_response(request, 400, "bad id");
      return;
    }

    if (id < 0 || id >= SOLENOID_COUNT) {
      send_response(request, 400, "invalid id");
      return;
    }

    trigger_t new_triggers[TRIGGER_COUNT];

    int offset = 0;
    for (int i = 0; i < TRIGGER_COUNT; i++) {
      int a, b;
      int read = 0;
      if (sscanf(raw_triggers + offset, "%d,%d,%n", &a, &b, &read) != 2) {
        send_response(request, 400, "bad triggers");
        return;
      }

      offset += read;

      new_triggers[i].cell = a;
      new_triggers[i].distance_threshold = b;

      ESP_LOGI(TAG, "trigger %d: %d %d", i, a, b);
    }

    memcpy(&triggers[id], &new_triggers, sizeof(new_triggers));
    send_response(request, 200, "ok");
  });

  server.on("/bell", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("id") || !request->hasParam("ms")) {
      send_response(request, 400, "missing id/ms");
      return;
    }

    const char *raw_id = request->getParam("id")->value().c_str();
    const char *raw_ms = request->getParam("ms")->value().c_str();

    int id;
    float ms;

    if (sscanf(raw_id, "%d", &id) != 1) {
      send_response(request, 400, "bad id");
      return;
    }

    if (id < 0 || id >= SOLENOID_COUNT) {
      send_response(request, 400, "invalid id");
      return;
    }

    if (sscanf(raw_ms, "%f", &ms) != 1) {
      send_response(request, 400, "bad ms");
      return;
    }

    pulse_add(PIN_SOLENOID[id], ms);

    AsyncWebServerResponse *response = request->beginResponse(200, "text/plain", "ok");
    response->addHeader("Access-Control-Allow-Origin", "*");
    request->send(response);
  });

  server.onNotFound([](AsyncWebServerRequest *request) {
    request->send(404, "text/plain", "Not found");
  });

  AsyncElegantOTA.begin(&server);

  server.addHandler(&ws);
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

inline bool sensor_valid(uint8_t status) {
  return status == 5 || status == 6 || status == 9;
}

void loop() {
  for (int sensor_id = 0; sensor_id < SENSOR_COUNT; sensor_id++) {
    if (send_request(10 + sensor_id, data)) {
      sensor_data[sensor_id].status = pkt_rx.data.status;
      memcpy(&sensor_data[sensor_id].target_status, &pkt_rx.data.target_status, sizeof(sensor_data[sensor_id].target_status));
      memcpy(&sensor_data[sensor_id].distance, &pkt_rx.data.distances, sizeof(sensor_data[sensor_id].distance));
    } else {
      sensor_data[sensor_id].status = timeout;
    }
  }

  static uint32_t last_trigger[SOLENOID_COUNT] = {0};

  for (int i = 0; i < SOLENOID_COUNT; i++) {
    for (int u = 0; u < TRIGGER_COUNT; u++) {
      trigger_t &trigger = triggers[i][u];

      if (trigger.cell < 0) continue;

      int sensor_id = trigger.cell / 100;
      int cell = trigger.cell % 100;

      if (sensor_data[sensor_id].status != active) continue;
      if (!sensor_valid(sensor_data[sensor_id].target_status[cell])) continue;

      if (sensor_data[sensor_id].distance[cell] < trigger.distance_threshold) {
        // One of the triggers have seen a target, create a pulse

        if (millis() - last_trigger[i] > prefs.getInt("trig_interval", 1000)) {
          ws.printfAll("Trigger solenoid %d from cell %d < threshold %d", i, cell, trigger.distance_threshold);
          pulse_add(PIN_SOLENOID[i], pulses[i].triggered);
          last_trigger[i] = millis();
        }

        break;
      }
    }
  }

  delay(50);
}
