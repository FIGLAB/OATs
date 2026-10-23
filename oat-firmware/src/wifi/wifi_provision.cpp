#include "wifi_provision.h"
#include "wifi_http.h"

#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <SPIFFS.h>
#include <Arduino.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "driver/gpio.h"
#include "user_config.h"

static const char *TAG = "wifi_prov";

#define NVS_NS   "wifi_creds"
#define NVS_SSID "ssid"
#define NVS_PASS "password"

#define OATS_SERVER "https://oats.daehwakim.com"

static WebServer   ap_server(80);
static Preferences g_prefs;

static volatile bool g_reboot_pending = false;
static void (*g_ap_timeout_cb)(void)  = NULL;

#define AVATAR_BIN "/base.bin"

static bool g_is_provisioning = false;
static char g_ap_ssid[32]     = "OAT";
static char g_ap_ip[16]       = "192.168.4.1";
// Unique device ID derived from MAC address (e.g. "A1B2C3D4E5F6").
// Generated once in wifi_provision_init, before any provisioning logic.
static char g_dev_id[20]      = "";

#define NVS_FORCE_AP "force_ap"

extern "C" bool        wifi_is_provisioning(void) { return g_is_provisioning; }
extern "C" const char *wifi_get_ap_ssid(void)     { return g_ap_ssid; }
extern "C" const char *wifi_get_ap_ip(void)       { return g_ap_ip; }
extern "C" const char *wifi_get_dev_id(void)      { return g_dev_id; }

extern "C" void wifi_enter_provisioning_mode(void)
{
    g_prefs.begin(NVS_NS, false);
    g_prefs.putBool(NVS_FORCE_AP, true);
    g_prefs.end();
    ESP_LOGI(TAG, "AP mode flag saved to NVS — rebooting");
    ESP.restart();
}

