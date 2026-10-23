#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "user_config.h"
#include "lvgl.h"

#include "user_app.h"

static const char *TAG = "main_1_54";

static void example_lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
  uint16_t *buffer = (uint16_t *)color_map;
  driver->EPD_Clear();
  for(int y = area->y1; y <= area->y2; y++)
  {
   	for(int x = area->x1; x <= area->x2; x++)
   	{
   	  uint8_t color = (*buffer < 0x7fff) ? DRIVER_COLOR_BLACK : DRIVER_COLOR_WHITE;
   	  driver->EPD_DrawColorPixel(x,y,color);
   	  buffer++;
   	}
  }
  driver->EPD_DisplayPart();
	lv_disp_flush_ready(drv);
}

static void example_increase_lvgl_tick(void *arg)
{
  lv_tick_inc(EXAMPLE_LVGL_TICK_PERIOD_MS);
}

static void lvgl_port(void)
{
  static lv_disp_draw_buf_t disp_buf;
  static lv_disp_drv_t disp_drv;

  lv_init();

  lv_color_t *buffer_1 = (lv_color_t *)heap_caps_malloc(LVGL_SPIRAM_BUFF_LEN, MALLOC_CAP_SPIRAM);
  lv_color_t *buffer_2 = (lv_color_t *)heap_caps_malloc(LVGL_SPIRAM_BUFF_LEN, MALLOC_CAP_SPIRAM);
  assert(buffer_1);
  assert(buffer_2);
  lv_disp_draw_buf_init(&disp_buf, buffer_1, buffer_2, EPD_WIDTH * EPD_HEIGHT);

  ESP_LOGI(TAG, "Register display driver to LVGL");
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = EPD_WIDTH;
  disp_drv.ver_res = EPD_HEIGHT;
  disp_drv.flush_cb = example_lvgl_flush_cb;
  disp_drv.draw_buf = &disp_buf;
  disp_drv.full_refresh = 1;
  lv_disp_drv_register(&disp_drv);

  ESP_LOGI(TAG, "Install LVGL tick timer");
  esp_timer_create_args_t lvgl_tick_timer_args = {};
  lvgl_tick_timer_args.callback = &example_increase_lvgl_tick;
  lvgl_tick_timer_args.name = "lvgl_tick";
  esp_timer_handle_t lvgl_tick_timer = NULL;
  ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
  ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, EXAMPLE_LVGL_TICK_PERIOD_MS * 1000));

  user_ui_init();
}


void setup()
{
  Serial.begin(115200);
  user_app_init();
  lvgl_port();
}

void loop()
{

}
