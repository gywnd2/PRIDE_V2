#ifndef __UI__
#define __UI__

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCREEN_WIDTH 800
#define SCREEN_HEIGHT 480

#define COOLANT_GAUGE_X_POS 35
#define COOLANT_GAUGE_Y_POS 67 // -25
#define BATTERY_GAUGE_X_POS 410
#define BATTERY_GAUGE_Y_POS 64 // -30

#define REF_BG_SCALE       96
#define REF_NEEDLE_SCALE   70
#define REF_X_OFFSET       1
#define REF_Y_OFFSET       -128

#define GAUGE_PIVOT_X      36
#define GAUGE_PIVOT_Y      140
#define GAUGE_START_ANGLE  770
#define GAUGE_MIN_ROT      2120
#define GAUGE_MOVE_RANGE   1420

#define WIFI_X_POS 485
#define WIFI_Y_POS 384
#define WIFI_SCALE 55

#define BT_X_POS 585
#define BT_Y_POS 384
#define BT_SCALE 19

#define OBD_X_POS 658
#define OBD_Y_POS 395
#define OBD_SCALE 29

#define FROST_X_POS 255
#define FROST_Y_POS 290
#define FROST_SCALE 140

LV_IMG_DECLARE(needle);
LV_IMG_DECLARE(coolantGauge);
LV_IMG_DECLARE(batteryGauge);
LV_IMG_DECLARE(btOn);
LV_IMG_DECLARE(obdOn);
LV_IMG_DECLARE(btOff);
LV_IMG_DECLARE(obdOff);
LV_IMG_DECLARE(wifi_full);
LV_IMG_DECLARE(wifi_3);
LV_IMG_DECLARE(wifi_2);
LV_IMG_DECLARE(wifi_1);
LV_IMG_DECLARE(wifi_off);
LV_IMG_DECLARE(frost);
LV_IMG_DECLARE(location_avail);
LV_IMG_DECLARE(location_non);

typedef struct {
    lv_obj_t * bar;
    lv_obj_t * label_val;
} monitor_item_t;

void GaugeInit();
void UiResetRuntimeState(void);
void GaugeSetValue(int16_t value);
void GaugeSetNeedleAngle(int16_t angle);
void CreateMainBackground();

void create_outside_temp();
void create_clock();
void create_gauge();
void create_weather();
void create_sys_monitor_panel();
void create_goodbye_screen(const char* time_text, const char* distance_text);
void create_debug_screen(void);
bool ui_debug_log_capture_enabled(void);
void ui_debug_log_enqueue(const char* line);
void update_monitor_ui(monitor_item_t * item, int32_t usage);
void update_system_monitor(int32_t ram_percent, int32_t core1_percent, int32_t core2_percent);
void update_coolant_gauge(int32_t val);
void update_battery_gauge(int32_t val);
void update_obd_gauges(bool obd_connected, bool coolant_valid, int32_t coolant_val, bool battery_valid, int32_t battery_val);
void update_outside_temp(int32_t temp_c, bool valid);
void update_clock_text(const char* text);
void update_wifi_icon_connected(int32_t rssi);
void update_wifi_icon_disconnected(void);
void update_bt_icon_connected(void);
void update_bt_icon_disconnected(void);
void update_obd_icon_connected(void);
void update_obd_icon_disconnected(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif
