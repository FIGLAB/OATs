#include <cstdio>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "user_app.h"
#include "driver/gpio.h"
#include "user_config.h"
#include "esp_log.h"
#include "esp_err.h"

#include "driver/rtc_io.h"
#include "esp_sleep.h"
#include "soc/usb_serial_jtag_struct.h"
#include "esp_wifi.h"
#include <SPIFFS.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include "esp_system.h"
#include <WiFi.h>
#include <vector>

#include "src/ui_src/generated/gui_guider.h"
#include "src/power/board_power_bsp.h"
#include "src/sdcard/sdcard_bsp.h"
#include "src/adc/adc_bsp.h"
#include "src/button_bsp/button_bsp.h"
#include "src/wifi/wifi_provision.h"

// Sensor drivers (C linkage)
extern "C" {
#include "src/i2c/i2c_bsp.h"
#include "src/shtc3/shtc3_bsp.h"
#include "src/audio/audio_bsp.h"
}

static const char *TAG = "user_app";

#define OATS_SERVER "https://oats.daehwakim.com"

// ── Sensor flags loaded from /sensors.json ──────────────────────────────────
static bool sensor_temperature = false;
static bool sensor_humidity    = false;
static bool sensor_sound       = false;

// GPIO pin sensor types (empty string = not used)
#define NUM_GPIO_PINS 4
static const gpio_num_t PIN_GPIOS[NUM_GPIO_PINS] = {
    SENSOR_PIN1, SENSOR_PIN2, SENSOR_PIN3, SENSOR_PIN4
};
static char pin_sensor_type[NUM_GPIO_PINS][16] = {"", "", "", ""};

// ADC channel for each pin (-1 = digital only).
// ESP32-S3: ADC1_CH0=GPIO1, CH1=GPIO2, CH2=GPIO3. GPIO19=ADC2_CH8.
// ADC2 is only available after wifi_off() — gpio_sensor_init() is called after that.
static const int PIN_ADC_CH[NUM_GPIO_PINS] = {
    ADC_CHANNEL_0,  // pin1 → GPIO 1  (ADC1_CH0)
    ADC_CHANNEL_1,  // pin2 → GPIO 2  (ADC1_CH1)
    ADC_CHANNEL_2,  // pin3 → GPIO 3  (ADC1_CH2)
    ADC_CHANNEL_8,  // pin4 → GPIO 19 (ADC2_CH8)
};
// ADC unit for each pin (1 = ADC1, 2 = ADC2, 0 = digital).
static const int PIN_ADC_UNIT[NUM_GPIO_PINS] = { 1, 1, 1, 2 };

// ── Sensor history ring buffer ───────────────────────────────────────────────
// Always collected. Comment out DEBUG_SENSOR_HISTORY below to stop uploading
// the buffer to the server (and to let wifi_off() run after file sync).
#define SENSOR_HIST_LEN          336                   // 1 entry/hour × 24h × 14 days
#define SENSOR_HIST_INTERVAL_MS  (3600UL * 1000UL)    // push one sample per hour
#define HIST_BIN_PATH            "/hist.bin"
typedef struct {
    uint32_t t_ms;
    float    temp;    // -1 if sensor not enabled
    float    rh;      // -1 if sensor not enabled
    float    sound;   // -1 if sensor not enabled
    float    pin[NUM_GPIO_PINS];  // mV or digital level; -1 if pin unused
} sensor_hist_t;
static sensor_hist_t s_hist[SENSOR_HIST_LEN];
static int      s_hist_pos   = 0;   // index of next write slot
static int      s_hist_count = 0;   // entries filled (0 → SENSOR_HIST_LEN)
static uint32_t s_last_hist_push_ms = 0;
static bool     s_hist_initialized  = false;
static bool     need_initial_emotion = true;

// Current (latest) sensor readings — updated every 2 s
static float s_cur_temp  = -1.0f;
static float s_cur_rh    = -1.0f;
static float s_cur_sound = -1.0f;
static float s_cur_pin[NUM_GPIO_PINS] = {-1.0f, -1.0f, -1.0f, -1.0f};

// ── Battery ──────────────────────────────────────────────────────────────────
// VBAT is read on the module's BAT_ADC line (GPIO4, see VBAT_ADC_CHANNEL).
// There is no fuel gauge and the charger's status pin is not wired to the
// MCU, so the percentage is an estimate from the cell voltage.
// Read policy: the battery is sampled once a minute in the quiet part of the
// sensor cycle (rail and WiFi off), right before each upload, and every minute
// in charging mode. Only samples taken on battery become a percentage: with a
// charger on the cell the pin reads the charger's voltage (a loose plug once
// showed a flat "98 %" for an empty pack), so those samples set the charging
// flag and drive a phone-style rising number: it starts from the last level
// known on battery (kept in flash across reboots), rises with a corrected
// voltage estimate, holds at 99 on the charger's plateau and shows 100 once
// the charger has finished. The gauge shows a bolt while charging.
#define BATTERY_ICON_ON_DISPLAY 1      // 0 = don't draw the gauge on the emotion image
#define CHARGING_REFRESH_MS     (60UL * 1000UL)   // charging-screen update period

static int  s_batt_mv      = -1;    // last battery voltage, mV, whatever the power source (-1 = none yet)
static int  s_batt_pct     = -1;    // state of charge 0-100 from the last sample taken on battery (-1 = none yet)
static int  s_batt_mv_batt = -1;    // voltage of that on-battery sample
static bool s_charging     = false; // last sample looked like a charger on the cell (USB connected)
// Set true when shutdown begins — prevents sensor task from overwriting sleep screen
static volatile bool s_shutting_down = false;
static bool s_in_charging_mode = false;   // charging screen is up (boot on USB or shutdown on USB)
static bool s_no_internet      = false;   // last emotion request failed: base face + offline banner
#define CHARGE_WATCH_MS         (10UL * 1000UL)   // battery poll period on the setup screen
static void show_sleep_and_power_off(void);       // defined with the power button code below
static uint32_t s_batt_last_ms = 0; // millis() of the last sample
#define BATTERY_SAMPLE_MS       (60UL * 1000UL)   // periodic sample interval while running
// Charging detection without a VBUS line: the charger holds the cell node at
// its 4.2 V regulation, which this ADC reads as 4.176-4.186 V, so anything at
// or above BATT_CHARGE_FLOOR_MV is the charger's plateau. On battery the
// voltage can only fall between quiet samples, so a rise of
// BATT_CHARGE_RISE_MV means a charger was plugged in, and a one-minute drop of
// BATT_UNPLUG_DROP_MV from a charging reading means it was unplugged (the
// step at unplug is 60-120 mV; the natural decline on battery is a few mV a
// minute). Measured here: 4.14 V on USB vs 4.02 V on battery at about 80 %.
#define BATT_CHARGE_FLOOR_MV    4150
#define BATT_CHARGE_RISE_MV     60
#define BATT_UNPLUG_DROP_MV     40
// While current flows into the cell the pin reads about BATT_CHARGE_OFFSET_MV
// above the cell's resting voltage (measured: 4.14 V on USB vs 4.02 V on
// battery at the same charge, of which ~50 mV is the board's own load).
// When is a charge finished? Once the plateau was reached, a fall of
// BATT_TERMINATED_DROP_MV below it means the charger let go, and
// BATT_CV_RAMP_MS on the plateau (the constant-voltage phase of a 0.2 A charge
// on a 400 mAh pack) means it is as good as done. Below the plateau the
// charger is only known to be pushing current while the voltage rises
// (BATT_CHARGE_RISE_WINDOW_MV over the last three samples); flat means it has
// stopped and the pin reads the resting cell.
#define BATT_CHARGE_OFFSET_MV       70
#define BATT_CHARGE_RISE_WINDOW_MV  5
#define BATT_TERMINATED_DROP_MV     30
// On a USB host with the voltage flat below the plateau and the cell this
// high, the charger is idle because it has finished (seen after a reboot on
// USB with a full pack): count that as complete too.
#define BATT_IDLE_FULL_MV           4050
#define BATT_CV_RAMP_MS             (40UL * 60UL * 1000UL)
static int      s_batt_pct_chg = -1;   // level shown while on the charger (-1 = no sample yet this session)
static int      s_cv_base_pct  = -1;   // shown level when this session reached the plateau (ramp start)
static int      s_last_pct_nvs = -1;   // last on-battery level, kept in flash: the floor after a reboot on USB
static int      s_batt_mv_chg_max = 0; // highest pin reading this charging session (dips are the charger, not the cell)
static int      s_chg_hist[4];         // last four charging samples, mV (rise detection)
static int      s_chg_hist_n = 0;
static uint32_t s_cv_since_ms  = 0;    // when this session first reached the plateau (0 = not yet)
// What "full" means is learned from the pack, the way a phone does it: after a
// charge the charger completed, the on-battery reading taken
// BATT_FULL_LEARN_SAMPLES minutes into battery use is stored in flash as 100 %,
// and the on-battery scale is stretched so that reading is full. Until one is
// learned the plain table is used. The board's own draw lowers the on-battery
// reading by about BATT_LOAD_OFFSET_MV, which is added back before the lookup.
#define BATT_LOAD_OFFSET_MV      50
#define BATT_FULL_LEARN_SAMPLES  8
#define BATT_FULL_MIN_MV         3950   // sanity bounds for the learned "full" reading
#define BATT_FULL_MAX_MV         4250
static int  s_full_mv        = 0;      // learned load-corrected reading that counts as 100 % (0 = not learned)
static bool s_charge_done    = false;  // the last charging session completed (kept in flash, cleared when a new one starts)
static int  s_batt_on_batt_n = 0;      // on-battery samples since boot or since the last charging session

static void battery_prefs_load(void)
{
    Preferences prefs;
    if (!prefs.begin("batt", true)) return;    // namespace not there yet: defaults
    s_full_mv      = prefs.getInt("full_mv", 0);
    s_charge_done  = prefs.getBool("done", false);
    s_last_pct_nvs = prefs.getInt("last_pct", -1);
    prefs.end();
}
static void battery_set_last_pct(int pct)
{
    if (pct == s_last_pct_nvs) return;
    s_last_pct_nvs = pct;
    Preferences prefs;
    if (prefs.begin("batt", false)) { prefs.putInt("last_pct", pct); prefs.end(); }
}
static void battery_set_done(bool done)
{
    if (done == s_charge_done) return;
    s_charge_done = done;
    Preferences prefs;
    if (prefs.begin("batt", false)) { prefs.putBool("done", done); prefs.end(); }
}
static void battery_set_full(int mv)
{
    if (mv == s_full_mv) return;
    s_full_mv = mv;
    Preferences prefs;
    if (prefs.begin("batt", false)) { prefs.putInt("full_mv", mv); prefs.end(); }
}
// Table percentage -> shown percentage, stretched so the learned "full" is 100.
static int battery_scaled_pct(int table_pct)
{
    if (s_full_mv <= 0) return table_pct;
    int full_pct = battery_percent(s_full_mv);
    if (full_pct < 50) return table_pct;       // nonsense reference: ignore it
    int p = table_pct * 100 / full_pct;
    return p > 100 ? 100 : p;
}
// The level to show: the charging estimate while charging, else the last on-battery one.
static inline int battery_shown_pct(void) { return s_charging ? s_batt_pct_chg : s_batt_pct; }
static bool s_usb_powered_at_boot = false;  // set once by detect_usb_power_at_boot()