// ---------------------------------------------------------------------------
// HTML served by the AP — WiFi credentials setup only
// After a successful connection the user is directed to fig2 for avatar setup
// ---------------------------------------------------------------------------
static const char PROV_HTML[] PROGMEM = R"html(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8"/>
  <meta name="viewport" content="width=device-width, initial-scale=1.0"/>
  <title>Object-Agent Twins (OATs) Setup</title>
  <style>
    *{box-sizing:border-box}
    body{font-family:system-ui,sans-serif;max-width:400px;margin:56px auto;padding:0 20px;color:#222}
    h1{font-size:1.3rem;margin-bottom:4px}
    p.sub{color:#666;font-size:.9rem;margin:0 0 24px}
    label{display:block;margin:14px 0 4px;font-size:.85rem;color:#555}
    input{width:100%;padding:10px;font-size:1rem;border:1px solid #d1d5db;border-radius:8px}
    button{width:100%;padding:12px;font-size:1rem;border:none;border-radius:8px;cursor:pointer;margin-top:18px}
    #btn-connect{background:#2563eb;color:#fff}
    button:disabled{opacity:.4;cursor:default}
    #status{margin-top:14px;font-size:.95rem;min-height:1.4em;line-height:1.6}
    .ok{color:#166534} .err{color:#991b1b} .info{color:#1d4ed8}
    #setup-link{margin-top:12px;word-break:break-all;font-size:.85rem}
  </style>
</head>
<body>
  <h1>Object-Agent Twins (OATs) Setup</h1>
  <p class="sub">Let's start with giving internet access to OAT.</p>
  <label>WiFi Name (SSID)</label>
  <input type="text" id="ssid" autocomplete="off"/>
  <label>Password</label>
  <input type="password" id="pass" autocomplete="off"/>
  <button id="btn-connect">Connect</button>
  <div id="status"></div>
  <div id="setup-link"></div>
  <script>
    const statusEl  = document.getElementById('status');
    const linkEl    = document.getElementById('setup-link');
    const btnConnect = document.getElementById('btn-connect');

    // Prefill saved credentials if available
    (async () => {
      try {
        const res = await fetch('/saved');
        const d = await res.json();
        if (d.ssid) document.getElementById('ssid').value = d.ssid;
        if (d.pass) document.getElementById('pass').value = d.pass;
      } catch(e) {}
    })();

    btnConnect.addEventListener('click', async () => {
      const ssid = document.getElementById('ssid').value.trim();
      const pass = document.getElementById('pass').value;
      if (!ssid) { statusEl.textContent = 'Enter a WiFi name first.'; statusEl.className = 'err'; return; }
      btnConnect.disabled = true;
      linkEl.innerHTML = '';
      statusEl.textContent = 'Connecting...';
      statusEl.className = 'info';
      try {
        const res = await fetch('/connect', {
          method: 'POST',
          headers: {'Content-Type': 'application/x-www-form-urlencoded'},
          body: 'ssid=' + encodeURIComponent(ssid) + '&pass=' + encodeURIComponent(pass)
        });
        const data = await res.json();
        if (data.ok) {
          const devId  = data.dev_id || '';
          const shortId = devId.slice(-6);
          const figUrl = 'https://oats.daehwakim.com/' + shortId;
          statusEl.textContent = 'Connected!';
          statusEl.className = 'ok';
          linkEl.innerHTML =
            'Reconnect to <b>' + document.getElementById('ssid').value.trim() + '</b>, then open:<br>' +
            '<a href="' + figUrl + '" target="_blank">' + figUrl + '</a>';
          fetch('/reboot', { method: 'POST' }).catch(() => {});
        } else {
          statusEl.textContent = 'Wrong WiFi name or password.';
          statusEl.className = 'err';
          btnConnect.disabled = false;
        }
      } catch(e) {
        statusEl.textContent = 'Request failed. Try again.';
        statusEl.className = 'err';
        btnConnect.disabled = false;
      }
    });
  </script>
</body>
</html>
)html";

// ---------------------------------------------------------------------------
// CORS helpers — needed so the fig2 page JS can call ESP32 endpoints
// ---------------------------------------------------------------------------
static void add_cors_headers()
{
    ap_server.sendHeader("Access-Control-Allow-Origin",  "*");
    ap_server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
    ap_server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

// Handles CORS preflight (OPTIONS) and unknown paths
static void handle_not_found()
{
    if (ap_server.method() == HTTP_OPTIONS) {
        add_cors_headers();
        ap_server.send(204);
        return;
    }
    ap_server.send(404, "text/plain", "Not found");
}

// ---------------------------------------------------------------------------
// Web handlers
// ---------------------------------------------------------------------------
static void handle_root()
{
    ap_server.send_P(200, "text/html", PROV_HTML);
}

static void handle_saved()
{
    g_prefs.begin(NVS_NS, true);
    String ssid = g_prefs.getString(NVS_SSID, "");
    String pass = g_prefs.getString(NVS_PASS, "");
    if (ssid == "ssid") ssid = "";
    if (pass == "password") pass = "";
    g_prefs.end();
    auto esc = [](const String &s) {
        String out; out.reserve(s.length());
        for (size_t i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c == '"')  out += "\\\"";
            else if (c == '\\') out += "\\\\";
            else out += c;
        }
        return out;
    };
    String json = String("{\"ssid\":\"") + esc(ssid) + "\",\"pass\":\"" + esc(pass) + "\"}";
    ap_server.send(200, "application/json", json);
}

static void handle_connect()
{
    if (!ap_server.hasArg("ssid") || ap_server.arg("ssid").length() == 0) {
        ap_server.send(400, "application/json", "{\"ok\":false}");
        return;
    }
    String ssid = ap_server.arg("ssid");
    String pass = ap_server.arg("pass");

    WiFi.disconnect(false);
    WiFi.begin(ssid.c_str(), pass.c_str());

    bool connected = false;
    for (int i = 0; i < 20; i++) {
        if (WiFi.status() == WL_CONNECTED) { connected = true; break; }
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    if (!connected) WiFi.disconnect(false);

    g_prefs.begin(NVS_NS, false);
    g_prefs.putString(NVS_SSID, ap_server.arg("ssid"));
    g_prefs.putString(NVS_PASS, ap_server.arg("pass"));
    g_prefs.end();
    ESP_LOGI(TAG, "WiFi credentials saved to NVS");

    if (connected) {
        // Register device on fig2 server to create its user folder
        const char *short_id = g_dev_id + 6;  // last 6 chars of device ID
        HTTPClient reg_http;
        reg_http.begin(String(OATS_SERVER) + "/register");
        reg_http.addHeader("Content-Type", "application/json");
        reg_http.POST(String("{\"device\":\"") + short_id + "\"}");
        reg_http.end();

        ap_server.send(200, "application/json",
            String("{\"ok\":true,\"dev_id\":\"") + g_dev_id + "\"}");
    } else {
        ap_server.send(200, "application/json", "{\"ok\":false}");
    }
}

// Called by the fig2 setup page: fetch a 1-bit bitmap from the given URL
// (on fig2) and save it to SPIFFS.  The URL is passed as JSON: {"url":"..."}
static void handle_fetch_avatar()
{
    add_cors_headers();

    // Parse JSON body for the URL field
    String body = ap_server.arg("plain");
    int start = body.indexOf("\"url\":\"");
    if (start < 0) {
        ap_server.send(400, "application/json", "{\"ok\":false,\"error\":\"Missing url\"}");
        return;
    }
    start += 7;  // skip past "url":"
    int end = body.indexOf('"', start);
    if (end < 0) {
        ap_server.send(400, "application/json", "{\"ok\":false,\"error\":\"Invalid url\"}");
        return;
    }
    String fetch_url = body.substring(start, end);
    ESP_LOGI(TAG, "fetch_avatar: fetching %s", fetch_url.c_str());

    HTTPClient http;
    http.setTimeout(30000);
    http.begin(fetch_url);
    int code = http.GET();
    ESP_LOGI(TAG, "fetch_avatar: server responded %d", code);

    if (code != HTTP_CODE_OK) {
        http.end();
        ap_server.send(502, "application/json",
            String("{\"ok\":false,\"error\":\"HTTP ") + code + "\"}");
        return;
    }

    File f = SPIFFS.open(AVATAR_BIN, "w");
    if (!f) {
        http.end();
        ap_server.send(500, "application/json", "{\"ok\":false,\"error\":\"SPIFFS open failed\"}");
        return;
    }

    WiFiClient *stream = http.getStreamPtr();
    uint8_t chunk[512];
    int total = 0;
    while (http.connected() || stream->available()) {
        int avail = stream->available();
        if (avail > 0) {
            int got = stream->readBytes(chunk, min(avail, (int)sizeof(chunk)));
            f.write(chunk, got);
            total += got;
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    f.close();
    http.end();

    printf("Avatar bitmap saved to SPIFFS: %d bytes\n", total);
    ap_server.send(200, "application/json", "{\"ok\":true}");
}

// Called by the fig2 setup page when the user clicks Done — reboots the device.
static void handle_reboot()
{
    add_cors_headers();
    ap_server.send(200, "application/json", "{\"ok\":true}");
    g_reboot_pending = true;
}

// ---------------------------------------------------------------------------
// AP server task
// ---------------------------------------------------------------------------
static void ap_server_task(void *arg)
{
    const int TICK_MS    = 10;
    const int TIMEOUT_MS = 3 * 60 * 1000;  // 3 minutes
    // const int TIMEOUT_MS = 10 * 1000;
    int       elapsed_ms = 0;

    for (;;) {
        ap_server.handleClient();

        if (g_reboot_pending) {
            vTaskDelay(pdMS_TO_TICKS(500));
            ESP_LOGI(TAG, "Reboot triggered — restarting");
            ESP.restart();
        }

        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
        elapsed_ms += TICK_MS;

        if (elapsed_ms >= TIMEOUT_MS) {
            ESP_LOGI(TAG, "AP mode timeout — turning off AP");
            wifi_off();
            if (g_ap_timeout_cb) {
                g_ap_timeout_cb();
            } else {
                ESP.restart();
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static bool do_wifi_connect(const char *ssid, const char *pass)
{
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, pass);
    ESP_LOGI(TAG, "Connecting to '%s'...", ssid);
    for (int i = 0; i < 10; i++) {
        if (WiFi.status() == WL_CONNECTED) {
            ESP_LOGI(TAG, "Connected! IP: %s", WiFi.localIP().toString().c_str());
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGW(TAG, "Connection timed out");
    return false;
}

// ---------------------------------------------------------------------------
// Public entry point
// ---------------------------------------------------------------------------
extern "C" void wifi_forget_credentials(void)
{
    g_prefs.begin(NVS_NS, false);
    g_prefs.clear();
    g_prefs.end();
    ESP_LOGW(TAG, "WiFi credentials cleared — rebooting into AP mode");
    ESP.restart();
}

extern "C" void wifi_on(void)
{
    if (WiFi.status() == WL_CONNECTED) return;  // already connected

    g_prefs.begin(NVS_NS, true);
    String ssid = g_prefs.getString(NVS_SSID, "");
    String pass = g_prefs.getString(NVS_PASS, "");
    g_prefs.end();

    if (ssid.length() == 0) {
        ESP_LOGW(TAG, "wifi_on: no credentials stored");
        return;
    }

    if (do_wifi_connect(ssid.c_str(), pass.c_str())) {
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    } else {
        ESP_LOGW(TAG, "wifi_on: failed to connect to '%s'", ssid.c_str());
    }
}

extern "C" void wifi_off(void)
{
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    ESP_LOGI(TAG, "WiFi radio disabled");
}

extern "C" void wifi_set_ap_timeout_cb(void (*cb)(void))
{
    g_ap_timeout_cb = cb;
}

extern "C" void wifi_provision_init(void)
{
    // 0. Derive a stable unique device ID from the eFuse base MAC.
    //    ESP.getEfuseMac() reads the hardware eFuse directly — no WiFi
    //    initialisation required.  The uint64_t is laid out little-endian:
    //    mac[0] (OUI first byte) sits at the LSB.
    {
        uint64_t mac64 = ESP.getEfuseMac();
        snprintf(g_dev_id, sizeof(g_dev_id), "%02X%02X%02X%02X%02X%02X",
                 (uint8_t)(mac64),
                 (uint8_t)(mac64 >> 8),
                 (uint8_t)(mac64 >> 16),
                 (uint8_t)(mac64 >> 24),
                 (uint8_t)(mac64 >> 32),
                 (uint8_t)(mac64 >> 40));
        // printf("mac64: 0x%012llX\n", mac64);
    }
    printf("Device ID: %s\n", g_dev_id);

    // 1. Read NVS — check force-AP flag and saved credentials
    g_prefs.begin(NVS_NS, false);
    bool   force_ap   = g_prefs.getBool(NVS_FORCE_AP, false);
    String saved_ssid = g_prefs.getString(NVS_SSID, "");
    String saved_pass = g_prefs.getString(NVS_PASS, "");
    if (force_ap) {
        g_prefs.putBool(NVS_FORCE_AP, false);  // clear so timeout reboot reconnects normally
    }
    g_prefs.end();

    if (!force_ap && saved_ssid.length() > 0) {
        ESP_LOGI(TAG, "Trying saved WiFi: '%s'", saved_ssid.c_str());
        if (do_wifi_connect(saved_ssid.c_str(), saved_pass.c_str())) {
            esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
            wifi_http_init(saved_ssid.c_str(), saved_pass.c_str());
            return;
        }
        ESP_LOGW(TAG, "Saved credentials failed — starting AP provisioning");
        // return; // I added this to make it fallback to avatar when wifi is not connectable. just for demo
    } else if (force_ap) {
        ESP_LOGI(TAG, "AP mode requested by user — skipping saved credentials");
    } else {
        ESP_LOGI(TAG, "No saved credentials — starting AP provisioning");
    }

    // 2. Start AP provisioning
    g_is_provisioning = true;
    SPIFFS.begin(true);

    // Clean up any failed STA connection before starting AP
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    vTaskDelay(pdMS_TO_TICKS(100));

    // Build unique SSID from the last 6 hex chars of the device ID
    snprintf(g_ap_ssid, sizeof(g_ap_ssid), "%s-%s", g_ap_ssid, g_dev_id + 6);

    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(g_ap_ssid);
    WiFi.softAPIP().toString().toCharArray(g_ap_ip, sizeof(g_ap_ip));
    ESP_LOGI(TAG, "AP started: SSID='%s'  IP=%s", g_ap_ssid, g_ap_ip);

    ap_server.on("/",             HTTP_GET,  handle_root);
    ap_server.on("/saved",        HTTP_GET,  handle_saved);
    ap_server.on("/connect",      HTTP_POST, handle_connect);
    ap_server.on("/fetch_avatar", HTTP_POST, handle_fetch_avatar);
    ap_server.on("/reboot",       HTTP_POST, handle_reboot);
    ap_server.onNotFound(handle_not_found);  // also handles OPTIONS preflight
    ap_server.begin();

    xTaskCreate(ap_server_task, "ap_prov", 8 * 1024, nullptr, 3, nullptr);
}
