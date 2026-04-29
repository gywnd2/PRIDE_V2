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
#define BATTERY_GAUGE_X_POS 440
#define BATTERY_GAUGE_Y_POS 64 // -30
#define COOLANT_GAUGE_WIDTH 304
#define BATTERY_GAUGE_WIDTH 308
#define GAUGE_GAP_LEFT (COOLANT_GAUGE_X_POS + COOLANT_GAUGE_WIDTH)
#define GAUGE_GAP_RIGHT BATTERY_GAUGE_X_POS

#define CENTER_STATUS_BAR_WIDTH 6
#define CENTER_STATUS_BAR_HEIGHT 130
#define CENTER_STATUS_BAR_X ((GAUGE_GAP_LEFT + GAUGE_GAP_RIGHT - CENTER_STATUS_BAR_WIDTH) / 2)
#define CENTER_STATUS_BAR_Y 88
#define CENTER_STATUS_LABEL_WIDTH 70
#define CENTER_STATUS_LABEL_X ((GAUGE_GAP_LEFT + GAUGE_GAP_RIGHT - CENTER_STATUS_LABEL_WIDTH) / 2)
#define CENTER_STATUS_LABEL_Y (CENTER_STATUS_BAR_Y - 28)
#define CENTER_STATUS_TOUCH_PADDING 8

#define OIL_POPUP_WIDTH ((SCREEN_WIDTH * 3) / 4)
#define OIL_POPUP_HEIGHT ((SCREEN_HEIGHT * 3) / 4)
#define OIL_POPUP_BUTTON_WIDTH 126
#define OIL_POPUP_BUTTON_HEIGHT 72
#define OIL_POPUP_BUTTON_GAP 10
#define OIL_POPUP_PRESS_ZOOM 276

#define OIL_POPUP_KEYPAD_WIDTH 540
#define OIL_POPUP_EDIT_DUE_WIDTH 190
#define OIL_POPUP_EDIT_DUE_Y 4
#define OIL_POPUP_EDIT_DUE_X_OFFSET (-(OIL_POPUP_KEYPAD_WIDTH / 2) + (OIL_POPUP_EDIT_DUE_WIDTH / 2))
#define OIL_POPUP_EDIT_CLOSE_WIDTH 56
#define OIL_POPUP_EDIT_CLOSE_HEIGHT 56
#define OIL_POPUP_EDIT_CLOSE_Y (OIL_POPUP_EDIT_DUE_Y + ((72 - OIL_POPUP_EDIT_CLOSE_HEIGHT) / 2))
#define OIL_POPUP_EDIT_CLOSE_X_OFFSET ((OIL_POPUP_KEYPAD_WIDTH / 2) - (OIL_POPUP_EDIT_CLOSE_WIDTH / 2))
#define OIL_POPUP_EDIT_INPUT_ROW_Y 82
#define OIL_POPUP_EDIT_INPUT_ROW_WIDTH OIL_POPUP_KEYPAD_WIDTH
#define OIL_POPUP_EDIT_INPUT_ROW_HEIGHT 56
#define OIL_POPUP_EDIT_OK_WIDTH 100
#define OIL_POPUP_EDIT_CLEAR_WIDTH 100
#define OIL_POPUP_EDIT_INPUT_GAP 10
#define OIL_POPUP_EDIT_INPUT_WIDTH (OIL_POPUP_EDIT_INPUT_ROW_WIDTH - OIL_POPUP_EDIT_OK_WIDTH - OIL_POPUP_EDIT_CLEAR_WIDTH - (OIL_POPUP_EDIT_INPUT_GAP * 2))
#define OIL_POPUP_KEYPAD_HEIGHT 160
#define OIL_POPUP_KEYPAD_BOTTOM_OFFSET -4
#define OIL_POPUP_KEY_WIDTH 100
#define OIL_POPUP_KEY_HEIGHT 75
#define OIL_POPUP_KEY_GAP 10
#define OIL_CYCLE_DEFAULT_KM 7000U
#define OIL_CYCLE_INPUT_MAX 10

#define OIL_ICON_ORIGINAL_WIDTH 157
#define OIL_ICON_SCALE 104
#define OIL_ICON_WIDTH ((OIL_ICON_ORIGINAL_WIDTH * OIL_ICON_SCALE) / 256)
#define OIL_ICON_X ((GAUGE_GAP_LEFT + GAUGE_GAP_RIGHT - OIL_ICON_WIDTH) / 2)
#define OIL_ICON_Y (CENTER_STATUS_BAR_Y + CENTER_STATUS_BAR_HEIGHT + 6)

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

#define COLD_X_POS 50
#define COLD_Y_POS 215
#define COLD_SCALE 117

#define SERVICE_X_POS 245
#define SERVICE_Y_POS 297
#define SERVICE_SCALE 137

#define OVERHEAT_X_POS 273
#define OVERHEAT_Y_POS 286
#define OVERHEAT_SCALE 155

#define COOLANT_COLD_THRESHOLD_C 50
#define COOLANT_OVERHEAT_THRESHOLD_C 80

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
LV_IMG_DECLARE(service);
LV_IMG_DECLARE(cold);
LV_IMG_DECLARE(overheat);
LV_IMG_DECLARE(oil);
LV_IMG_DECLARE(oil_reset_prompt);
LV_IMG_DECLARE(oil_popup_yes);
LV_IMG_DECLARE(oil_popup_no);
LV_IMG_DECLARE(oil_popup_edit);
LV_IMG_DECLARE(oil_popup_due);

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
bool ui_debug_overlay_animating(void);
void ui_debug_log_enqueue(const char* line);
void update_monitor_ui(monitor_item_t * item, int32_t usage);
void update_system_monitor(int32_t ram_percent, int32_t core1_percent, int32_t core2_percent);
void update_coolant_gauge(int32_t val);
void update_battery_gauge(int32_t val);
void update_obd_gauges(bool obd_connected, bool coolant_valid, int32_t coolant_val, bool battery_valid, int32_t battery_val);
void update_outside_temp(int32_t temp_c, bool valid);
void update_service_icon(bool visible);
void update_oil_percent(int32_t percent);
void update_clock_text(const char* text);
void update_wifi_icon_connected(int32_t rssi);
void update_wifi_icon_disconnected(void);
void update_bt_icon_connected(void);
void update_bt_icon_disconnected(void);
void update_obd_icon_connected(void);
void update_obd_icon_disconnected(void);
bool ui_reset_service_odo(void);
bool ui_get_service_oil_cycle_km(uint32_t* outKm);
bool ui_set_service_oil_cycle_km(uint32_t cycleKm);
bool ui_reset_service_oil_cycle_km(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif
