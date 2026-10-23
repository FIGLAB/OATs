#include "button_bsp.h"
#include "multi_button.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "user_config.h"
#include "src/wifi/wifi_provision.h"

EventGroupHandle_t boot_groups;
EventGroupHandle_t pwr_groups;

// Boot button (GPIO 0) — double-click to forget WiFi credentials
static Button button1;
#define USER_KEY_1    BOOT_BUTTON_PIN
#define button1_id    1
#define button1_active 0   // active LOW

static Button button2;
#define USER_KEY_2 PWR_BUTTON_PIN
#define button2_id 2
#define button2_active 0

static void on_boot_double_click(Button* btn_handle);
static void on_pwr_longpress_press(Button* btn_handle);
static void on_pwr_pressup_press(Button* btn_handle);
static void on_boot_double_click(Button* btn_handle);

static void clock_task_callback(void *arg)
{
  	button_ticks();
}
static uint8_t read_button_GPIO(uint8_t button_id)
{
	switch (button_id)
  	{
	  	case button1_id:
	  	  	return gpio_get_level(USER_KEY_1);
  	  	case button2_id:
  	  	  	return gpio_get_level(USER_KEY_2);
  	  	default:
  	  	  	break;
  	}
  	return 1;
}

static void gpio_init(void)
{
  	gpio_config_t gpio_conf = {};
  	gpio_conf.intr_type = GPIO_INTR_DISABLE;
  	gpio_conf.mode = GPIO_MODE_INPUT;
  	gpio_conf.pin_bit_mask = (0x1ULL<<USER_KEY_1) | (0x1ULL<<USER_KEY_2);
  	gpio_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
  	gpio_conf.pull_up_en = GPIO_PULLUP_ENABLE;

  	ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&gpio_conf));
}

void user_button_init(void)
{
  	pwr_groups = xEventGroupCreate();
  	gpio_init();

  	// Boot button: double-click to forget WiFi and reboot into AP mode
  	button_init(&button1, read_button_GPIO, button1_active, button1_id);
  	button_attach(&button1, BTN_DOUBLE_CLICK, on_boot_double_click);
  	button_start(&button1);

  	button_init(&button2, read_button_GPIO, button2_active, button2_id);
	button_attach(&button2, BTN_LONG_PRESS_START, on_pwr_longpress_press);
	button_attach(&button2, BTN_PRESS_UP, on_pwr_pressup_press);

  	esp_timer_create_args_t clock_tick_timer_args = {};
  	  	clock_tick_timer_args.callback = &clock_task_callback;
  	  	clock_tick_timer_args.name = "clock_task";
  	  	clock_tick_timer_args.arg = NULL;
  	esp_timer_handle_t clock_tick_timer = NULL;
  	ESP_ERROR_CHECK(esp_timer_create(&clock_tick_timer_args, &clock_tick_timer));
  	ESP_ERROR_CHECK(esp_timer_start_periodic(clock_tick_timer, 1000 * 5));  //5ms
  	button_start(&button2);
}

static void on_boot_double_click(Button* btn_handle)
{
	wifi_enter_provisioning_mode();  // reboots into AP mode, NVS credentials kept
}

static void on_pwr_longpress_press(Button* btn_handle)
{
	xEventGroupSetBits(pwr_groups, set_bit_button(2));
}

static void on_pwr_pressup_press(Button* btn_handle)
{
	xEventGroupSetBits(pwr_groups, set_bit_button(3));
}
