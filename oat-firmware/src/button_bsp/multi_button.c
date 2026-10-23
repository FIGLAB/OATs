/*
 * Copyright (c) 2016 Zibin Zheng <znbin@qq.com>
 * All rights reserved
 */

#include "multi_button.h"

// Macro for callback execution with null check
#define EVENT_CB(ev)   do { if(handle->cb[ev]) handle->cb[ev](handle); } while(0)

// Button handle list head
static Button* head_handle = NULL;

// Forward declarations
static void button_handler(Button* handle);
static inline uint8_t button_read_level(Button* handle);

void button_init(Button* handle, uint8_t(*pin_level)(uint8_t), uint8_t active_level, uint8_t button_id)
{
	if (!handle || !pin_level) return;

	memset(handle, 0, sizeof(Button));
	handle->event = (uint8_t)BTN_NONE_PRESS;
	handle->hal_button_level = pin_level;
	handle->button_level = !active_level;
	handle->active_level = active_level;
	handle->button_id = button_id;
	handle->state = BTN_STATE_IDLE;
}

void button_attach(Button* handle, ButtonEvent event, BtnCallback cb)
{
	if (!handle || event >= BTN_EVENT_COUNT) return;
	handle->cb[event] = cb;
}

void button_detach(Button* handle, ButtonEvent event)
{
	if (!handle || event >= BTN_EVENT_COUNT) return;
	handle->cb[event] = NULL;
}

ButtonEvent button_get_event(Button* handle)
{
	if (!handle) return BTN_NONE_PRESS;
	return (ButtonEvent)(handle->event);
}

uint8_t button_get_repeat_count(Button* handle)
{
	if (!handle) return 0;
	return handle->repeat;
}

void button_reset(Button* handle)
{
	if (!handle) return;
	handle->state = BTN_STATE_IDLE;
	handle->ticks = 0;
	handle->repeat = 0;
	handle->event = (uint8_t)BTN_NONE_PRESS;
	handle->debounce_cnt = 0;
}

int button_is_pressed(Button* handle)
{
	if (!handle) return -1;
	return (handle->button_level == handle->active_level) ? 1 : 0;
}

static inline uint8_t button_read_level(Button* handle)
{
	return handle->hal_button_level(handle->button_id);
}

static void button_handler(Button* handle)
{
	uint8_t read_gpio_level = button_read_level(handle);

	if (handle->state > BTN_STATE_IDLE) {
		handle->ticks++;
	}

	if (read_gpio_level != handle->button_level) {
		if (++(handle->debounce_cnt) >= DEBOUNCE_TICKS) {
			handle->button_level = read_gpio_level;
			handle->debounce_cnt = 0;
		}
	} else {
		handle->debounce_cnt = 0;
	}

	switch (handle->state) {
	case BTN_STATE_IDLE:
		if (handle->button_level == handle->active_level) {
			handle->event = (uint8_t)BTN_PRESS_DOWN;
			EVENT_CB(BTN_PRESS_DOWN);
			handle->ticks = 0;
			handle->repeat = 1;
			handle->state = BTN_STATE_PRESS;
		} else {
			handle->event = (uint8_t)BTN_NONE_PRESS;
		}
		break;

	case BTN_STATE_PRESS:
		if (handle->button_level != handle->active_level) {
			handle->event = (uint8_t)BTN_PRESS_UP;
			EVENT_CB(BTN_PRESS_UP);
			handle->ticks = 0;
			handle->state = BTN_STATE_RELEASE;
		} else if (handle->ticks > LONG_TICKS) {
			handle->event = (uint8_t)BTN_LONG_PRESS_START;
			EVENT_CB(BTN_LONG_PRESS_START);
			handle->state = BTN_STATE_LONG_HOLD;
		}
		break;

	case BTN_STATE_RELEASE:
		if (handle->button_level == handle->active_level) {
			handle->event = (uint8_t)BTN_PRESS_DOWN;
			EVENT_CB(BTN_PRESS_DOWN);
			if (handle->repeat < PRESS_REPEAT_MAX_NUM) {
				handle->repeat++;
			}
			EVENT_CB(BTN_PRESS_REPEAT);
			handle->ticks = 0;
			handle->state = BTN_STATE_REPEAT;
		} else if (handle->ticks > SHORT_TICKS) {
			if (handle->repeat == 1) {
				handle->event = (uint8_t)BTN_SINGLE_CLICK;
				EVENT_CB(BTN_SINGLE_CLICK);
			} else if (handle->repeat == 2) {
				handle->event = (uint8_t)BTN_DOUBLE_CLICK;
				EVENT_CB(BTN_DOUBLE_CLICK);
			}
			handle->state = BTN_STATE_IDLE;
		}
		break;

	case BTN_STATE_REPEAT:
		if (handle->button_level != handle->active_level) {
			handle->event = (uint8_t)BTN_PRESS_UP;
			EVENT_CB(BTN_PRESS_UP);
			if (handle->ticks < SHORT_TICKS) {
				handle->ticks = 0;
				handle->state = BTN_STATE_RELEASE;
			} else {
				handle->state = BTN_STATE_IDLE;
			}
		} else if (handle->ticks > SHORT_TICKS) {
			handle->state = BTN_STATE_PRESS;
		}
		break;

	case BTN_STATE_LONG_HOLD:
		if (handle->button_level == handle->active_level) {
			handle->event = (uint8_t)BTN_LONG_PRESS_HOLD;
			EVENT_CB(BTN_LONG_PRESS_HOLD);
		} else {
			handle->event = (uint8_t)BTN_PRESS_UP;
			EVENT_CB(BTN_PRESS_UP);
			handle->state = BTN_STATE_IDLE;
		}
		break;

	default:
		handle->state = BTN_STATE_IDLE;
		break;
	}
}

int button_start(Button* handle)
{
	if (!handle) return -2;

	Button* target = head_handle;
	while (target) {
		if (target == handle) return -1;
		target = target->next;
	}

	handle->next = head_handle;
	head_handle = handle;
	return 0;
}

void button_stop(Button* handle)
{
	if (!handle) return;

	Button** curr;
	for (curr = &head_handle; *curr; ) {
		Button* entry = *curr;
		if (entry == handle) {
			*curr = entry->next;
			entry->next = NULL;
			return;
		} else {
			curr = &entry->next;
		}
	}
}

void button_ticks(void)
{
	Button* target;
	for (target = head_handle; target; target = target->next) {
		button_handler(target);
	}
}
