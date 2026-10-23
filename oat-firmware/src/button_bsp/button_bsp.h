#ifndef BUTTON_BSP_H
#define BUTTON_BSP_H
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#ifdef __cplusplus
extern "C" {
#endif

extern EventGroupHandle_t boot_groups;
extern EventGroupHandle_t pwr_groups;
#define set_bit_button(x) ((uint32_t)(0x01)<<(x))
#define get_bit_button(x,y) (((uint32_t)(x)>>(y)) & 0x01)
#define set_bit_all 0x00ffffff

void user_button_init(void);

#ifdef __cplusplus
}
#endif
#endif
