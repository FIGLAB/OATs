/* batt_probe: print the battery ADC on the Waveshare ESP32-S3-ePaper-1.54(G)
   once a second, the same way the firmware reads it (ADC1 CH3 = GPIO4,
   12 dB, curve-fitting calibration, 16-sample average, 200K/200K divider).
   Keeps the battery latch (GPIO17) on so the sketch survives an unplug, and
   records one sample per second in a ring buffer (last 20 min). Send 'd' to
   dump the buffer after plugging back in; 'l0'/'l1' set the latch. */
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_system.h"
#include "esp_attr.h"

static adc_oneshot_unit_handle_t adc;
static adc_cali_handle_t cali;
#define RING_N 600                                  // one sample per 2 s = 20 min
#define RING_MAGIC 0xB4770001u
RTC_NOINIT_ATTR static struct { uint16_t t; int16_t mv; } ring[RING_N];   // survives resets, not power loss
RTC_NOINIT_ATTR static uint32_t ring_count;
RTC_NOINIT_ATTR static uint32_t ring_magic;
RTC_NOINIT_ATTR static uint32_t boots;
static bool latch = true;

static int battery_percent(int mv)
{
  static const struct { int mv; int pct; } tbl[] = {
    {4200,100},{4100,90},{4000,78},{3900,62},{3800,45},{3750,35},{3700,25},{3650,15},{3600,8},{3500,3},{3300,0}};
  const int N = sizeof(tbl)/sizeof(tbl[0]);
  if (mv >= tbl[0].mv) return 100;
  for (int i = 1; i < N; i++) if (mv >= tbl[i].mv) {
    int dv = tbl[i-1].mv - tbl[i].mv, dp = tbl[i-1].pct - tbl[i].pct;
    return tbl[i].pct + (mv - tbl[i].mv) * dp / dv; }
  return 0;
}

void setup()
{
  Serial.begin(115200);
  pinMode(17, OUTPUT); digitalWrite(17, HIGH);   // latch on: keep running after unplug
  pinMode(3, OUTPUT);  digitalWrite(3, LOW);     // green LED (GPIO3, active low) blinks while alive
  if (ring_magic != RING_MAGIC) { ring_magic = RING_MAGIC; ring_count = 0; boots = 0; }
  boots++;
  adc_oneshot_unit_init_cfg_t u = { .unit_id = ADC_UNIT_1 };
  adc_oneshot_new_unit(&u, &adc);
  adc_oneshot_chan_cfg_t c = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12 };
  adc_oneshot_config_channel(adc, ADC_CHANNEL_3, &c);
  adc_cali_curve_fitting_config_t cc = { .unit_id = ADC_UNIT_1, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12 };
  esp_err_t e = adc_cali_create_scheme_curve_fitting(&cc, &cali);
  delay(1500);
  Serial.printf("batt_probe: cali %s, reset reason %d (1=power-on 3=sw 7=brownout 8=wdt), boot #%lu, %lu samples retained\n",
                e == ESP_OK ? "ok" : "FAILED", (int)esp_reset_reason(), (unsigned long)boots, (unsigned long)ring_count);
}

void loop()
{
  static uint32_t n = 0;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == 'd') {
      uint32_t from = ring_count > RING_N ? ring_count - RING_N : 0;
      Serial.printf("dump: %lu samples, boot #%lu, reset reason %d\n", (unsigned long)(ring_count - from), (unsigned long)boots, (int)esp_reset_reason());
      for (uint32_t i = from; i < ring_count; i++)
        Serial.printf("  #%4lu VBAT=%4d mV -> %3d%%\n", (unsigned long)i, ring[i % RING_N].mv, battery_percent(ring[i % RING_N].mv));
      Serial.println("dump end");
    } else if (c == 'l') {
      while (!Serial.available()) delay(1);
      latch = Serial.read() == '1'; digitalWrite(17, latch ? HIGH : LOW);
    }
  }
  long sraw = 0, smv = 0; int k = 0, mn = 99999, mx = 0;
  for (int i = 0; i < 16; i++) {
    int raw = 0, mv = 0;
    if (adc_oneshot_read(adc, ADC_CHANNEL_3, &raw) == ESP_OK) {
      adc_cali_raw_to_voltage(cali, raw, &mv);
      sraw += raw; smv += mv; k++; if (mv < mn) mn = mv; if (mv > mx) mx = mv;
    }
  }
  if (k) {
    int pin = smv / k, vbat = pin * 2;
    if (n % 2 == 0) { ring[ring_count % RING_N].t = (uint16_t)n; ring[ring_count % RING_N].mv = vbat; ring_count++; }
    Serial.printf("t=%3lus latch=%d raw=%4ld pin=%4d mV (min %d max %d)  VBAT=%4d mV  -> %3d%%\n",
                  (unsigned long)n, latch, sraw / k, pin, mn, mx, vbat, battery_percent(vbat));
  } else Serial.println("adc read failed");
  n++;
  for (int i = 0; i < 4; i++) { digitalWrite(3, i & 1); delay(250); }   // blink, 1 s total
}
