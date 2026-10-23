#include <WiFi.h>
#include <WebServer.h>
#include "wifi_http.h"
#include "esp_log.h"
#include "user_app.h"

static const char *TAG = "wifi_http";
static WebServer server(80);

static void handle_root()
{
  server.send(200, "text/html",
    "<h1>TinyMind Emotion Controller</h1>"
    "<p>GET /emotion?name=angry</p>"
    "<p>GET /emotion?name=happy</p>"
    "<p>GET /emotion?name= (empty = show all)</p>"
  );
}

static void handle_emotion()
{
  if (!server.hasArg("name")) {
    server.send(400, "text/plain", "Missing 'name' parameter");
    return;
  }
  String emotion = server.arg("name");
  set_emotion(emotion.c_str());
  server.send(200, "text/plain", "OK: " + emotion);
  ESP_LOGI(TAG, "Emotion set via HTTP: %s", emotion.c_str());
}

static void wifi_http_task(void *arg)
{
  for (;;) {
    server.handleClient();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void wifi_http_init(const char *ssid, const char *password)
{
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  ESP_LOGI(TAG, "Connecting to WiFi '%s'...", ssid);
  while (WiFi.status() != WL_CONNECTED) {
    vTaskDelay(pdMS_TO_TICKS(500));
    Serial.print(".");
  }
  Serial.println();
  ESP_LOGI(TAG, "Connected! IP: %s", WiFi.localIP().toString().c_str());
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  server.on("/", handle_root);
  server.on("/emotion", handle_emotion);
  server.begin();
  ESP_LOGI(TAG, "HTTP server started on port 80");

  xTaskCreatePinnedToCore(wifi_http_task, "http_server", 4 * 1024, NULL, 3, NULL, 0);
}