// USB power detection.
// The module has no VBUS sense line, but its power path tells us anyway:
// VBUS feeds VSYS directly through D4, while the battery reaches VSYS only
// when the latch (GPIO17 -> T2 -> Q4) is on or the PWR button is physically
// held (D9 pulls Q4's gate low). So if the firmware is running at boot, the
// latch is not set yet, and the PWR button is up, the power must be USB.
// This sees any 5 V source (PC, wall adapter, power bank), unlike the SOF
// check below which only sees an enumerating host.
// Must be called before board_div.VBAT_POWER_ON().
static bool detect_usb_power_at_boot(void)
{
    // The "running with PWR up" argument only holds for a cold start or a
    // reset by the USB peripheral (flashing). After a software restart (the
    // BOOT double press that enters AP mode, the PWR press on the charging
    // screen), a crash or a watchdog, the latch may still be feeding us from
    // the battery with the button up, so treat those as battery.
    esp_reset_reason_t why = esp_reset_reason();
    if (why != ESP_RST_POWERON && why != ESP_RST_EXT && why != ESP_RST_USB && why != ESP_RST_JTAG)
        return false;
    gpio_set_direction(PWR_BUTTON_PIN, GPIO_MODE_INPUT);   // R58 10K pull-up on the module
    for (int i = 0; i < 20; i++) {                         // ~20 ms stable "up" reading
        if (gpio_get_level(PWR_BUTTON_PIN) == 0) return false;   // button held: normal boot
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return true;
}

// True when a USB host is enumerated on the native USB port: a full-speed
// host sends a SOF frame every 1 ms, so the frame counter keeps advancing.
// Only a secondary hint: it misses chargers, and GP20 (USB D+) doubles as
// the sensor power rail on this board.
static bool usb_host_present(void)
{
    uint32_t sof1 = USB_SERIAL_JTAG.fram_num.sof_frame_index;
    vTaskDelay(pdMS_TO_TICKS(3));
    uint32_t sof2 = USB_SERIAL_JTAG.fram_num.sof_frame_index;
    return sof1 != sof2;
}

// Take one battery reading and update the cached value used by the display
// gauge and the upload. Call while the system is quiet (sensor rail and WiFi
// off) so the reading is close to the resting cell voltage.
static void battery_sample(void)
{
    int mv = battery_read_mv();                 // already a 16-sample average
    if (mv <= 0) return;                        // ADC error: keep the last reading
    bool was_charging = s_charging;             // for the sleep-on-charge rule below
    bool had_sample   = s_batt_mv >= 0;
    bool usb = s_usb_powered_at_boot || usb_host_present();
    bool charging;
    if (usb || mv >= BATT_CHARGE_FLOOR_MV) charging = true;
    else if (s_charging)                   charging = (s_batt_mv < 0) || mv > s_batt_mv - BATT_UNPLUG_DROP_MV;
    else                                   charging = (s_batt_mv_batt >= 0) && mv >= s_batt_mv_batt + BATT_CHARGE_RISE_MV;
    s_batt_mv      = mv;
    s_charging     = charging;
    s_batt_last_ms = millis();
    if (!charging) {                            // a reading on battery is the real thing
        int v = mv + BATT_LOAD_OFFSET_MV;       // undo the board's own draw
        s_batt_mv_batt = mv;
        s_batt_pct     = battery_scaled_pct(battery_percent(v));
        battery_set_last_pct(s_batt_pct);       // the floor for the next charge, even after a reboot
        s_cv_since_ms     = 0;
        s_batt_pct_chg    = -1;                 // the next charging session starts fresh
        s_batt_mv_chg_max = 0;
        s_chg_hist_n      = 0;
        s_batt_on_batt_n++;
        // Learn "full": a few minutes after a completed charge, once the surface
        // charge has settled, this reading is what this charger leaves in this pack.
        if (s_charge_done && s_batt_on_batt_n == BATT_FULL_LEARN_SAMPLES) {
            if (v >= BATT_FULL_MIN_MV && v <= BATT_FULL_MAX_MV) {
                battery_set_full(s_full_mv > 0 ? (s_full_mv * 3 + v) / 4 : v);
                s_batt_pct = battery_scaled_pct(battery_percent(v));
            }
            battery_set_done(false);
        }
    } else {
        if (s_chg_hist_n == 0) {                // a new charging session
            battery_set_done(false);
            s_batt_on_batt_n = 0;
            s_cv_base_pct    = -1;
        }
        // Within a session the pin can dip (the charger pausing near full, a
        // load spike) without the cell losing charge, so judge the phase by
        // the session's highest reading and keep the plateau timer running.
        if (mv > s_batt_mv_chg_max) s_batt_mv_chg_max = mv;
        if (s_batt_mv_chg_max >= BATT_CHARGE_FLOOR_MV && s_cv_since_ms == 0)
            s_cv_since_ms = millis() ? millis() : 1;
        s_chg_hist[s_chg_hist_n % 4] = mv; s_chg_hist_n++;
        bool rising = s_chg_hist_n < 4 || (mv - s_chg_hist[s_chg_hist_n % 4]) >= BATT_CHARGE_RISE_WINDOW_MV;
        // Finished? Once it is, it stays finished for the session.
        bool done = s_charge_done;
        if (!done) {
            if (s_cv_since_ms == 0) done = !rising && usb && mv >= BATT_IDLE_FULL_MV;   // idle charger, full cell
            else done = mv < BATT_CHARGE_FLOOR_MV - BATT_TERMINATED_DROP_MV            // came off the plateau
                     || (millis() - s_cv_since_ms) >= BATT_CV_RAMP_MS;                  // or long enough on it
        }
        if (done) {
            battery_set_done(true);
            s_batt_pct_chg = 100;               // finished: full, by this pack's own definition
        } else {
            // Phone style: a number that only rises. The floor is the last level
            // known on battery, from this session or from flash before a reboot.
            int floor_pct = s_batt_pct >= 0 ? s_batt_pct : s_last_pct_nvs;
            int est;
            if (s_cv_since_ms == 0) {           // below the plateau: the voltage tracks the cell
                est = battery_scaled_pct(battery_percent(rising ? mv - BATT_CHARGE_OFFSET_MV : mv));
            } else {                            // on the plateau: the voltage says nothing more, ramp to 99 with time
                if (s_cv_base_pct < 0)
                    s_cv_base_pct = s_batt_pct_chg >= 0 ? s_batt_pct_chg
                                  : battery_scaled_pct(battery_percent(BATT_CHARGE_FLOOR_MV - BATT_CHARGE_OFFSET_MV));
                est = s_cv_base_pct + (int)((uint64_t)(millis() - s_cv_since_ms) * (99 - s_cv_base_pct) / BATT_CV_RAMP_MS);
            }
            if (est > 99) est = 99;             // 100 is reserved for "finished"
            if (est < floor_pct) est = floor_pct;
            if (est < s_batt_pct_chg) est = s_batt_pct_chg;   // never down while the cable is in
            s_batt_pct_chg = est;
        }
    }
    printf("battery:     %d mV, %s, showing %d%% (last on-battery %d%%, stored %d%%, full ref %d mV, done %d)\n",
           mv, charging ? "charging" : "on battery", battery_shown_pct(), s_batt_pct, s_last_pct_nvs, s_full_mv, s_charge_done);

    // Sleep on charge: a cable arriving while the device is running (face or
    // setup screen) sends it to the charging screen, exactly like a PWR long
    // press on USB. Only a transition counts: a device deliberately started on
    // USB with PWR held (development) reads "charging" from its first sample
    // on and keeps running.
    if (charging && !was_charging && had_sample && !s_shutting_down && !s_in_charging_mode) {
        ESP_LOGI(TAG, "Charger detected while running: going to sleep (charging screen)");
        wifi_set_ap_timeout_cb(NULL);           // setup screen: the AP timeout must not call it too
        s_shutting_down = true;                 // parks the sensor task, as the PWR long press does
        show_sleep_and_power_off();
    }
}

// ── Accumulators for averaging sensor readings over each history interval ────
static double s_acc_temp    = 0.0;
static double s_acc_rh      = 0.0;
static double s_acc_sound   = 0.0;
static double s_acc_pin[NUM_GPIO_PINS] = {0.0, 0.0, 0.0, 0.0};
static int    s_acc_count    = 0;  // number of samples accumulated

// Accumulate one sensor reading into the running sums.
// Uses the global sensor enable flags to decide which values to accumulate.
static void accumulate_sensor_reading(float temp, float rh, float sound,
                                      const float pin[NUM_GPIO_PINS])
{
    if (sensor_temperature) s_acc_temp  += temp;
    if (sensor_humidity)    s_acc_rh    += rh;
    if (sensor_sound)       s_acc_sound += sound;
    for (int i = 0; i < NUM_GPIO_PINS; i++) {
        if (pin_sensor_type[i][0] != '\0') s_acc_pin[i] += pin[i];
    }
    s_acc_count++;
}

// ── Push one sample into the history ring buffer ────────────────────────────
static void push_history(uint32_t now_ms, float temp, float rh, float sound,
                         const float pin[NUM_GPIO_PINS])
{
    sensor_hist_t &e = s_hist[s_hist_pos];
    e.t_ms    = now_ms;
    e.temp    = temp;
    e.rh      = rh;
    e.sound   = sound;
    for (int i = 0; i < NUM_GPIO_PINS; i++) e.pin[i] = pin[i];
    s_hist_pos = (s_hist_pos + 1) % SENSOR_HIST_LEN;
    if (s_hist_count < SENSOR_HIST_LEN) s_hist_count++;
}

// Update the most recent history entry in-place with current accumulated averages.
// Does NOT push a new entry or reset accumulators — used when inference is needed
// (anomaly / initial emotion) but the hourly interval has not yet elapsed.
static void update_latest_history(uint32_t now_ms, float cur_temp, float cur_rh,
                                  float cur_sound, const float cur_pin[NUM_GPIO_PINS])
{
    if (s_hist_count == 0) return;  // nothing to update
    int latest = (s_hist_pos - 1 + SENSOR_HIST_LEN) % SENSOR_HIST_LEN;
    sensor_hist_t &e = s_hist[latest];
    e.t_ms  = now_ms;
    e.temp  = (s_acc_count > 0 && sensor_temperature) ? (float)(s_acc_temp  / s_acc_count) : cur_temp;
    e.rh    = (s_acc_count > 0 && sensor_humidity)    ? (float)(s_acc_rh    / s_acc_count) : cur_rh;
    e.sound = (s_acc_count > 0 && sensor_sound)       ? (float)(s_acc_sound / s_acc_count) : cur_sound;
    for (int i = 0; i < NUM_GPIO_PINS; i++) {
        e.pin[i] = (s_acc_count > 0 && pin_sensor_type[i][0] != '\0') ? (float)(s_acc_pin[i] / s_acc_count) : cur_pin[i];
    }
}

// Compute the average of all accumulated readings and push into history.
// Resets the accumulators afterwards.
static void push_averaged_history(uint32_t now_ms, float cur_temp, float cur_rh,
                                  float cur_sound, const float cur_pin[NUM_GPIO_PINS])
{
    float avg_temp    = (s_acc_count > 0 && sensor_temperature) ? (float)(s_acc_temp  / s_acc_count) : cur_temp;
    float avg_rh      = (s_acc_count > 0 && sensor_humidity)    ? (float)(s_acc_rh    / s_acc_count) : cur_rh;
    float avg_sound   = (s_acc_count > 0 && sensor_sound)       ? (float)(s_acc_sound / s_acc_count) : cur_sound;
    float avg_pin[NUM_GPIO_PINS];
    for (int i = 0; i < NUM_GPIO_PINS; i++) {
        avg_pin[i] = (s_acc_count > 0 && pin_sensor_type[i][0] != '\0') ? (float)(s_acc_pin[i] / s_acc_count) : cur_pin[i];
    }

    push_history(now_ms, avg_temp, avg_rh, avg_sound, avg_pin);

    // Reset accumulators
    s_acc_temp    = 0.0;
    s_acc_rh      = 0.0;
    s_acc_sound   = 0.0;
    for (int i = 0; i < NUM_GPIO_PINS; i++) s_acc_pin[i] = 0.0;
    s_acc_count   = 0;
}

// ── Anomaly detection ────────────────────────────────────────────────────────
// Anchored reference values — initialized from the most recent history entry,
// updated to the current reading whenever an anomaly is detected.
static bool  s_anchor_initialized = false;
static float s_anchor_temp  = 0.0f;
static float s_anchor_rh    = 0.0f;
static float s_anchor_sound = 0.0f;
static float s_anchor_pin[NUM_GPIO_PINS] = {0.0f, 0.0f, 0.0f, 0.0f};

// Dynamic thresholds: dual EMA of |deviation| × margin multiplier, with a floor minimum.
// Fast EMA (α = 0.6) adapts within ~3 samples (6 s) for quick reaction.
// Slow EMA (α = 0.065) retains ~1 min memory (30 samples @ 2 s) so past volatility
// keeps the threshold elevated even if recent readings are calm.
// Threshold = K * max(fast, slow), ensuring spikes from 30–60 s ago still matter.
#define ANOMALY_ALPHA_FAST  0.6f
#define ANOMALY_ALPHA_SLOW  0.065f   // ~30-sample half-life ≈ 1 minute @ 2 s
#define ANOMALY_K           3.0f

// Floor minimums — prevent threshold from shrinking to near-zero during calm periods
#define ANOMALY_MIN_TEMP   0.5f   // °C
#define ANOMALY_MIN_RH     3.0f   // %
#define ANOMALY_MIN_SOUND  3.0f   // dB

// Fast EMA trackers (react quickly)
static float s_avg_dev_temp_fast  = 0.0f;
static float s_avg_dev_rh_fast    = 0.0f;
static float s_avg_dev_sound_fast = 0.0f;
static float s_avg_dev_pin_fast[NUM_GPIO_PINS] = {0.0f, 0.0f, 0.0f, 0.0f};

// Slow EMA trackers (retain memory ≥ 1 min)
static float s_avg_dev_temp_slow  = 0.0f;
static float s_avg_dev_rh_slow    = 0.0f;
static float s_avg_dev_sound_slow = 0.0f;
static float s_avg_dev_pin_slow[NUM_GPIO_PINS] = {0.0f, 0.0f, 0.0f, 0.0f};

// Floor minimums per GPIO sensor type
static float anomaly_min_for_pin(int i)
{
    if (strcmp(pin_sensor_type[i], "light") == 0)     return 80.0f;   // lux -- dark: 7 lux, office light at night: 70-127 lux, 8am sunlight: 300 lux, 11am: 2600 lux, 12pm: 3000lux
    if (strcmp(pin_sensor_type[i], "motion") == 0)    return 10.0f;   // cm/s²
    if (strcmp(pin_sensor_type[i], "occupancy") == 0) return 500.0f;  // mV
    if (strcmp(pin_sensor_type[i], "water") == 0)     return 300.0f;  // mV
    return 30.0f;
}

// Drift rate: each 2s cycle, anchor moves 40% toward current value.
#define ANCHOR_DRIFT_RATE 0.6f

// ── Fixed-anchor anomaly detection (temperature & humidity only) ─────────────
// These anchors do NOT drift — they only update when an anomaly is detected.
// This catches slow, cumulative changes that the drifting anchors would miss.
#define FIXED_THRESH_TEMP  1.0f   // °C
#define FIXED_THRESH_RH    5.0f   // %
static float s_fixed_anchor_temp = 0.0f;
static float s_fixed_anchor_rh   = 0.0f;
static bool  s_fixed_anchor_initialized = false;

// Cooldown: suppress anomalies for 30 s after the last detection
#define ANOMALY_COOLDOWN_MS  (10UL * 1000UL)
static uint32_t s_last_anomaly_ms = 0;  // millis() of last true return

// Check whether any enabled sensor has deviated from its anchor by more than
// the threshold. Uses the global sensor enable flags to decide which sensors
// to check. If anomaly detected, update anchors and return true.
// After returning true, forces false for ANOMALY_COOLDOWN_MS.
static bool check_anomaly(float temp, float rh, float sound,
                          const float pin[NUM_GPIO_PINS])
{
    if (!s_anchor_initialized) return false;

    // Cooldown: if an anomaly was detected recently, suppress further detections
    // but still let anchor drift run below.
    uint32_t now = millis();
    bool in_cooldown = (s_last_anomaly_ms != 0) &&
                       ((now - s_last_anomaly_ms) < ANOMALY_COOLDOWN_MS);

    bool anomaly = false;

    if (sensor_temperature) {
        float d = temp - s_anchor_temp;
        if (d < 0) d = -d;
        float avg_dev = fmaxf(s_avg_dev_temp_fast, s_avg_dev_temp_slow);
        float thresh = fmaxf(ANOMALY_K * avg_dev, ANOMALY_MIN_TEMP);
        if (d >= thresh){
          anomaly = true;
          s_anchor_temp  = temp;
        }
        s_avg_dev_temp_fast = s_avg_dev_temp_fast * (1.0f - ANOMALY_ALPHA_FAST) + d * ANOMALY_ALPHA_FAST;
        s_avg_dev_temp_slow = s_avg_dev_temp_slow * (1.0f - ANOMALY_ALPHA_SLOW) + d * ANOMALY_ALPHA_SLOW;
        s_anchor_temp += (temp - s_anchor_temp) * ANCHOR_DRIFT_RATE;
        printf("temperature sensor: %.1f, anchor: %.1f, thresh: %.1f\n", temp, s_anchor_temp, thresh);
    }
    if (sensor_humidity) {
        float d = rh - s_anchor_rh;
        if (d < 0) d = -d;
        float avg_dev = fmaxf(s_avg_dev_rh_fast, s_avg_dev_rh_slow);
        float thresh = fmaxf(ANOMALY_K * avg_dev, ANOMALY_MIN_RH);
        if (d >= thresh){
          anomaly = true;
          s_anchor_rh    = rh;
        }
        printf("humidity sensor: %.1f, anchor: %.1f, thresh: %.1f\n", rh, s_anchor_rh, thresh);
        s_avg_dev_rh_fast = s_avg_dev_rh_fast * (1.0f - ANOMALY_ALPHA_FAST) + d * ANOMALY_ALPHA_FAST;
        s_avg_dev_rh_slow = s_avg_dev_rh_slow * (1.0f - ANOMALY_ALPHA_SLOW) + d * ANOMALY_ALPHA_SLOW;
        s_anchor_rh += (rh - s_anchor_rh) * ANCHOR_DRIFT_RATE;
    }
    if (sensor_sound) {
        float d = sound - s_anchor_sound;
        if (d < 0) d = -d;
        float avg_dev = fmaxf(s_avg_dev_sound_fast, s_avg_dev_sound_slow);
        float thresh = fmaxf(ANOMALY_K * avg_dev, ANOMALY_MIN_SOUND);
        if (d >= thresh){
          anomaly = true;
          s_anchor_sound = sound;
        }
        printf("sound sensor: %.1f, anchor: %.1f, thresh: %.1f\n", sound, s_anchor_sound, thresh);
        s_avg_dev_sound_fast = s_avg_dev_sound_fast * (1.0f - ANOMALY_ALPHA_FAST) + d * ANOMALY_ALPHA_FAST;
        s_avg_dev_sound_slow = s_avg_dev_sound_slow * (1.0f - ANOMALY_ALPHA_SLOW) + d * ANOMALY_ALPHA_SLOW;
        s_anchor_sound += (sound - s_anchor_sound) * ANCHOR_DRIFT_RATE;
    }
    for (int i = 0; i < NUM_GPIO_PINS; i++) {
        if (pin_sensor_type[i][0] != '\0') {
            float d = pin[i] - s_anchor_pin[i];
            if (d < 0) d = -d;
            float avg_dev = fmaxf(s_avg_dev_pin_fast[i], s_avg_dev_pin_slow[i]);
            float thresh = fmaxf(ANOMALY_K * avg_dev, anomaly_min_for_pin(i));
            printf("%s sensor: %.1f, anchor: %.1f, thresh: %.1f\n", pin_sensor_type[i], pin[i], s_anchor_pin[i], thresh);
            if (d >= thresh){
              anomaly = true;
              s_anchor_pin[i] = pin[i];
            }
            s_avg_dev_pin_fast[i] = s_avg_dev_pin_fast[i] * (1.0f - ANOMALY_ALPHA_FAST) + d * ANOMALY_ALPHA_FAST;
            s_avg_dev_pin_slow[i] = s_avg_dev_pin_slow[i] * (1.0f - ANOMALY_ALPHA_SLOW) + d * ANOMALY_ALPHA_SLOW;
            s_anchor_pin[i] += (pin[i] - s_anchor_pin[i]) * ANCHOR_DRIFT_RATE;
        }
    }

    // Fixed-anchor check for temperature & humidity (catches slow cumulative drift)
    if (s_fixed_anchor_initialized) {
        if (sensor_temperature) {
            float fd = temp - s_fixed_anchor_temp;
            if (fd < 0) fd = -fd;
            printf("temperature fixed anchor: %.1f, diff: %.1f, thresh: %.1f\n",
                   s_fixed_anchor_temp, fd, FIXED_THRESH_TEMP);
            if (fd >= FIXED_THRESH_TEMP) anomaly = true;
        }
        if (sensor_humidity) {
            float fd = rh - s_fixed_anchor_rh;
            if (fd < 0) fd = -fd;
            printf("humidity fixed anchor: %.1f, diff: %.1f, thresh: %.1f\n",
                   s_fixed_anchor_rh, fd, FIXED_THRESH_RH);
            if (fd >= FIXED_THRESH_RH) anomaly = true;
        }
    }

    // Suppress if still in cooldown (anchor drift above still ran)
    #ifndef DEBUG_SENSOR_HISTORY
      if (in_cooldown) anomaly = false;
    #endif

    printf("anomaly: %s\n", anomaly? "true":"false");

    #ifdef DEBUG_SENSOR_HISTORY
      if (anomaly) {
          printf("Anomaly detected\n");
      }
      if (in_cooldown) {
          printf("Anomaly cooldown: %lu ms remaining\n",
                 ANOMALY_COOLDOWN_MS - (now - s_last_anomaly_ms));
      }
    #endif

    // Update fixed anchors only when anomaly is actually detected (not suppressed)
    if (anomaly) {
        if (sensor_temperature) s_fixed_anchor_temp = temp;
        if (sensor_humidity)    s_fixed_anchor_rh   = rh;
    }

    return anomaly;
}

// Seed anchor values from the most recent history entry (called after load_sensor_history).
static void init_anomaly_anchors(void)
{
    if (s_hist_count == 0) return;
    int latest = (s_hist_pos - 1 + SENSOR_HIST_LEN) % SENSOR_HIST_LEN;
    s_anchor_temp  = s_hist[latest].temp;
    s_anchor_rh    = s_hist[latest].rh;
    s_anchor_sound = s_hist[latest].sound;
    for (int i = 0; i < NUM_GPIO_PINS; i++) s_anchor_pin[i] = s_hist[latest].pin[i];
    s_anchor_initialized = true;
    if (sensor_temperature) s_fixed_anchor_temp = s_hist[latest].temp;
    if (sensor_humidity)    s_fixed_anchor_rh   = s_hist[latest].rh;
    if (sensor_temperature || sensor_humidity) s_fixed_anchor_initialized = true;
    ESP_LOGI(TAG, "Anomaly anchors initialized — temp:%.1f rh:%.1f sound:%.1f",
             s_anchor_temp, s_anchor_rh, s_anchor_sound);
}

static SemaphoreHandle_t emotion_mutex = NULL;
static char current_emotion[32] = "curious";
static bool emotion_changed = false;  // set by set_emotion() when server returns new emotion
static int anim_frames_left = 0;      // remaining display refreshes (counts down each 2s cycle)
#define ANIM_TOTAL_FRAMES 10           // number of frames to animate after emotion change
// A full refresh on the 4-color panel takes several seconds, so it gets a
// single frame instead of an animation.
static int anim_total_frames(void) { return driver->fast_refresh() ? ANIM_TOTAL_FRAMES : 1; }

// ── Debug: upload history to server ─────────────────────────────────────────
// Comment out to disable WiFi upload (wifi_off() will then run after file sync,
// and ADC2 / GPIO19 will give valid readings).
// #define DEBUG_SENSOR_HISTORY

// ── JSON helpers ─────────────────────────────────────────────────────────────
// Parse "key":true or "key":false
static bool json_get_bool(const String &json, const char *key)
{
    int idx = json.indexOf(key);
    if (idx < 0) return false;
    int colon = json.indexOf(':', idx);
    if (colon < 0) return false;
    String val = json.substring(colon + 1);
    val.trim();
    return val.startsWith("true");
}

// Parse "key":"value" — returns empty String if key absent or value is ""
static String json_get_string(const String &json, const char *key)
{
    int idx = json.indexOf(key);
    if (idx < 0) return String();
    int colon = json.indexOf(':', idx);
    if (colon < 0) return String();
    int q1 = json.indexOf('"', colon + 1);
    if (q1 < 0) return String();
    int q2 = json.indexOf('"', q1 + 1);
    if (q2 <= q1) return String();
    return json.substring(q1 + 1, q2);
}

// ── Config loader ─────────────────────────────────────────────────────────────
static void load_sensor_config(void)
{
    if (!SPIFFS.exists("/sensors.json")) {
        ESP_LOGW(TAG, "/sensors.json not found — all sensors disabled");
        return;
    }
    File f = SPIFFS.open("/sensors.json", "r");
    if (!f) return;
    String content = f.readString();
    f.close();

    sensor_temperature = json_get_bool(content, "\"temperature\"");
    sensor_humidity    = json_get_bool(content, "\"humidity\"");
    sensor_sound       = json_get_bool(content, "\"sound\"");

    const char *pin_keys[NUM_GPIO_PINS] = {"\"pin1\"", "\"pin2\"", "\"pin3\"", "\"pin4\""};
    for (int i = 0; i < NUM_GPIO_PINS; i++) {
        String val = json_get_string(content, pin_keys[i]);
        val.toCharArray(pin_sensor_type[i], sizeof(pin_sensor_type[i]));
    }

    ESP_LOGI(TAG, "Sensor config — temp:%d humidity:%d sound:%d  pins:[%s][%s][%s][%s]",
             sensor_temperature, sensor_humidity, sensor_sound,
             pin_sensor_type[0], pin_sensor_type[1],
             pin_sensor_type[2], pin_sensor_type[3]);
}

// ── GPIO pin sensor init ──────────────────────────────────────────────────────
// For pins on ADC1 (pin1–3): adds the channel to the shared ADC1 unit.
// For pin4 (GPIO19, ADC2 — unavailable with WiFi): configures as digital input.
// Skips any pin whose sensor type is empty.
static void gpio_sensor_init(void)
{
    adc_bsp_init();  // idempotent — ensures adc1_handle exists
    for (int i = 0; i < NUM_GPIO_PINS; i++) {
        if (pin_sensor_type[i][0] == '\0') continue;
        if (PIN_ADC_UNIT[i] == 1) {
            adc_channel_add((adc_channel_t)PIN_ADC_CH[i]);
            ESP_LOGI(TAG, "ADC1_CH%d (GPIO%d) ready for sensor '%s'",
                     PIN_ADC_CH[i], PIN_GPIOS[i], pin_sensor_type[i]);
        } else if (PIN_ADC_UNIT[i] == 2) {
            adc2_bsp_init();  // idempotent — WiFi is already off at this point
            adc2_channel_add((adc_channel_t)PIN_ADC_CH[i]);
            ESP_LOGI(TAG, "ADC2_CH%d (GPIO%d) ready for sensor '%s'",
                     PIN_ADC_CH[i], PIN_GPIOS[i], pin_sensor_type[i]);
        } else {
            gpio_config_t cfg = {};
            cfg.pin_bit_mask = (1ULL << PIN_GPIOS[i]);
            cfg.mode         = GPIO_MODE_INPUT;
            cfg.pull_up_en   = GPIO_PULLUP_DISABLE;
            cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
            cfg.intr_type    = GPIO_INTR_DISABLE;
            gpio_config(&cfg);
            ESP_LOGI(TAG, "GPIO%d (digital) ready for sensor '%s'",
                     PIN_GPIOS[i], pin_sensor_type[i]);
        }
    }
}

// GY-61 (ADXL335) accelerometer: convert millivolt reading to cm/s².
// Single axis. Zero-g output = VCC/2 = 1.65 V, sensitivity = 300 mV/g.
// Returns acceleration in cm/s² (divide by 100 on server for m/s²).
static float voltage_to_accel(int mv)
{
    const float VCC       = 3.3f;
    const float ZERO_G_V  = VCC / 2.0f;   // 1.65 V at 0 g
    const float SENS_V_G  = 0.330f;        // 330 mV/g at 3.3 V (ADXL335)
    const float G_TO_MS2  = 9.80665f;

    float v = mv / 1000.0f;
    float g_force = (v - ZERO_G_V) / SENS_V_G;
    float accel   = g_force * G_TO_MS2;
    return accel * 100.0f;  // cm/s² (divide by 100 on server for m/s²)
}

// Sliding-window motion detector for accelerometer.
// Maintains a moving average of recent readings (gravity baseline) and returns
// the absolute deviation from that average — i.e. only dynamic acceleration
// caused by motion (picking up, shaking, etc.). Gravity is filtered out.
// Returns motion magnitude in cm/s² (0 = stationary).
#define ACCEL_WINDOW_SIZE 1
static float  s_accel_window[ACCEL_WINDOW_SIZE];
static int    s_accel_win_count = 0;
static int    s_accel_win_pos   = 0;
static float  s_accel_win_sum   = 0.0f;

static float accel_motion(float accel_cm_s2)
{
    // Fill the window first — report 0 until we have enough baseline samples
    if (s_accel_win_count < ACCEL_WINDOW_SIZE) {
        s_accel_window[s_accel_win_count] = accel_cm_s2;
        s_accel_win_sum += accel_cm_s2;
        s_accel_win_count++;
        return 0.0f;
    }

    // Compute moving average (gravity baseline)
    float avg = s_accel_win_sum / ACCEL_WINDOW_SIZE;
    float delta = accel_cm_s2 - avg;
    if (delta < 0) delta = -delta;

    // Update ring buffer: subtract old, add new
    s_accel_win_sum -= s_accel_window[s_accel_win_pos];
    s_accel_window[s_accel_win_pos] = accel_cm_s2;
    s_accel_win_sum += accel_cm_s2;
    s_accel_win_pos = (s_accel_win_pos + 1) % ACCEL_WINDOW_SIZE;

    // return delta;  // cm/s² — 0 when still, spikes on motion
    // clip motion to make it percentange
    printf("motion before clip %f\n",delta);
    if (delta >= 30.0f) return 10.001f;
    else return 0.0f;
}

// GM5539 photoresistor: convert millivolt reading to lux.
// Assumes voltage divider with R_FIXED pulldown, LDR on high/Users/daehwakim/Desktop/research/TinyMind/oat-firmware/src/audio/audio_bsp.h /Users/daehwakim/Desktop/research/TinyMind/oat-firmware/src/audio/audio_bsp.c side (LDR→VCC, R→GND).
// Datasheet link: https://www.micros.com.pl/mediaserver/info-orrgl5539.pdf
static float voltage_to_lux(int mv)
{
    const float R_FIXED = 10000.0f;  // fixed resistor (Ω)
    const float VCC     = 3.3f;
    const float C       = 473219.0f; // calibration constant (75kΩ@10lux, γ=0.8)
    const float INV_GAMMA = 1.25f;   // 1/γ where γ=0.8 (from datasheet)

    float v = mv / 1000.0f;
    if (v < 0.01f) return 0.0f;
    float r_ldr = R_FIXED * (VCC / v - 1.0f);
    if (r_ldr <= 0.0f) return 0.0f;
    return powf(C / r_ldr, INV_GAMMA);
}

static float clip_occupancy(int mv){
  if (mv > 2000) return 3000.0f;
  else return 0.0f;
}

static void update_emotion_display(void);      // forward declaration
static void charging_mode(void);               // forward declaration
static void charging_task(void *arg);          // forward declaration

static void upload_sensor_history(void); // forward declaration

// Server unreachable: show the neutral face with the offline banner
// (drawn by update_emotion_display). Forces a redraw even if the face is
// already the base one, so the banner appears.
#define NO_INTERNET_FACE "base"
static void set_emotion_offline(void)
{
    s_no_internet = true;
    set_emotion(NO_INTERNET_FACE);
    xSemaphoreTake(emotion_mutex, portMAX_DELAY);
    emotion_changed = true;
    xSemaphoreGive(emotion_mutex);
}

static void request_emotion_from_server(void)
{
    const char *short_id = wifi_get_dev_id() + 6;
    int code = 0;
    String body;

    String url = String(OATS_SERVER) + "/get_emotion?device=" + short_id;
    for (int attempt = 1; attempt <= 5; ++attempt) {
        HTTPClient http;
        http.setTimeout(120000);
        http.begin(url);

        Serial.printf("[emotion] attempt %d: before GET\n", attempt);
        code = http.GET();
        Serial.printf("[emotion] attempt %d: HTTP %d\n", attempt, code);

        if (code == 200) {
            body = http.getString();
            http.end();
            break;
        }

        http.end();

        // Retry only on timeout
        if (code == HTTPC_ERROR_READ_TIMEOUT && attempt < 2) {
            Serial.println("[emotion] timeout, retrying...");
            delay(500);  // small pause before retry
            continue;
        }

        // Any non-retryable failure, or second timeout
        Serial.println("[emotion] request failed");
        set_emotion_offline();
        return;
    }

    if (code != 200) {
        set_emotion_offline();
        return;
    }

    // Parse "emotion" field from {"emotion":"...","reason":"..."}
    char emotion_buf[32] = "";
    int idx = body.indexOf("\"emotion\":");
    if (idx >= 0) {
        int q1 = body.indexOf('"', idx + 10);
        int q2 = body.indexOf('"', q1 + 1);
        if (q1 >= 0 && q2 > q1)
            body.substring(q1 + 1, q2).toCharArray(emotion_buf, sizeof(emotion_buf));
    }
    String reason;
    idx = body.indexOf("\"reason\":");
    if (idx >= 0) {
        int q1 = body.indexOf('"', idx + 9);
        int q2 = body.indexOf('"', q1 + 1);
        if (q1 >= 0 && q2 > q1)
            reason = body.substring(q1 + 1, q2);
    }

    Serial.printf("[emotion] %s — %s\n", emotion_buf, reason.c_str());
    if (emotion_buf[0] != '\0') {
        if (s_no_internet) {                 // back online: drop the banner even if the face is unchanged
            s_no_internet = false;
            xSemaphoreTake(emotion_mutex, portMAX_DELAY);
            emotion_changed = true;
            xSemaphoreGive(emotion_mutex);
        }
        set_emotion(emotion_buf);
    }
}

#ifdef DEBUG_SENSOR_HISTORY
static void serial_debug_task(void *arg)
{
    String input = "";
    Serial.println("[debug] Commands: 'send' = upload history,  'emotion' = upload + request emotion");
    for (;;) {
        while (Serial.available()) {
            char c = (char)Serial.read();
            if (c == '\n' || c == '\r') {
                input.trim();
                if (input == "send") {
                    Serial.println("[debug] Connecting WiFi...");
                    wifi_on();
                    battery_sample();
                    Serial.println("[debug] Uploading sensor history...");
                    upload_sensor_history();
                    wifi_off();
                    Serial.println("[debug] Done. WiFi off.");
                } else if (input == "emotion") {
                    Serial.println("[debug] Connecting WiFi...");
                    wifi_on();
                    battery_sample();
                    Serial.println("[debug] Uploading sensor history...");
                    upload_sensor_history();
                    Serial.println("[debug] Requesting emotion from server...");
                    request_emotion_from_server();
                    wifi_off();
                    Serial.println("[debug] Done. WiFi off.");
                }
                input = "";
            } else {
                input += c;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
#endif

// Sanitize a float for JSON: replace NaN/Inf with 0.
static float safe_float(float v) { return (isnan(v) || isinf(v)) ? 0.0f : v; }

static void upload_sensor_history(void)
{
    const char *short_id = wifi_get_dev_id() + 6;
    int oldest = (s_hist_count < SENSOR_HIST_LEN) ? 0 : s_hist_pos;

    // Worst case: 7 sensors × 336 entries × ~8 chars each ≈ 19KB + overhead
    String json;
    json.reserve(24000);
    json = "{\"device\":\"";
    json += short_id;
    json += "\",\"sensors\":{";

    bool first_sensor = true;
    char buf[32];

    // Temperature
    if (sensor_temperature) {
        json += "\"temperature\":{\"current value\":";
        snprintf(buf, sizeof(buf), "%.1f", safe_float(s_cur_temp));
        json += buf;
        json += ",\"history\":[";
        for (int j = 0; j < s_hist_count; j++) {
            const sensor_hist_t &h = s_hist[(oldest + j) % SENSOR_HIST_LEN];
            if (j > 0) json += ",";
            snprintf(buf, sizeof(buf), "%.1f", safe_float(h.temp));
            json += buf;
        }
        json += "]}";
        first_sensor = false;
    }

    // Humidity
    if (sensor_humidity) {
        if (!first_sensor) json += ",";
        json += "\"humidity\":{\"current value\":";
        snprintf(buf, sizeof(buf), "%.1f", safe_float(s_cur_rh));
        json += buf;
        json += ",\"history\":[";
        for (int j = 0; j < s_hist_count; j++) {
            const sensor_hist_t &h = s_hist[(oldest + j) % SENSOR_HIST_LEN];
            if (j > 0) json += ",";
            snprintf(buf, sizeof(buf), "%.1f", safe_float(h.rh));
            json += buf;
        }
        json += "]}";
        first_sensor = false;
    }

    // Sound
    if (sensor_sound) {
        if (!first_sensor) json += ",";
        json += "\"sound\":{\"current value\":";
        snprintf(buf, sizeof(buf), "%.1f", safe_float(s_cur_sound));
        json += buf;
        json += ",\"history\":[";
        for (int j = 0; j < s_hist_count; j++) {
            const sensor_hist_t &h = s_hist[(oldest + j) % SENSOR_HIST_LEN];
            if (j > 0) json += ",";
            snprintf(buf, sizeof(buf), "%.1f", safe_float(h.sound));
            json += buf;
        }
        json += "]}";
        first_sensor = false;
    }

    // GPIO pin sensors
    for (int i = 0; i < NUM_GPIO_PINS; i++) {
        if (pin_sensor_type[i][0] == '\0') continue;
        if (!first_sensor) json += ",";
        json += "\"";
        json += pin_sensor_type[i];
        json += "\":{\"current value\":";
        snprintf(buf, sizeof(buf), "%.1f", safe_float(s_cur_pin[i]));
        json += buf;
        json += ",\"history\":[";
        for (int j = 0; j < s_hist_count; j++) {
            const sensor_hist_t &h = s_hist[(oldest + j) % SENSOR_HIST_LEN];
            if (j > 0) json += ",";
            snprintf(buf, sizeof(buf), "%.1f", safe_float(h.pin[i]));
            json += buf;
        }
        json += "]}";
        first_sensor = false;
    }

    json += "}";   // close "sensors"

    // Battery goes beside "sensors", not inside it, so the server does not
    // treat it as an environment sensor during inference.
    // "percent" is the shown level (charging estimate while "usb" is true,
    // else the last on-battery reading); absent until one exists.
    if (s_batt_mv >= 0) {
        char bbuf[96];
        if (battery_shown_pct() >= 0)
            snprintf(bbuf, sizeof(bbuf), ",\"battery\":{\"mv\":%d,\"percent\":%d,\"usb\":%s}",
                     s_batt_mv, battery_shown_pct(), s_charging ? "true" : "false");
        else
            snprintf(bbuf, sizeof(bbuf), ",\"battery\":{\"mv\":%d,\"usb\":%s}",
                     s_batt_mv, s_charging ? "true" : "false");
        json += bbuf;
    }
    json += "}";   // close root

    HTTPClient hist_http;
    hist_http.setTimeout(5000);
    hist_http.begin(String(OATS_SERVER) + "/sensor_history");
    hist_http.addHeader("Content-Type", "application/json");
    int hist_code = hist_http.POST(json);
    if (hist_code != 200)
        ESP_LOGW(TAG, "sensor_history POST: %d", hist_code);
    hist_http.end();
}

// ── Sensor history persistence (SPIFFS) ──────────────────────────────────────
static void save_sensor_history(void)
{
    File f = SPIFFS.open(HIST_BIN_PATH, "w");
    if (!f) { ESP_LOGW(TAG, "save_sensor_history: cannot open " HIST_BIN_PATH); return; }
    f.write((uint8_t *)&s_hist_count, sizeof(s_hist_count));
    f.write((uint8_t *)&s_hist_pos,   sizeof(s_hist_pos));
    f.write((uint8_t *)s_hist,        sizeof(s_hist));
    f.close();
    ESP_LOGI(TAG, "Saved %d history samples to SPIFFS", s_hist_count);
}

static void load_sensor_history(void)
{
    if (!SPIFFS.exists(HIST_BIN_PATH)) {
        ESP_LOGI(TAG, "load_sensor_history: no saved history");
        return;
    }
    File f = SPIFFS.open(HIST_BIN_PATH, "r");
    if (!f) { ESP_LOGW(TAG, "load_sensor_history: cannot open " HIST_BIN_PATH); return; }
    f.read((uint8_t *)&s_hist_count, sizeof(s_hist_count));
    f.read((uint8_t *)&s_hist_pos,   sizeof(s_hist_pos));
    f.read((uint8_t *)s_hist,        sizeof(s_hist));
    f.close();

    // Sanity-clamp in case file is corrupt
    if (s_hist_count < 0 || s_hist_count > SENSOR_HIST_LEN) { s_hist_count = 0; return; }
    if (s_hist_pos   < 0 || s_hist_pos   >= SENSOR_HIST_LEN)  s_hist_pos   = 0;

    // Restore current readings from the most recent entry
    int latest_idx = (s_hist_pos - 1 + SENSOR_HIST_LEN) % SENSOR_HIST_LEN;
    s_cur_temp  = s_hist[latest_idx].temp;
    s_cur_rh    = s_hist[latest_idx].rh;
    s_cur_sound = s_hist[latest_idx].sound;
    for (int i = 0; i < NUM_GPIO_PINS; i++) s_cur_pin[i] = s_hist[latest_idx].pin[i];

    s_hist_initialized  = true;
    s_last_hist_push_ms = millis();  // schedule next push SENSOR_HIST_INTERVAL_MS from now
    ESP_LOGI(TAG, "Loaded %d history samples from SPIFFS", s_hist_count);
}

// Peak-to-peak amplitude of 16-bit PCM buffer (0–65535).
static int amplitude_peak_to_peak(const int16_t *samples, int num_samples)
{
    int16_t vmin = samples[0], vmax = samples[0];
    for (int i = 1; i < num_samples; i++) {
        if (samples[i] < vmin) vmin = samples[i];
        if (samples[i] > vmax) vmax = samples[i];
    }
    return vmax - vmin;
}

// Convert 16-bit PCM buffer to dBFS (decibels relative to full scale).
// 0 dBFS = max amplitude (32768), silence → -96 dBFS.
static float amplitude_to_dB(const int16_t *samples, int num_samples)
{
    // Stereo interleaved: use only left channel (every other sample)
    float sum_sq = 0.0f;
    int count = 0;
    for (int i = 0; i < num_samples; i += 2) {
        float s = (float)samples[i];
        sum_sq += s * s;
        count++;
    }
    float rms = sqrtf(sum_sq / count);
    if (rms < 1.0f) return -96.0f;
    return 20.0f * log10f(rms / 32768.0f);
}

extern board_power_bsp_t board_div;

// Set by the sensor task while it is parked in its shutdown idle loop, so the
// shutdown path knows no e-ink update is in flight.
static volatile bool s_sensor_idle = false;
static TaskHandle_t  s_sensor_task_handle = NULL;

// The sensor task's e-ink power-off, suppressed once shutdown has begun so it
// cannot cut the panel's power underneath the sleep screen.
static inline void epd_power_off_unless_shutting_down(void)
{
    if (!s_shutting_down) board_div.POWEER_EPD_OFF();
}

// ── Sensor read task ─────────────────────────────────────────────────────────
// Reads all enabled sensors every 2 s and logs the values.
// (Emotion mapping will be added here later.)
static void sensor_read_task(void *arg)
{
    // Microphone audio buffer: 2048 samples × 2 channels × 2 bytes = 8192 bytes
    const uint32_t MIC_BUF_BYTES = 8192;
    int16_t *mic_buf = NULL;
    if (sensor_sound) {
        mic_buf = (int16_t *)heap_caps_malloc(MIC_BUF_BYTES, MALLOC_CAP_SPIRAM);
    }

    // Warmup: discard the first few sensor readings to avoid garbage values.
    // Sensors (SHTC3, ADC, microphone) need several cycles after power-on to settle.
    const int WARMUP_CYCLES = 3;
    for (int w = 0; w < WARMUP_CYCLES; w++) {
        #ifndef DEBUG_SENSOR_HISTORY
        gpio_set_level(SENSOR_PWR_PIN, 1);
        #endif
        vTaskDelay(pdMS_TO_TICKS(100));  // extra stabilization time during warmup

        if (sensor_temperature || sensor_humidity) {
            shtc3_data_t dummy = {};
            shtc3_read(&dummy);
        }
        for (int i = 0; i < NUM_GPIO_PINS; i++) {
            if (pin_sensor_type[i][0] == '\0') continue;
            int raw = 0;
            if (PIN_ADC_UNIT[i] == 1)
                adc_channel_read_mv((adc_channel_t)PIN_ADC_CH[i], &raw);
            else if (PIN_ADC_UNIT[i] == 2)
                adc2_channel_read_mv((adc_channel_t)PIN_ADC_CH[i], &raw);
        }
        if (sensor_sound && mic_buf != NULL) {
            audio_bsp_read(mic_buf, MIC_BUF_BYTES);
        }

        gpio_set_level(SENSOR_PWR_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    ESP_LOGI(TAG, "Sensor warmup complete (%d cycles discarded)", WARMUP_CYCLES);

    int light_sleep_duration = 2 * 1000 * 1000;
    for (;;) {
        // Shutting down, or "off" while USB keeps us alive: stop reading,
        // uploading and light-sleeping; just idle.
        if (s_shutting_down) { s_sensor_idle = true; vTaskDelay(pdMS_TO_TICKS(500)); continue; }
        s_sensor_idle = false;

        float hist_temp = -1.0f, hist_rh = -1.0f;
        float hist_sound = -1.0f;
        float hist_pin[NUM_GPIO_PINS] = {-1.0f, -1.0f, -1.0f, -1.0f};

        // Power ON sensor rail for SHTC3 + GPIO pin sensors
        gpio_set_level(SENSOR_PWR_PIN, 1);           // active-HIGH: 1 = ON
        if (sensor_sound) board_div.POWEER_Audio_ON();
        vTaskDelay(pdMS_TO_TICKS(50));               // let sensors stabilize

        if (sensor_temperature || sensor_humidity) {
            shtc3_data_t data = {};
            if (shtc3_read(&data)) {
                if (sensor_temperature) {
                    #ifndef DEBUG_SENSOR_HISTORY
                      printf("temperature: %.1f C\n", data.temp);
                    #endif
                    hist_temp = data.temp;
                }
                if (sensor_humidity) {
                    #ifndef DEBUG_SENSOR_HISTORY
                      printf("humidity:    %.1f %%\n", data.rh);
                    #endif
                    hist_rh = data.rh;
                }
            }
        }

        for (int i = 0; i < NUM_GPIO_PINS; i++) {
            if (pin_sensor_type[i][0] == '\0') continue;

            int mv = 0;
            bool is_motion = (strcmp(pin_sensor_type[i], "motion") == 0);

            if (is_motion) {
                // Motion sensor: sample 5 times, take abs of each, then average
                const int N_SAMPLES = 10;
                float abs_sum = 0.0f;
                for (int s = 0; s < N_SAMPLES; s++) {
                    int sample_mv = 0;
                    if (PIN_ADC_UNIT[i] == 1) {
                        int raw = 0;
                        sample_mv = adc_channel_read_mv((adc_channel_t)PIN_ADC_CH[i], &raw);
                    } else if (PIN_ADC_UNIT[i] == 2) {
                        int raw = 0;
                        sample_mv = adc2_channel_read_mv((adc_channel_t)PIN_ADC_CH[i], &raw);
                    }
                    float raw_accel = voltage_to_accel(sample_mv);
                    float abs_accel = (raw_accel < 0) ? -raw_accel : raw_accel;
                    abs_sum += abs_accel;
                    if (s < N_SAMPLES - 1) vTaskDelay(pdMS_TO_TICKS(3));
                }
                float mean_abs_accel = abs_sum / N_SAMPLES;
                hist_pin[i] = accel_motion(mean_abs_accel);
            } else if (PIN_ADC_UNIT[i] == 1) {
                int raw = 0;
                mv = adc_channel_read_mv((adc_channel_t)PIN_ADC_CH[i], &raw);
            } else if (PIN_ADC_UNIT[i] == 2) {
                int raw = 0;
                mv = adc2_channel_read_mv((adc_channel_t)PIN_ADC_CH[i], &raw);
            } else {
                hist_pin[i] = (float)gpio_get_level(PIN_GPIOS[i]);
                continue;
            }

            if (is_motion) {
                // already handled above
            } else if (strcmp(pin_sensor_type[i], "light") == 0) {
                hist_pin[i] = voltage_to_lux(mv);
            } else if (strcmp(pin_sensor_type[i], "occupancy") == 0){
              hist_pin[i] = clip_occupancy(mv);
            } else {
                hist_pin[i] = (float)mv;
            }

            #ifdef DEBUG_SENSOR_HISTORY
              printf("%s: %.1f\n",pin_sensor_type[i], hist_pin[i]);
            #endif
        }

        if (sensor_sound && mic_buf != NULL) {
            audio_bsp_reopen();
            audio_bsp_read(mic_buf, MIC_BUF_BYTES);  // discard first full buffer (power-on transient)
            audio_bsp_read(mic_buf, MIC_BUF_BYTES);  // discard first full buffer (power-on transient)
            if (audio_bsp_read(mic_buf, MIC_BUF_BYTES)) {
                int num_samples = MIC_BUF_BYTES / sizeof(int16_t);
                // hist_sound = amplitude_peak_to_peak(mic_buf, num_samples);
                hist_sound = amplitude_to_dB(mic_buf, num_samples);
                #ifdef DEBUG_SENSOR_HISTORY
                  printf("sound: %.1f dB\n",hist_sound);
                #endif
            }
        }



        // Power OFF sensor rail (sound uses a separate audio power pin, unaffected)
        #ifndef DEBUG_SENSOR_HISTORY
        gpio_set_level(SENSOR_PWR_PIN, 0);
        #endif

        // Always update current readings
        s_cur_temp  = hist_temp;
        s_cur_rh    = hist_rh;
        s_cur_sound = hist_sound;
        for (int i = 0; i < NUM_GPIO_PINS; i++) s_cur_pin[i] = hist_pin[i];

        accumulate_sensor_reading(hist_temp, hist_rh, hist_sound, hist_pin);

        // Seed anomaly anchors from the first live reading if no history was loaded
        if (!s_anchor_initialized) {
            s_anchor_temp  = hist_temp;
            s_anchor_rh    = hist_rh;
            s_anchor_sound = hist_sound;
            for (int i = 0; i < NUM_GPIO_PINS; i++) s_anchor_pin[i] = hist_pin[i];
            s_anchor_initialized = true;
        }
        if (!s_fixed_anchor_initialized && (sensor_temperature || sensor_humidity)) {
            if (sensor_temperature) s_fixed_anchor_temp = hist_temp;
            if (sensor_humidity)    s_fixed_anchor_rh   = hist_rh;
            s_fixed_anchor_initialized = true;
        }

        // Check for anomaly (large deviation from anchored reference)
        bool is_anomaly = check_anomaly(hist_temp, hist_rh, hist_sound, hist_pin);

        // Push averaged sample per hour into the history ring buffer
        uint32_t now_ms = millis();
        bool need_upload = !s_hist_initialized ||
            (now_ms - s_last_hist_push_ms) >= SENSOR_HIST_INTERVAL_MS;
        if (need_upload) {
            if (!s_hist_initialized && need_initial_emotion)
              push_averaged_history(now_ms, hist_temp, hist_rh, hist_sound, hist_pin);
            else if (!need_initial_emotion)
              push_averaged_history(now_ms, hist_temp, hist_rh, hist_sound, hist_pin);
            save_sensor_history();
            s_last_hist_push_ms = now_ms;
            s_hist_initialized  = true;
        }

        // Show the stored emotion right away while the first upload runs.
        // Skipped on the slow (4-color) panel: every refresh there is a
        // ~15 s flash, so wait for the real answer instead.
        if (need_initial_emotion && driver->fast_refresh()) {
            board_div.POWEER_EPD_ON();
            update_emotion_display();
            epd_power_off_unless_shutting_down();
        }

        #ifndef DEBUG_SENSOR_HISTORY
          // Request emotion on hourly upload OR anomaly
          if (need_upload || is_anomaly || need_initial_emotion) {
              bool was_initial = need_initial_emotion;
              // Battery is read only here, right before talking to the server.
              // The sensor rail is already off, so this is close to the
              // resting cell voltage.
              battery_sample();

              // If inference is needed but the hour hasn't elapsed, update the
              // most recent history entry in-place with current accumulated data
              // so the server sees fresh values without adding a new entry.
              if (!need_upload && s_hist_count > 0) {
                  update_latest_history(now_ms, hist_temp, hist_rh, hist_sound, hist_pin);
                  save_sensor_history();
              }
              // "Thinking" face while we talk to the server. Skipped on the
              // slow panel (it would be one 15 s flash followed by another).
              if (driver->fast_refresh()) {
                  set_emotion("curious");
                  board_div.POWEER_EPD_ON();
                  update_emotion_display();
              }
              wifi_on();
              upload_sensor_history();
              request_emotion_from_server();
              wifi_off();
              need_initial_emotion = false;
              s_last_anomaly_ms = millis();

              // Start animation if the emotion actually changed
              if (emotion_changed) {
                  emotion_changed = false;
                  anim_frames_left = anim_total_frames();
              } else if (!driver->fast_refresh() && (was_initial || need_upload)) {
                  // Slow panel skipped the interim screens above: draw the
                  // result once (this also refreshes the battery icon).
                  // Anomaly cycles with an unchanged emotion leave it alone.
                  anim_frames_left = 1;
              }
              if (anim_frames_left <= 0) epd_power_off_unless_shutting_down();
          }

          // Animate: show next matching image each 2s cycle, then stop
          if (anim_frames_left > 0) {
              board_div.POWEER_EPD_ON();
              update_emotion_display();
              anim_frames_left--;
              light_sleep_duration = 1 * 1000 * 1000;
              if (anim_frames_left <= 0){
                epd_power_off_unless_shutting_down();
                light_sleep_duration = 2 * 1000 * 1000;
              }
          }
          else {
            // Refresh emotion animation every 5 minutes to keep display lively
            // (fast-refresh panels only: on the 4-color panel every update is
            // a multi-second full flash, so leave the image alone)
            static uint32_t s_last_anim_refresh_ms = 0;
            uint32_t anim_now = millis();
            if (s_last_anim_refresh_ms == 0) s_last_anim_refresh_ms = anim_now;
            if (driver->fast_refresh() &&
                (anim_now - s_last_anim_refresh_ms) >= 5UL * 60UL * 1000UL) {
                s_last_anim_refresh_ms = anim_now;
                anim_frames_left = anim_total_frames();
            }
          }

          // Keep the gauge fresh: one quiet sample a minute (sensor rail and
          // WiFi are off here), on top of the one taken before each upload.
          if (millis() - s_batt_last_ms >= BATTERY_SAMPLE_MS) battery_sample();
          // A PC on the USB port is visible at once (SOF frames), so let the
          // sleep-on-charge rule see it now instead of at the next minute.
          // Chargers and power banks only show in the voltage: up to a minute.
          else if (!s_charging && usb_host_present()) battery_sample();

          // Power off audio codec + gate I2S clocks before sleep
          if (sensor_sound) {
              audio_bsp_close();
              board_div.POWEER_Audio_OFF();
          }

          // Skip light sleep if shutdown is in progress — light sleep pauses
          // ALL tasks (including button_power_task), which would block the
          // shutdown sequence from completing.
          if (s_shutting_down) {
              vTaskDelay(pdMS_TO_TICKS(100));
              continue;
          }

          // Light sleep for 2 seconds to save power (~240 µA vs ~20-30 mA active idle)
          esp_sleep_enable_timer_wakeup(light_sleep_duration);  // 2 seconds in µs
          esp_sleep_enable_ext1_wakeup(
              (1ULL << BOOT_BUTTON_PIN) | (1ULL << PWR_BUTTON_PIN),
              ESP_EXT1_WAKEUP_ANY_LOW
          );
          esp_light_sleep_start();

          // If woken by button press (EXT1), stay awake long enough for the
          // button state machine to detect a long press (needs ~1000 ms of
          // active esp_timer ticks). Without this delay the loop would
          // immediately start the next sensor cycle and re-enter light sleep
          // before the long-press threshold is reached.
          if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1) {
              vTaskDelay(pdMS_TO_TICKS(1200));
          }
        #endif
        #ifdef DEBUG_SENSOR_HISTORY
          vTaskDelay(pdMS_TO_TICKS(2000));
        #endif
    }
}
#define IMG_WIDTH    200
#define IMG_HEIGHT   200
#define IMG_STRIDE   ((IMG_WIDTH + 7) / 8)   // 25 bytes per row
#define IMG_HEADER   8
#define IMG_BUF_SIZE (IMG_HEADER + IMG_STRIDE * IMG_HEIGHT)  // 5008 bytes
#define MAX_IMAGES   128

epaper_driver_display *driver = NULL;
board_power_bsp_t board_div(EPD_PWR_PIN,Audio_PWR_PIN,VBAT_PWR_PIN);

lv_ui src_ui;

// PSRAM buffer for one image at a time (1-bit bitmap + 8-byte header)
static uint8_t *img_buf = NULL;

// File list scanned from SPIFFS /images/
static char img_filenames[MAX_IMAGES][64];
static int img_count = 0;

// Current emotion filter (empty string = show all)

// Battery power latch flag
static bool is_vabtflag = false;

// (moved earlier — before sensor_read_task)

// Handle for the image loop task so button task can suspend it on shutdown
static TaskHandle_t loop_img_task_handle = NULL;

// 1 = evaluate local /emotion_logic.txt from SPIFFS
// 2 = request emotion string from server each cycle
int emotion_method = 1;

// Remove every file in SPIFFS and reset the image count.
// Call this to force a full re-download on the next sync.
static void clear_spiffs(void)
{
  File root = SPIFFS.open("/");
  File file = root.openNextFile();
  while (file) {
    String path = String("/") + file.name();
    file.close();
    SPIFFS.remove(path);
    printf("Removed %s\n", path.c_str());
    file = root.openNextFile();
  }
  img_count = 0;
  printf("SPIFFS cleared.\n");
}

static int scan_spiffs_images(void)
{
  img_count = 0;
  File root = SPIFFS.open("/");
  File file = root.openNextFile();
  while (file && img_count < MAX_IMAGES) {
    String name = String(file.name());
    if (name.endsWith(".bin")) {
      strncpy(img_filenames[img_count], name.c_str(), sizeof(img_filenames[0]) - 1);
      img_filenames[img_count][sizeof(img_filenames[0]) - 1] = '\0';
      img_count++;
    }
    file = root.openNextFile();
  }
  printf("Found %d images in SPIFFS\n", img_count);
  return img_count;
}

static bool load_image_from_spiffs(const char *path)
{
  // SPIFFS requires a leading '/' — file.name() omits it
  String fullpath = (path[0] == '/') ? String(path) : String("/") + path;
  File f = SPIFFS.open(fullpath, "r");
  if (!f) {
    printf("Failed to open %s\n", fullpath.c_str());
    return false;
  }
  size_t read_bytes = f.read(img_buf, IMG_BUF_SIZE);
  f.close();
  return read_bytes >= IMG_HEADER;
}

// Draw the loaded 1-bit bitmap directly to the e-ink display.
// Image pixel data uses the same format as the driver buffer (1-bit, MSB-first,
// 25 bytes/row), so we bulk-copy instead of setting 40k pixels individually.
static void display_bitmap(void)
{
  uint16_t w = (img_buf[0] << 8) | img_buf[1];
  uint16_t h = (img_buf[2] << 8) | img_buf[3];
  if (w == 0 || h == 0 || w > IMG_WIDTH || h > IMG_HEIGHT) {
    return;
  }
  const uint8_t *px = img_buf + IMG_HEADER;
  uint16_t stride = (w + 7) / 8;

  driver->EPD_SetBuffer(px, stride * h);
  driver->EPD_DisplayPart();
}


// Set one pixel in a 1-bit image that uses the panel's buffer format:
// MSB-first, bit 1 = white, bit 0 = black.
static inline void bmp_put(uint8_t *px, uint16_t stride, int x, int y, bool black)
{
    uint8_t *b = &px[y * stride + (x >> 3)];
    uint8_t  m = 0x80 >> (x & 7);
    if (black) *b &= ~m; else *b |= m;
}

// Draw a small battery gauge into the loaded image (top-right corner).
static void draw_battery_icon(void)
{
    const int pct = battery_shown_pct();
    if (pct < 0 && !s_charging) return;           // nothing known yet
    uint16_t w = (img_buf[0] << 8) | img_buf[1];
    uint16_t h = (img_buf[2] << 8) | img_buf[3];
    if (w < 32 || h < 16 || w > IMG_WIDTH || h > IMG_HEIGHT) return;
    uint8_t *px = img_buf + IMG_HEADER;
    uint16_t stride = (w + 7) / 8;

    const int BW = 18, BH = 8;            // body size in pixels
    const int x0 = w - 4 - BW - 2;        // 4 px margin, room for the 2 px nub
    const int y0 = 4;

    // White halo so the gauge stays readable on dark artwork
    for (int y = y0 - 1; y <= y0 + BH; y++)
        for (int x = x0 - 1; x <= x0 + BW + 2; x++)
            bmp_put(px, stride, x, y, false);
    // Body outline
    for (int x = x0; x < x0 + BW; x++) {
        bmp_put(px, stride, x, y0, true);
        bmp_put(px, stride, x, y0 + BH - 1, true);
    }
    for (int y = y0; y < y0 + BH; y++) {
        bmp_put(px, stride, x0, y, true);
        bmp_put(px, stride, x0 + BW - 1, y, true);
    }
    // Nub (positive terminal)
    for (int y = y0 + 2; y < y0 + BH - 2; y++) {
        bmp_put(px, stride, x0 + BW,     y, true);
        bmp_put(px, stride, x0 + BW + 1, y, true);
    }
    // Fill proportional to charge; interior is (BW - 4) px wide
    int fill = pct < 0 ? 0 : ((BW - 4) * pct + 50) / 100;   // charging, not finished: bolt only
    for (int y = y0 + 2; y < y0 + BH - 2; y++)
        for (int x = x0 + 2; x < x0 + 2 + fill; x++)
            bmp_put(px, stride, x, y, true);
    // Charging: a lightning bolt through the middle, inverted against the fill.
    if (s_charging) {
        static const int8_t bolt[8][2] = {{3,0},{4,0},{2,1},{3,1},{2,2},{3,2},{1,3},{2,3}};
        const int bx = x0 + 2 + (BW - 4) / 2 - 3, by = y0 + 2;
        for (int i = 0; i < 8; i++) {
            int x = bx + bolt[i][0], y = by + bolt[i][1];
            bmp_put(px, stride, x, y, !(x < x0 + 2 + fill));
        }
    }
}

// Update e-ink display with the next emotion-matching image
// Offline banner: "Bad Internet Connection" on a white box, centred
// horizontally, two thirds of the way down the face. The face is drawn
// straight into the 1-bit panel buffer, so the text is rasterised here from
// LVGL's Montserrat glyph data (4 bpp, thresholded to black/white).
#define NO_INTERNET_TEXT   "Bad Internet Connection"
#define NO_INTERNET_Y_NUM  2      // banner centre at height * 2/3
#define NO_INTERNET_Y_DEN  3
#define NO_INTERNET_PAD    4      // white margin around the text, px

static int glyph_text_width(const lv_font_t *font, const char *text)
{
    int w = 0;
    lv_font_glyph_dsc_t g;
    for (const char *c = text; *c; c++)
        if (lv_font_get_glyph_dsc(font, &g, (uint32_t)*c, (uint32_t)c[1])) w += g.adv_w;
    return w;
}

static void draw_glyph_text(uint8_t *px, uint16_t stride, int w, int h,
                            const lv_font_t *font, const char *text, int x, int baseline)
{
    lv_font_glyph_dsc_t g;
    for (const char *c = text; *c; c++) {
        if (!lv_font_get_glyph_dsc(font, &g, (uint32_t)*c, (uint32_t)c[1])) continue;
        const uint8_t *bmp = lv_font_get_glyph_bitmap(font, (uint32_t)*c);
        if (bmp && g.box_w && g.box_h) {
            const int bpp  = g.bpp;
            const int full = (1 << bpp) - 1;
            const int top  = baseline - g.box_h - g.ofs_y;
            for (int row = 0; row < g.box_h; row++) {
                for (int col = 0; col < g.box_w; col++) {
                    uint32_t bit = (uint32_t)(row * g.box_w + col) * bpp;   // rows packed, no padding
                    int v = (bmp[bit >> 3] >> (8 - bpp - (bit & 7))) & full;
                    int X = x + g.ofs_x + col, Y = top + row;
                    if (v > full / 2 && X >= 0 && X < w && Y >= 0 && Y < h)
                        bmp_put(px, stride, X, Y, true);
                }
            }
        }
        x += g.adv_w;
    }
}

static void draw_no_internet_banner(void)
{
    uint16_t w = (img_buf[0] << 8) | img_buf[1];
    uint16_t h = (img_buf[2] << 8) | img_buf[3];
    if (w == 0 || h == 0 || w > IMG_WIDTH || h > IMG_HEIGHT) return;
    uint8_t *px = img_buf + IMG_HEADER;
    uint16_t stride = (w + 7) / 8;

    const lv_font_t *font = &lv_font_montserrat_14;
    int tw = glyph_text_width(font, NO_INTERNET_TEXT);
    if (tw + 2 * NO_INTERNET_PAD > w) {                 // too wide: smaller font
        font = &lv_font_montserrat_12;
        tw = glyph_text_width(font, NO_INTERNET_TEXT);
    }
    const int bw = tw + 2 * NO_INTERNET_PAD;
    const int bh = font->line_height + 2 * NO_INTERNET_PAD;
    int x0 = w / 2 - bw / 2;
    int y0 = h * NO_INTERNET_Y_NUM / NO_INTERNET_Y_DEN - bh / 2;
    if (x0 < 0) x0 = 0;
    if (y0 + bh > h) y0 = h - bh;

    for (int y = y0; y < y0 + bh; y++)                    // white box
        for (int x = x0; x < x0 + bw && x < w; x++)
            bmp_put(px, stride, x, y, false);
    const int baseline = y0 + NO_INTERNET_PAD + font->line_height - font->base_line;
    draw_glyph_text(px, stride, w, h, font, NO_INTERNET_TEXT, x0 + NO_INTERNET_PAD, baseline);
}

static void update_emotion_display(void)
{
    if (s_shutting_down) return;
    if (img_buf == NULL || img_count == 0) return;
    static int filtered_idx = 0;
    char filter[32];
    xSemaphoreTake(emotion_mutex, portMAX_DELAY);
    strncpy(filter, current_emotion, sizeof(filter));
    xSemaphoreGive(emotion_mutex);

    bool found = false;
    for (int i = 0; i < img_count; i++) {
        int idx = (filtered_idx + i) % img_count;
        if (filter[0] == '\0' || strstr(img_filenames[idx], filter) != NULL) {
            if (load_image_from_spiffs(img_filenames[idx])) {
                #if BATTERY_ICON_ON_DISPLAY
                draw_battery_icon();
                #endif
                if (s_no_internet) draw_no_internet_banner();
                display_bitmap();
            }
            filtered_idx = (idx + 1) % img_count;
            found = true;
            break;
        }
    }
    if (!found) filtered_idx = 0;
}

static bool has_required_files(void);  // forward declaration

// Stream-download one file from the server and save it to SPIFFS.
// Returns true on success.
static bool download_single_file(const char *short_id, const String &fname)
{
  String dl_url = String(OATS_SERVER) + "/download_file?device=" + short_id + "&name=" + fname;
  HTTPClient dl_http;
  dl_http.setTimeout(30000);
  dl_http.begin(dl_url);
  int code = dl_http.GET();
  if (code != 200) {
    printf("  %s: HTTP %d\n", fname.c_str(), code);
    dl_http.end();
    return false;
  }

  String spiffs_path = String("/") + fname;
  File f = SPIFFS.open(spiffs_path, "w");
  if (!f) {
    printf("  Cannot write %s\n", spiffs_path.c_str());
    dl_http.end();
    return false;
  }

  int written = dl_http.writeToStream(&f);
  f.close();
  dl_http.end();
  if (written < 0) {
    printf("  %s: writeToStream failed (%d)\n", fname.c_str(), written);
    SPIFFS.remove(String("/") + fname);
    return false;
  }
  printf("  %s: %d bytes\n", fname.c_str(), written);
  return true;
}

// Helper: parse a newline-delimited string and run action(fname) for each line.
static void _each_line(const String &list, std::function<void(const String &)> action)
{
  int start = 0, len = list.length();
  while (start < len) {
    int nl = list.indexOf('\n', start);
    String fname = (nl < 0) ? list.substring(start) : list.substring(start, nl);
    fname.trim();
    start = (nl < 0) ? len : nl + 1;
    if (fname.length() > 0) action(fname);
  }
}

// Sync files from the server:
//   1. Fetch /files_to_download — server-flagged updated files — force-download each.
//   2. POST /mark_downloaded so server clears their flags.
//   3. Re-scan SPIFFS; if still incomplete fall back to /list_files for any missing files.
// status_cb(msg) is called with a short status string before each file download.
static void download_files_from_server(std::function<void(const char *)> status_cb = nullptr)
{
  const char *short_id = wifi_get_dev_id() + 6;
  printf("Syncing files for device %s...\n", short_id);

  // --- Step 1: download server-flagged updates ---
  std::vector<String> downloaded;
  {
    HTTPClient http;
    http.setTimeout(15000);
    http.begin(String(OATS_SERVER) + "/files_to_download?device=" + short_id);
    int code = http.GET();
    if (code == 200) {
      String list = http.getString();
      http.end();
      _each_line(list, [&](const String &fname) {
        printf("  Updating: %s\n", fname.c_str());
        if (status_cb) status_cb(fname.c_str());
        if (download_single_file(short_id, fname))
          downloaded.push_back(fname);
      });
    } else {
      http.end();
    }
  }

  // --- Step 2: mark downloaded files on server ---
  if (!downloaded.empty()) {
    String body = "{\"device\":\"";
    body += short_id;
    body += "\",\"files\":[";
    for (size_t i = 0; i < downloaded.size(); i++) {
      if (i > 0) body += ",";
      body += "\"" + downloaded[i] + "\"";
    }
    body += "]}";
    HTTPClient http;
    http.setTimeout(10000);
    http.begin(String(OATS_SERVER) + "/mark_downloaded");
    http.addHeader("Content-Type", "application/json");
    http.POST(body);
    http.end();
  }

  // --- Step 3: fill in any still-missing files ---
  if (!has_required_files()) {
    printf("Files incomplete — fetching full list...\n");
    HTTPClient http;
    http.setTimeout(15000);
    http.begin(String(OATS_SERVER) + "/list_files?device=" + short_id);
    int code = http.GET();
    if (code == 200) {
      String list = http.getString();
      http.end();
      _each_line(list, [&](const String &fname) {
        if (!SPIFFS.exists(String("/") + fname)) {
          printf("  Missing: %s\n", fname.c_str());
          if (status_cb) status_cb(fname.c_str());
          download_single_file(short_id, fname);
        }
      });
      scan_spiffs_images();
    } else {
      http.end();
    }
  }

  printf("Sync complete.\n");
}

// Returns true if all required files are present in SPIFFS.
static bool has_required_files(void)
{
  if (!SPIFFS.exists("/config.json"))   return false;
  if (!SPIFFS.exists("/defaults.json")) return false;
  if (!SPIFFS.exists("/sensors.json"))  return false;
  return (img_count >= 12*5+1);
}

void user_app_init(void)
{
  // Must run before the latch is set: see detect_usb_power_at_boot().
  s_usb_powered_at_boot = detect_usb_power_at_boot();
  board_div.VBAT_POWER_ON();   // Latch battery power — must be first!
  battery_prefs_load();        // learned "full" reference and charge-complete flag
  board_div.POWEER_EPD_ON();
  // Audio power is toggled per-cycle in sensor_read_task; no need to keep it on here.

  /*epaper init*/
  custom_lcd_spi_t driver_config = {};
    driver_config.cs = EPD_CS_PIN;
    driver_config.dc = EPD_DC_PIN;
    driver_config.rst = EPD_RST_PIN;
    driver_config.busy = EPD_BUSY_PIN;
    driver_config.mosi = EPD_MOSI_PIN;
    driver_config.scl = EPD_SCK_PIN;
    driver_config.spi_host = EPD_SPI_NUM;
    driver_config.buffer_len  = 5000;
  // The black/white and the 4-color (G) boards share every pin but use
  // different display controllers. Pick the driver for the fitted panel.
  epaper_panel_t panel = (EPD_PANEL == 1) ? EPAPER_PANEL_BW
                       : (EPD_PANEL == 2) ? EPAPER_PANEL_G
                       : epaper_detect_panel(driver_config);
  if (panel == EPAPER_PANEL_G)
    driver = new epaper_driver_display_g(EPD_WIDTH,EPD_HEIGHT,driver_config);
  else
    driver = new epaper_driver_display(EPD_WIDTH,EPD_HEIGHT,driver_config);
  ESP_LOGI(TAG, "e-paper panel: %s", driver->name());
  driver->EPD_Init();
  driver->EPD_Clear();
  driver->EPD_DisplayPartBaseImage();
  driver->EPD_Init_Partial();

  // Allocate image buffer in PSRAM (5008 bytes for 1-bit 200x200 + header)
  img_buf = (uint8_t *)heap_caps_malloc(IMG_BUF_SIZE, MALLOC_CAP_SPIRAM);
  if (img_buf == NULL) {
    ESP_LOGE(TAG, "Failed to allocate %d bytes in PSRAM for image buffer", IMG_BUF_SIZE);
  }

  // Mount SPIFFS and scan for images
  SPIFFS.begin(true);
  // SPIFFS.remove("/base.bin"); // for debug
  // clear_spiffs(); // for debug
  // wifi_forget_credentials(); // for debug
  printf("SPIFFS total: %d bytes, used: %d bytes\n",SPIFFS.totalBytes(), SPIFFS.usedBytes());
  scan_spiffs_images();

  // Create mutex for emotion filter
  emotion_mutex = xSemaphoreCreateMutex();

  // Light sleep wakeup (WiFi modem power-save is set inside wifi_provision_init)
  // esp_sleep_enable_timer_wakeup(3 * 1000 * 1000);  // 3 seconds in microseconds

  // Button init and power latch check
  user_button_init();
  if (gpio_get_level(PWR_BUTTON_PIN) && (!is_vabtflag)) {
    is_vabtflag = true;
  }
}


void set_emotion(const char *emotion)
{
  xSemaphoreTake(emotion_mutex, portMAX_DELAY);
  if (strcmp(current_emotion, emotion) != 0) {
    strncpy(current_emotion, emotion, sizeof(current_emotion) - 1);
    current_emotion[sizeof(current_emotion) - 1] = '\0';
    emotion_changed = true;
  }
  xSemaphoreGive(emotion_mutex);
  ESP_LOGI(TAG, "Emotion set to: '%s'", emotion);
}

void loop_lvgl_img(void *arg)
{
  if (img_buf == NULL || img_count == 0) {
    printf("No images available (buf=%p, count=%d)\n", img_buf, img_count);
    vTaskDelete(NULL);
    return;
  }

  char filter[32];
  int filtered_idx = 0;

  for(;;)
  {
    // Snapshot the current emotion filter
    xSemaphoreTake(emotion_mutex, portMAX_DELAY);
    strncpy(filter, current_emotion, sizeof(filter));
    xSemaphoreGive(emotion_mutex);

    // Find the next matching image starting from filtered_idx
    bool found = false;

    for (int i = 0; i < img_count; i++) {
      int idx = (filtered_idx + i) % img_count;
      if (filter[0] == '\0' || strstr(img_filenames[idx], filter) != NULL) {
        if (load_image_from_spiffs(img_filenames[idx])) {
          display_bitmap();
        }
        filtered_idx = (idx + 1) % img_count;
        found = true;
        break;
      }
    }

    if (!found) {
      // No matching images, wait and retry
      filtered_idx = 0;
    }

    // Light sleep for 3 seconds — WiFi stays connected, CPU pauses
    // esp_light_sleep_start();
  }
}

// "<name>\nis sleeping... zzz" from /config.json, or a generic line without a name.
static void sleep_text_from_config(char *out, size_t n)
{
    snprintf(out, n, "Sleepy time... zzz");
    File cfg = SPIFFS.open("/config.json", "r");
    if (!cfg) return;
    String content = cfg.readString();
    cfg.close();
    int idx = content.indexOf("\"name\"");
    if (idx < 0) return;
    int q1 = content.indexOf('"', content.indexOf(':', idx) + 1);
    int q2 = content.indexOf('"', q1 + 1);
    if (q1 >= 0 && q2 > q1) {
      String name = content.substring(q1 + 1, q2);
      snprintf(out, n, "%s\nis sleeping... zzz", name.c_str());
    }
}

// Show sleep screen on e-ink and power off the device.
static void show_sleep_and_power_off(void)
{
    s_shutting_down = true;
    board_div.POWEER_EPD_ON();

    char sleep_text[80];
    sleep_text_from_config(sleep_text, sizeof(sleep_text));
    lv_obj_clean(src_ui.screen);
    lv_obj_t *sleep_label = lv_label_create(src_ui.screen);
    lv_label_set_text(sleep_label, sleep_text);
    lv_obj_set_style_text_color(sleep_label, lv_color_black(), 0);
    lv_obj_align(sleep_label, LV_ALIGN_CENTER, 0, 0);
    lv_task_handler();
    lv_refr_now(NULL);               // flushes and blocks until the panel reports done
    vTaskDelay(pdMS_TO_TICKS(100));  // small margin before cutting power
    board_div.VBAT_POWER_OFF();

    // On battery the line above cuts our power and we never get here. If we
    // are still running half a second later, USB is powering the board: the
    // device cannot actually turn off, so show the charging screen instead
    // of leaving "sleeping" on the panel.
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGI(TAG, "Still powered after latch release: USB present, entering charging mode");
    xTaskCreatePinnedToCore(charging_task, "charging", 6 * 1024, NULL, 4, NULL, 0);
}

void button_power_task(void *arg)
{
  xEventGroupClearBits(pwr_groups, set_bit_all);   // drop events from a button held through boot
  for (;;) {
    EventBits_t even = xEventGroupWaitBits(pwr_groups, set_bit_all, pdTRUE, pdFALSE, pdMS_TO_TICKS(2000));
    if (get_bit_button(even, 2)) {
      // Long press detected — shut down if power was latched (and not already shutting down)
      if (is_vabtflag && !s_shutting_down) {
        is_vabtflag = false;
        s_shutting_down = true;  // block sensor task from updating display
        ESP_LOGI(TAG, "Power button long press — shutting down");

        // Wait for the sensor task to park itself (it checks s_shutting_down
        // at the top of its loop) so no e-ink update is in flight. Bounded,
        // because a long upload can hold it a while and that path never
        // touches the panel.
        if (s_sensor_task_handle) {
          for (int i = 0; i < 30 && !s_sensor_idle; i++) vTaskDelay(pdMS_TO_TICKS(100));
        }
        show_sleep_and_power_off();
      }
    } else if (get_bit_button(even, 3)) {
      // Button released — latch power if not already
      if (!is_vabtflag) {
        is_vabtflag = true;
      }
    }
  }
}

// // Method 1: parse /emotion_logic.txt from SPIFFS and evaluate rules.
// // File format — one rule per line:
// //   voltage OP THRESHOLD EMOTION   (e.g.  voltage < 5.9 content)
// //   default EMOTION                (fallback if no rule matches)
// // Lines starting with '#' are ignored. First matching rule wins.
// static const char *apply_emotion_logic(float voltage, int raw)
// {
//   static char result[32];
//   strncpy(result, "calm", sizeof(result));
//   if (!SPIFFS.exists("/emotion_logic.txt")) return result;
//   File f = SPIFFS.open("/emotion_logic.txt", "r");
//   if (!f) return result;

//   while (f.available()) {
//     String line = f.readStringUntil('\n');
//     line.trim();
//     if (line.length() == 0 || line[0] == '#') continue;

//     if (line.startsWith("default ")) {
//       String em = line.substring(8);
//       em.trim();
//       strncpy(result, em.c_str(), sizeof(result) - 1);
//       result[sizeof(result) - 1] = '\0';
//       continue;
//     }

//     char sensor[16], op[4], emotion[32];
//     float threshold;
//     if (sscanf(line.c_str(), "%15s %3s %f %31s", sensor, op, &threshold, emotion) != 4) continue;

//     float reading = (strcmp(sensor, "raw") == 0) ? (float)raw : voltage;
//     bool match = false;
//     if      (strcmp(op, "<")  == 0) match = reading <  threshold;
//     else if (strcmp(op, "<=") == 0) match = reading <= threshold;
//     else if (strcmp(op, ">")  == 0) match = reading >  threshold;
//     else if (strcmp(op, ">=") == 0) match = reading >= threshold;

//     if (match) {
//       strncpy(result, emotion, sizeof(result) - 1);
//       result[sizeof(result) - 1] = '\0';
//       f.close();
//       return result;
//     }
//   }
//   f.close();
//   return result;
// }

// // Method 2: ask the server which emotion to show given current sensor values.
// // Calls GET /get_emotion?device=...&voltage=...&raw=... and parses the response.
// static const char *fetch_emotion_from_server(float voltage, int raw)
// {
//   static char result[32];
//   strncpy(result, "calm", sizeof(result));
//   const char *short_id = wifi_get_dev_id() + 6;
//   char url[256];
//   snprintf(url, sizeof(url),
//            OATS_SERVER "/get_emotion?device=%s&voltage=%.2f&raw=%d",
//            short_id, voltage, raw);
//   HTTPClient http;
//   http.setTimeout(5000);
//   http.begin(url);
//   int code = http.GET();
//   if (code == 200) {
//     String body = http.getString();
//     // Parse {"emotion":"VALUE"} without a JSON library
//     int idx = body.indexOf("\"emotion\":");
//     if (idx >= 0) {
//       int start = body.indexOf('"', idx + 10) + 1;
//       int end   = body.indexOf('"', start);
//       if (start > 0 && end > start) {
//         String em = body.substring(start, end);
//         strncpy(result, em.c_str(), sizeof(result) - 1);
//         result[sizeof(result) - 1] = '\0';
//       }
//     }
//   }
//   http.end();
//   return result;
// }

// void adc_emotion_task(void *arg)
// {
//   adc_bsp_init();
//   float voltage = 0;
//   int raw = 0;
//   for (;;) {
//     adc_get_value(&voltage, &raw);
//     ESP_LOGI(TAG, "ADC voltage: %.2fV", voltage);
//     const char *emotion = (emotion_method == 1)
//       ? apply_emotion_logic(voltage, raw)
//       : fetch_emotion_from_server(voltage, raw);
//     set_emotion(emotion);
//     vTaskDelay(pdMS_TO_TICKS(1000));
//   }
// }

// Charging mode: entered when the device booted on USB power with the PWR
// button up (see detect_usb_power_at_boot), or when a shutdown found USB
// still powering the board. Shows the same "<name> is sleeping... zzz" as
// the power-off screen, with the battery level at the top, refreshed every
// CHARGING_REFRESH_MS.
//
// Unplug handling depends on what is on the other end of the cable:
//  - A USB host (PC): its SOF frames are visible, so we keep the battery
//    latch on, poll for the host, and when it disappears we draw the sleep
//    screen and release the latch (power off).
//  - A plain charger / power bank: nothing on this board can see VBUS, so
//    the latch stays released and pulling the cable simply cuts power; the
//    panel keeps the charging screen.
// Pressing PWR restarts into normal mode; keep it held until the face appears.
static void charging_mode(void)
{
  s_in_charging_mode = true;
  // lv_obj_clean() removes the image widgets too. Don't touch them by
  // pointer here: on the shutdown-on-USB path they are already gone.
  lv_obj_clean(src_ui.screen);
  char sleep_text[80];
  sleep_text_from_config(sleep_text, sizeof(sleep_text));
  lv_obj_t *label = lv_label_create(src_ui.screen);     // "<name> is sleeping... zzz", centred
  lv_label_set_text(label, sleep_text);
  lv_obj_set_style_text_color(label, lv_color_black(), 0);
  lv_obj_set_width(label, EPD_WIDTH - 16);
  lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
  lv_obj_t *level = lv_label_create(src_ui.screen);     // battery level along the top edge
  lv_obj_set_style_text_color(level, lv_color_black(), 0);
  lv_obj_align(level, LV_ALIGN_TOP_MID, 0, 6);

  board_div.POWEER_EPD_ON();    // keep the panel powered: EPD_DisplayPart() does not
                                // re-initialise it, and USB has power to spare

  // Is a USB host talking to us? Give enumeration a couple of seconds.
  bool host = false;
  for (int i = 0; i < 20 && !host; i++) {
    host = usb_host_present();
    if (!host) vTaskDelay(pdMS_TO_TICKS(100));
  }
  if (host) board_div.VBAT_POWER_ON();    // survive the unplug long enough to show "sleeping"
  else      board_div.VBAT_POWER_OFF();   // unplug = power off
  ESP_LOGI(TAG, "charging mode: %s",
           host ? "USB host seen, unplug will show the sleep screen"
                : "no USB host (charger): unplug cuts power");

  // A full refresh on the 4-color panel is a 15 s flash: redraw less often there.
  const uint32_t refresh_ms = driver->fast_refresh() ? CHARGING_REFRESH_MS : (5UL * 60UL * 1000UL);

  char     txt[64];
  int      misses = 0;
  uint32_t last_refresh_ms = 0;
  bool     first = true;
  for (;;) {
    if (first || (millis() - last_refresh_ms) >= refresh_ms) {
      first = false;
      last_refresh_ms = millis();
      battery_sample();           // reads high while the charger is active; fine here
      ESP_LOGI(TAG, "charging: %d mV", s_batt_mv);
      if (battery_shown_pct() >= 0)
        snprintf(txt, sizeof(txt), LV_SYMBOL_CHARGE " %d%%", battery_shown_pct());
      else
        snprintf(txt, sizeof(txt), LV_SYMBOL_CHARGE " charging");
      lv_label_set_text(level, txt);
      lv_obj_invalidate(src_ui.screen);
      lv_refr_now(NULL);                       // redraw and flush to e-ink right now
    }

    if (host) {
      // Stay awake (light sleep would drop the USB link) and watch the host.
      if (usb_host_present()) {
        misses = 0;
      } else if (++misses >= 3) {              // ~3 s without SOF: cable pulled
        ESP_LOGI(TAG, "USB host gone: showing sleep screen and powering off");
        show_sleep_and_power_off();            // releases the latch -> power off on battery
        vTaskDelete(NULL);                     // still here = host only suspended; a new charging task took over
      }
      if (gpio_get_level(PWR_BUTTON_PIN) == 0) {
        vTaskDelay(pdMS_TO_TICKS(500));                 // debounce + intent
        if (gpio_get_level(PWR_BUTTON_PIN) == 0) {      // still held
          esp_restart();   // boots with the button down -> normal mode
        }
      }
      vTaskDelay(pdMS_TO_TICKS(1000));
    } else {
      // Sleep until the next refresh, or until PWR is pressed
      esp_sleep_enable_timer_wakeup((uint64_t)refresh_ms * 1000ULL);
      esp_sleep_enable_ext1_wakeup(1ULL << PWR_BUTTON_PIN, ESP_EXT1_WAKEUP_ANY_LOW);
      esp_light_sleep_start();
      if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1) {
        vTaskDelay(pdMS_TO_TICKS(500));                 // debounce + intent
        if (gpio_get_level(PWR_BUTTON_PIN) == 0) {      // still held
          board_div.VBAT_POWER_ON();
          esp_restart();   // boots with the button down -> normal mode
        }
      }
    }
  }
}

static void charging_task(void *arg)
{
  charging_mode();   // never returns
}

// Setup (AP) screen: nothing samples the battery there, so poll it. A cable
// arriving makes battery_sample() put the device on the charging screen.
static void charge_watch_task(void *arg)
{
    adc_bsp_init();   // idempotent; normally done later in boot by gpio_sensor_init()
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(CHARGE_WATCH_MS));
        if (s_shutting_down || s_in_charging_mode) break;
        battery_sample();
    }
    vTaskDelete(NULL);
}

void user_ui_init(void)
{
  setup_ui(&src_ui);

  // Booted on USB power with the PWR button up: show the charging screen.
  // Hold PWR while plugging in (or press it on the charging screen) to run
  // normally on USB instead, e.g. for development with the serial monitor.
  if (s_usb_powered_at_boot) {
    ESP_LOGI(TAG, "USB power detected at boot - entering charging mode");
    charging_mode();   // never returns
  }

  // Always start power button task so long-press shutdown works in all modes
  xTaskCreatePinnedToCore(button_power_task, "pwr_button", 4 * 1024, NULL, 5, NULL, 0);

  // Show loading text while WiFi initializes (this can take a few seconds)
  lv_obj_add_flag(src_ui.screen_img_1, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(src_ui.screen_img_2, LV_OBJ_FLAG_HIDDEN);
  lv_obj_t *loading_label = lv_label_create(src_ui.screen);
  lv_label_set_text(loading_label, "\nWarming up my tiny brain...");
  // lv_label_set_text(loading_label, "\nWarming up my tiny brain...\n\n\n Double click the gear button to re-setup wifi " LV_SYMBOL_RIGHT);
  lv_label_set_long_mode(loading_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(loading_label, EPD_WIDTH - 16);
  lv_obj_set_style_text_color(loading_label, lv_color_black(), 0);
  lv_obj_align(loading_label, LV_ALIGN_CENTER, 0, 0);
  lv_task_handler();  // flush label to display before blocking

  wifi_set_ap_timeout_cb(show_sleep_and_power_off);
  wifi_provision_init();
  lv_obj_del(loading_label);

  if (wifi_is_provisioning()) {
  // if (false) {
    printf("Entering wifi provisioning mode\n");
    // Hide image widgets — not needed during setup
    lv_obj_add_flag(src_ui.screen_img_1, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(src_ui.screen_img_2, LV_OBJ_FLAG_HIDDEN);

    // First boot (no avatar/config on flash yet) vs. re-provisioning WiFi later.
    const char *greeting = has_required_files()
        ? "Help me connect to the world :)"
        : "Help me hatch into the world :)";
    char ap_text[160];
    snprintf(ap_text, sizeof(ap_text),
             "Hello you!\n\n\\
%s\n\n\\
Connect to WiFi:\n%s\n\n\\
Then open browser:\nhttp://%s",
             greeting, wifi_get_ap_ssid(), wifi_get_ap_ip());

    lv_obj_t *label = lv_label_create(src_ui.screen);
    lv_label_set_text(label, ap_text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, EPD_WIDTH - 16);
    // Indent the setup text by four spaces' worth; padding (not literal spaces)
    // so wrapped lines are indented too.
    const lv_font_t *ap_font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    lv_obj_set_style_pad_left(label, 4 * lv_font_get_glyph_width(ap_font, ' ', ' '), 0);
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
    lv_task_handler();  // flush label to display before blocking

    // Watch for a charger while the setup screen is up (see charge_watch_task).
    xTaskCreatePinnedToCore(charge_watch_task, "charge_watch", 4 * 1024, NULL, 3, NULL, 0);
    return;  // AP timeout in wifi_provision handles shutdown
  }
  // Download any updated files before deciding which screen to show
  lv_obj_add_flag(src_ui.screen_img_1, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(src_ui.screen_img_2, LV_OBJ_FLAG_HIDDEN);

  char agent_name[64] = "";
  {
    File cfg = SPIFFS.open("/config.json", "r");
    if (cfg) {
      String content = cfg.readString();
      cfg.close();
      int idx = content.indexOf("\"name\"");
      if (idx >= 0) {
        int q1 = content.indexOf('"', content.indexOf(':', idx) + 1);
        int q2 = content.indexOf('"', q1 + 1);
        if (q1 >= 0 && q2 > q1) {
          content.substring(q1 + 1, q2).toCharArray(agent_name, sizeof(agent_name));
        }
      }
    }
  }

  char updating_text[80];
  if (agent_name[0] != '\0') {
    snprintf(updating_text, sizeof(updating_text), "Updating %s...", agent_name);
  } else {
    snprintf(updating_text, sizeof(updating_text), "Updating OAT...");
  }

  lv_obj_t *updating_label = lv_label_create(src_ui.screen);
  lv_label_set_text(updating_label, updating_text);
  lv_label_set_long_mode(updating_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(updating_label, EPD_WIDTH - 16);
  lv_obj_set_style_text_color(updating_label, lv_color_black(), 0);
  lv_obj_align(updating_label, LV_ALIGN_CENTER, 0, 0);
  // Slow (4-color) panel: each flush is a ~15 s flash, so show "Updating"
  // only if a file is actually downloaded, and once rather than per file.
  bool updating_shown = driver->fast_refresh();
  if (updating_shown) lv_task_handler();  // flush label to display before blocking

  download_files_from_server([&](const char *fname) {
    String text = String(updating_text) + "\n" + fname;
    lv_label_set_text(updating_label, text.c_str());
    if (driver->fast_refresh() || !updating_shown) {
      lv_task_handler();
      updating_shown = true;
    }
  });
  lv_obj_del(updating_label);

  // If the server queued a clear-history command, wipe local history now
  if (SPIFFS.exists("/clear_history")) {
    SPIFFS.remove("/clear_history");
    SPIFFS.remove(HIST_BIN_PATH);
    s_hist_count       = 0;
    s_hist_pos         = 0;
    s_hist_initialized = false;
    ESP_LOGI(TAG, "History cleared by server command");
  }

  // Restore history from SPIFFS (emotion will be updated on first hourly push)
  load_sensor_history();
  init_anomaly_anchors();

  wifi_off();  // no more HTTP needed — save power

  if (!has_required_files()) {
    lv_obj_add_flag(src_ui.screen_img_1, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(src_ui.screen_img_2, LV_OBJ_FLAG_HIDDEN);

    char setup_text[180];
    const char *short_id = wifi_get_dev_id() + 6;
    snprintf(setup_text, sizeof(setup_text),
             "\nPlease finish setup at:\nhttps://oats.daehwakim.com/%s",
             short_id);

    lv_obj_t *label = lv_label_create(src_ui.screen);
    lv_label_set_text(label, setup_text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, EPD_WIDTH - 16);
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t *hint = lv_label_create(src_ui.screen);
    lv_label_set_text(hint, "Long-press the power button (" LV_SYMBOL_POWER ") if done");
    lv_obj_set_style_text_color(hint, lv_color_black(), 0);
    lv_obj_set_width(hint, EPD_WIDTH - 16);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_task_handler();  // flush labels to display before blocking
    vTaskDelay(pdMS_TO_TICKS(500));  // allow e-ink refresh to complete
    board_div.VBAT_POWER_OFF();
    return;
  }

  printf("Entering main agent mode\n");

  // Load sensor flags from /sensors.json (already confirmed present above)
  load_sensor_config();

  // Enable sensor power rail (GP20 HIGH = 3.3 V) before I2C init
  #ifndef DEBUG_SENSOR_HISTORY
    gpio_config_t pwr_cfg = {};
    pwr_cfg.pin_bit_mask = (1ULL << SENSOR_PWR_PIN);
    pwr_cfg.mode         = GPIO_MODE_OUTPUT;
    pwr_cfg.pull_up_en   = GPIO_PULLUP_DISABLE;
    pwr_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    pwr_cfg.intr_type    = GPIO_INTR_DISABLE;
    gpio_config(&pwr_cfg);
    gpio_set_level(SENSOR_PWR_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(50));  // let sensor power stabilize
  #endif

  // Initialize I2C bus if any I2C sensor is needed
  if (sensor_temperature || sensor_humidity) {
    i2c_sensor_init();
    if (!shtc3_init()) {
      ESP_LOGE(TAG, "SHTC3 init failed — disabling temp/humidity");
      sensor_temperature = false;
      sensor_humidity    = false;
    }
  }

  // Initialize audio codec if sound is enabled
  if (sensor_sound) {
    if (!audio_bsp_init()) {
      ESP_LOGE(TAG, "Audio init failed — disabling sound");
      sensor_sound = false;
    }
  }

  // Configure GPIO pins that have a sensor type assigned
  gpio_sensor_init();

  // Determine if any sensor is active (I2C, audio, or GPIO)
  bool any_gpio = false;
  for (int i = 0; i < NUM_GPIO_PINS; i++)
    if (pin_sensor_type[i][0] != '\0') { any_gpio = true; break; }

  xTaskCreatePinnedToCore(sensor_read_task, "sensor_read", 6 * 1024, NULL, 3, &s_sensor_task_handle, 0);
  // xTaskCreatePinnedToCore(loop_lvgl_img, "loop_lvgl_img", 8 * 1024, NULL, 4, &loop_img_task_handle, 1);
#ifdef DEBUG_SENSOR_HISTORY
  xTaskCreatePinnedToCore(serial_debug_task, "serial_dbg", 4 * 1024, NULL, 2, NULL, 0);
#endif
  gpio_set_level(SENSOR_PWR_PIN, 0);
}
