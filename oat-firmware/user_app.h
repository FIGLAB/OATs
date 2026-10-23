#ifndef USER_APP_H
#define USER_APP_H

#include "src/display/epaper_driver_bsp.h"

extern epaper_driver_display *driver;

// Emotion method: 1 = evaluate local logic from /emotion_logic.txt,
//                 2 = request emotion string from server each cycle
extern int emotion_method;

#ifdef __cplusplus
extern "C" {
#endif

void user_app_init(void);
void user_ui_init(void);
void set_emotion(const char *emotion);

#ifdef __cplusplus
}
#endif

#endif