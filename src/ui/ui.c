#include <ui.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifndef UI_DEBUG_SCREEN_ENABLED
#define UI_DEBUG_SCREEN_ENABLED 0
#endif

static lv_obj_t * target_needle = NULL;
static lv_obj_t * target_batt_needle = NULL;
static lv_obj_t * icon_bt = NULL;
static lv_obj_t * icon_obd = NULL;
static lv_obj_t * icon_wifi = NULL;
static lv_obj_t * icon_frost = NULL;
static lv_obj_t * icon_service = NULL;
static lv_obj_t * icon_cold = NULL;
static lv_obj_t * icon_overheat = NULL;

static lv_obj_t * clock_label = NULL;
static lv_obj_t * outside_temp_label = NULL;
static lv_obj_t * oil_percent_label = NULL;
static lv_obj_t * oil_life_bar = NULL;
static lv_obj_t * oil_popup_overlay = NULL;
static lv_obj_t * oil_cycle_input_label = NULL;
static lv_obj_t * main_test_panel = NULL;
static bool outside_temp_rendered_valid = false;
static int32_t outside_temp_rendered_value = 1000;
static bool frost_icon_rendered_visible = false;
static bool service_icon_rendered_visible = false;
static bool cold_icon_rendered_visible = false;
static bool overheat_icon_rendered_visible = false;
static bool service_icon_requested_visible = false;
static bool needle_ceremony_warning_icons_active = false;
static lv_timer_t * startup_warning_icon_timer = NULL;
static bool startup_warning_icon_sequence_active = false;
static uint8_t startup_warning_icon_sequence_phase = 0;
static lv_timer_t * oil_life_ceremony_timer = NULL;
static bool oil_life_ceremony_active = false;
static uint32_t oil_life_ceremony_start_ms = 0;
static int32_t oil_percent_requested_value = 100;
static int32_t oil_percent_rendered_value = -1;
static char oil_cycle_input_text[OIL_CYCLE_INPUT_MAX + 1] = "7000";
static bool oil_cycle_input_replace_on_next_digit = false;
static bool oil_popup_replace_mode = false;
static bool oil_touch_long_pressed = false;

static monitor_item_t cpu_core1, cpu_core2, ram_usage;
static lv_timer_t * monitor_ceremony_timer = NULL;
static bool monitor_ceremony_active = false;
static uint32_t monitor_ceremony_start_ms = 0;
static int32_t pending_ram_percent = 0;
static int32_t pending_core1_percent = 0;
static int32_t pending_core2_percent = 0;

static lv_timer_t * needle_ceremony_timer = NULL;
static bool needle_ceremony_active = false;
static bool needle_ceremony_pending = false;
static uint32_t needle_ceremony_start_ms = 0;
static bool coolant_needle_animating = false;
static bool battery_needle_animating = false;
static bool coolant_pending_valid = false;
static int32_t coolant_pending_value = 0;
static bool battery_pending_valid = false;
static int32_t battery_pending_value = 0;
static bool obd_connected_latched = false;
static bool coolant_valid_latched = false;
static int32_t coolant_value_latched = 0;
static bool battery_valid_latched = false;
static int32_t battery_value_latched = 0;
static int32_t coolant_rendered_value = -1;
static int32_t battery_rendered_value = -1;
static lv_obj_t * debug_container = NULL;
static lv_obj_t * debug_swipe_zone = NULL;
static lv_obj_t * debug_close_zone = NULL;
static lv_obj_t * debug_log_ta = NULL;
static volatile bool debug_screen_visible = false;
static volatile bool debug_animation_active = false;
static bool debug_swipe_tracking = false;
static bool debug_hide_tracking = false;
static lv_point_t debug_swipe_start = {0, 0};
static lv_point_t debug_hide_start = {0, 0};
static lv_timer_t * debug_log_flush_timer = NULL;
static QueueHandle_t debug_log_queue = NULL;

static const int32_t DEBUG_OPEN_ZONE_HEIGHT = 100;
static const int32_t DEBUG_CLOSE_ZONE_HEIGHT = 100;
static const int32_t DEBUG_OPEN_TRIGGER_PX = 60;
static const int32_t DEBUG_CLOSE_TRIGGER_PX = 60;
// Keep this false when touch coordinates are already aligned to physical Y.
static const bool DEBUG_GESTURE_Y_INVERTED = false;
static const uint32_t DEBUG_PANEL_ANIM_MS = 500;
static const uint32_t DEBUG_LOG_FLUSH_PERIOD_MS = 200;
static const uint32_t DEBUG_LOG_FLUSH_BATCH = 8;
static const uint32_t DEBUG_LOG_QUEUE_DEPTH = 96;
static const uint32_t DEBUG_LOG_TEXT_MAX_CHARS = 6000;
static const uint32_t DEBUG_LOG_ENQUEUE_MAX_PER_SEC = 12;
static const uint32_t NEEDLE_RUNTIME_ANIM_MS = 300;
static const uint32_t STARTUP_WARNING_ICON_STEP_MS = 500;
static const uint32_t OIL_LIFE_CEREMONY_PHASE_MS = 1500;
static const uint32_t OIL_LIFE_CEREMONY_TIMER_MS = 16;
static const int32_t COOLANT_NEEDLE_IGNORE_DELTA = 1;
static const int32_t COOLANT_NEEDLE_INSTANT_DELTA = 2;
static const int32_t BATTERY_NEEDLE_INSTANT_DELTA = 1;

static portMUX_TYPE debug_log_budget_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t debug_log_budget_window_ms = 0;
static uint16_t debug_log_budget_count = 0;
static uint16_t debug_log_budget_dropped = 0;

static void set_coolant_gauge_instant(int32_t val);
static void set_battery_gauge_instant(int32_t val);
static void apply_coolant_gauge_value(int32_t val, bool force_anim);
static void apply_battery_gauge_value(int32_t val, bool force_anim);
static void start_needle_ceremony(void);
static void apply_latched_obd_needles(void);
static void set_icon_rendered_visible(lv_obj_t * icon, bool * rendered_visible, bool visible);
static void set_needle_ceremony_warning_icons(bool visible);
static void start_startup_warning_icon_sequence(void);
static void startup_warning_icon_sequence_timer_cb(lv_timer_t * t);
static void finish_startup_warning_icon_sequence(void);
static void apply_latched_warning_icon_states(void);
static bool get_latched_frost_visible(void);
static void update_coolant_warning_icons(bool valid, int32_t coolant_val);
static int32_t clamp_percent(int32_t percent);
static void set_oil_percent_display(int32_t percent, lv_anim_enable_t anim);
static void close_oil_popup_overlay(lv_obj_t * overlay);
static void reset_oil_popup_content_refs(void);
static void oil_popup_yes_button_event_cb(lv_event_t * e);
static void oil_popup_no_button_event_cb(lv_event_t * e);
static void oil_popup_edit_button_event_cb(lv_event_t * e);
static void oil_popup_ok_button_event_cb(lv_event_t * e);
static void oil_popup_clear_button_event_cb(lv_event_t * e);
static void oil_popup_digit_button_event_cb(lv_event_t * e);
static lv_obj_t * create_oil_popup_button_base(lv_obj_t * parent, int32_t width, int32_t height, lv_color_t bg_color, lv_color_t border_color);
static lv_obj_t * create_oil_popup_image_button(lv_obj_t * parent, const void * label_src, lv_color_t bg_color, lv_color_t border_color);
static lv_obj_t * create_oil_popup_text_button(lv_obj_t * parent, const char * text, int32_t width, int32_t height, lv_color_t bg_color, lv_color_t border_color, const lv_font_t * font);
static void show_oil_popup(void);
static void show_oil_popup_with_mode(bool edit_mode, bool replace_mode);
static void show_oil_replace_popup(bool edit_mode);
static void show_oil_popup_confirm_content(lv_obj_t * popup);
static void show_oil_popup_edit_content(lv_obj_t * popup);
static void oil_touch_area_pressed_event_cb(lv_event_t * e);
static void oil_touch_area_event_cb(lv_event_t * e);
static void oil_touch_area_long_press_event_cb(lv_event_t * e);
static void start_oil_life_ceremony(void);
static void finish_oil_life_ceremony(void);
static void oil_life_ceremony_timer_cb(lv_timer_t * t);
static bool get_active_touch_point(lv_event_t * e, lv_point_t * p);
static void debug_invalidate_full_screen(void);
static void debug_show_panel(bool anim);
static void debug_hide_panel(bool anim);
static void debug_swipe_zone_event_cb(lv_event_t * e);
static void debug_close_zone_event_cb(lv_event_t * e);
static void create_debug_swipe_zone(void);
static void create_debug_close_zone(void);
static QueueHandle_t get_debug_log_queue(void);
static void debug_log_queue_clear(void);
static void debug_log_flush_timer_cb(lv_timer_t * t);
static int32_t debug_get_physical_y(int32_t y);
static void debug_log_budget_reset(void);
static bool debug_log_budget_take(uint16_t * dropped_report);
static void debug_log_queue_push_line(const char* line);

static bool get_active_touch_point(lv_event_t * e, lv_point_t * p)
{
    if (!p) return false;
    lv_indev_t * indev = NULL;
    if (e) {
        indev = lv_event_get_indev(e);
    }
    if (!indev) {
        indev = lv_indev_get_act();
    }
    if (!indev) {
        indev = lv_indev_get_next(NULL);
        while (indev && lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) {
            indev = lv_indev_get_next(indev);
        }
    }
    if (!indev) return false;
    lv_indev_get_point(indev, p);
    return true;
}

static int32_t debug_get_physical_y(int32_t y)
{
    if (!DEBUG_GESTURE_Y_INVERTED) return y;
    return (SCREEN_HEIGHT - 1) - y;
}

static void debug_log_budget_reset(void)
{
    portENTER_CRITICAL(&debug_log_budget_mux);
    debug_log_budget_window_ms = 0;
    debug_log_budget_count = 0;
    debug_log_budget_dropped = 0;
    portEXIT_CRITICAL(&debug_log_budget_mux);
}

static bool debug_log_budget_take(uint16_t * dropped_report)
{
    if (dropped_report) *dropped_report = 0;
    uint32_t now = (uint32_t)(xTaskGetTickCount() * (TickType_t)portTICK_PERIOD_MS);
    bool allow = false;

    portENTER_CRITICAL(&debug_log_budget_mux);
    if (debug_log_budget_window_ms == 0 || (now - debug_log_budget_window_ms) >= 1000U) {
        debug_log_budget_window_ms = now;
        debug_log_budget_count = 0;
        if (dropped_report) {
            *dropped_report = debug_log_budget_dropped;
        }
        debug_log_budget_dropped = 0;
    }

    if (debug_log_budget_count < DEBUG_LOG_ENQUEUE_MAX_PER_SEC) {
        debug_log_budget_count++;
        allow = true;
    } else {
        debug_log_budget_dropped++;
    }
    portEXIT_CRITICAL(&debug_log_budget_mux);

    return allow;
}

static void debug_invalidate_full_screen(void)
{
    lv_obj_t * scr = lv_scr_act();
    if (!scr) return;
    lv_obj_invalidate(scr);
}

static void debug_panel_y_anim_exec_cb(void * var, int32_t y)
{
    lv_obj_set_y((lv_obj_t *)var, y);
}

static void debug_show_anim_ready_cb(lv_anim_t * a)
{
    LV_UNUSED(a);
    debug_animation_active = false;
    debug_invalidate_full_screen();
}

static void debug_hide_anim_ready_cb(lv_anim_t * a)
{
    LV_UNUSED(a);
    debug_animation_active = false;
    if (debug_container) {
        lv_obj_add_flag(debug_container, LV_OBJ_FLAG_HIDDEN);
    }
    if (debug_close_zone) {
        lv_obj_add_flag(debug_close_zone, LV_OBJ_FLAG_HIDDEN);
    }
    if (debug_swipe_zone) {
        lv_obj_move_foreground(debug_swipe_zone);
    }
    debug_invalidate_full_screen();
}

static void debug_show_panel(bool anim)
{
    if (!debug_container || debug_screen_visible || debug_animation_active) return;

    debug_screen_visible = true;
    lv_obj_clear_flag(debug_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(debug_container);
    if (debug_close_zone) {
        lv_obj_clear_flag(debug_close_zone, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(debug_close_zone);
    }
    lv_anim_del(debug_container, (lv_anim_exec_xcb_t)debug_panel_y_anim_exec_cb);
    if (debug_log_flush_timer) {
        lv_timer_resume(debug_log_flush_timer);
    }

    if (!anim) {
        debug_animation_active = false;
        lv_obj_set_y(debug_container, 0);
        debug_log_flush_timer_cb(NULL);
        debug_invalidate_full_screen();
        return;
    }

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, debug_container);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)debug_panel_y_anim_exec_cb);
    lv_anim_set_values(&a, lv_obj_get_y(debug_container), 0);
    lv_anim_set_time(&a, DEBUG_PANEL_ANIM_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_ready_cb(&a, debug_show_anim_ready_cb);
    debug_animation_active = true;
    lv_anim_start(&a);
}

static void debug_hide_panel(bool anim)
{
    if (!debug_container || !debug_screen_visible || debug_animation_active) return;

    debug_screen_visible = false;
    if (debug_close_zone) {
        lv_obj_add_flag(debug_close_zone, LV_OBJ_FLAG_HIDDEN);
    }
    lv_anim_del(debug_container, (lv_anim_exec_xcb_t)debug_panel_y_anim_exec_cb);
    if (debug_log_flush_timer) {
        lv_timer_pause(debug_log_flush_timer);
    }
    debug_log_queue_clear();
    debug_log_budget_reset();

    if (!anim) {
        debug_animation_active = false;
        lv_obj_set_y(debug_container, -SCREEN_HEIGHT);
        lv_obj_add_flag(debug_container, LV_OBJ_FLAG_HIDDEN);
        if (debug_swipe_zone) {
            lv_obj_move_foreground(debug_swipe_zone);
        }
        debug_invalidate_full_screen();
        return;
    }

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, debug_container);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)debug_panel_y_anim_exec_cb);
    lv_anim_set_values(&a, lv_obj_get_y(debug_container), -SCREEN_HEIGHT);
    lv_anim_set_time(&a, DEBUG_PANEL_ANIM_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_ready_cb(&a, debug_hide_anim_ready_cb);
    debug_animation_active = true;
    lv_anim_start(&a);
}

static void debug_swipe_zone_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_PRESSED) {
        if (debug_screen_visible || debug_animation_active) return;
        debug_swipe_tracking = false;
        if (!get_active_touch_point(e, &debug_swipe_start)) return;
        int32_t start_y = debug_get_physical_y(debug_swipe_start.y);
        if (start_y > DEBUG_OPEN_ZONE_HEIGHT) return;
        debug_swipe_tracking = true;
        return;
    }

    if (code == LV_EVENT_PRESSING) {
        if (!debug_swipe_tracking || debug_screen_visible || debug_animation_active) return;
        lv_point_t p;
        if (!get_active_touch_point(e, &p)) return;

        int32_t dy = debug_get_physical_y(p.y) - debug_get_physical_y(debug_swipe_start.y);
        if (dy >= DEBUG_OPEN_TRIGGER_PX) {
            debug_swipe_tracking = false;
            debug_show_panel(true);
        }
        return;
    }

    if (code == LV_EVENT_RELEASED) {
        if (debug_swipe_tracking && !debug_screen_visible && !debug_animation_active) {
            lv_point_t p;
            if (get_active_touch_point(e, &p)) {
                int32_t dy = debug_get_physical_y(p.y) - debug_get_physical_y(debug_swipe_start.y);
                if (dy >= DEBUG_OPEN_TRIGGER_PX) {
                    debug_show_panel(true);
                }
            }
        }
        debug_swipe_tracking = false;
        return;
    }

    if (code == LV_EVENT_PRESS_LOST) {
        debug_swipe_tracking = false;
    }
}

static void debug_close_zone_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_PRESSED) {
        if (!debug_screen_visible || debug_animation_active) return;
        lv_point_t p;
        if (!get_active_touch_point(e, &p)) return;
        int32_t start_y = debug_get_physical_y(p.y);
        if (start_y < (SCREEN_HEIGHT - DEBUG_CLOSE_ZONE_HEIGHT)) return;
        debug_hide_start = p;
        debug_hide_tracking = true;
        return;
    }

    if (code == LV_EVENT_PRESSING) {
        if (!debug_hide_tracking || !debug_screen_visible || debug_animation_active) return;
        lv_point_t p;
        if (!get_active_touch_point(e, &p)) return;

        int32_t dy = debug_get_physical_y(p.y) - debug_get_physical_y(debug_hide_start.y);
        if (dy <= -DEBUG_CLOSE_TRIGGER_PX) {
            debug_hide_tracking = false;
            debug_hide_panel(true);
        }
        return;
    }

    if (code == LV_EVENT_RELEASED) {
        if (debug_hide_tracking && debug_screen_visible && !debug_animation_active) {
            lv_point_t p;
            if (get_active_touch_point(e, &p)) {
                int32_t dy = debug_get_physical_y(p.y) - debug_get_physical_y(debug_hide_start.y);
                if (dy <= -DEBUG_CLOSE_TRIGGER_PX) {
                    debug_hide_panel(true);
                }
            }
        }
        debug_hide_tracking = false;
        return;
    }

    if (code == LV_EVENT_PRESS_LOST) {
        debug_hide_tracking = false;
    }
}

static void create_debug_swipe_zone(void)
{
    debug_swipe_zone = lv_obj_create(lv_scr_act());
    lv_obj_set_size(debug_swipe_zone, SCREEN_WIDTH, DEBUG_OPEN_ZONE_HEIGHT);
    lv_obj_align(debug_swipe_zone, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(debug_swipe_zone, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(debug_swipe_zone, 0, 0);
    lv_obj_set_style_radius(debug_swipe_zone, 0, 0);
    lv_obj_set_style_pad_all(debug_swipe_zone, 0, 0);
    lv_obj_clear_flag(debug_swipe_zone, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(debug_swipe_zone, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(debug_swipe_zone, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_add_event_cb(debug_swipe_zone, debug_swipe_zone_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_move_foreground(debug_swipe_zone);
}

static void create_debug_close_zone(void)
{
    debug_close_zone = lv_obj_create(lv_scr_act());
    lv_obj_set_size(debug_close_zone, SCREEN_WIDTH, DEBUG_CLOSE_ZONE_HEIGHT);
    lv_obj_align(debug_close_zone, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(debug_close_zone, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(debug_close_zone, 0, 0);
    lv_obj_set_style_radius(debug_close_zone, 0, 0);
    lv_obj_set_style_pad_all(debug_close_zone, 0, 0);
    lv_obj_clear_flag(debug_close_zone, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(debug_close_zone, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(debug_close_zone, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_add_event_cb(debug_close_zone, debug_close_zone_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_flag(debug_close_zone, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(debug_close_zone);
}

typedef struct {
    char line[256];
} debug_log_entry_t;

static QueueHandle_t get_debug_log_queue(void)
{
    if (debug_log_queue) return debug_log_queue;
    debug_log_queue = xQueueCreate((UBaseType_t)DEBUG_LOG_QUEUE_DEPTH, sizeof(debug_log_entry_t));
    return debug_log_queue;
}

bool ui_debug_log_capture_enabled(void)
{
#if UI_DEBUG_SCREEN_ENABLED
    return debug_screen_visible;
#else
    return false;
#endif
}

bool ui_debug_overlay_animating(void)
{
#if UI_DEBUG_SCREEN_ENABLED
    return debug_animation_active;
#else
    return false;
#endif
}

static void debug_log_queue_clear(void)
{
    QueueHandle_t q = get_debug_log_queue();
    if (!q) return;

    debug_log_entry_t dropped;
    while (xQueueReceive(q, &dropped, 0) == pdTRUE) {
        // drop pending logs
    }
}

static void debug_log_queue_push_line(const char* line)
{
    if (!line || line[0] == '\0') return;

    QueueHandle_t q = get_debug_log_queue();
    if (!q) return;

    debug_log_entry_t entry;
    strncpy(entry.line, line, sizeof(entry.line) - 1);
    entry.line[sizeof(entry.line) - 1] = '\0';

    if (xQueueSend(q, &entry, 0) != pdTRUE) {
        debug_log_entry_t dropped;
        (void)xQueueReceive(q, &dropped, 0);
        (void)xQueueSend(q, &entry, 0);
    }
}

void ui_debug_log_enqueue(const char* line)
{
#if !UI_DEBUG_SCREEN_ENABLED
    LV_UNUSED(line);
    return;
#else
    if (!line || line[0] == '\0') return;
    if (!ui_debug_log_capture_enabled()) return;
    uint16_t dropped_report = 0;
    bool allow = debug_log_budget_take(&dropped_report);
    if (dropped_report > 0) {
        char notice[80];
        snprintf(notice, sizeof(notice), "[DBG] dropped %u lines", (unsigned int)dropped_report);
        debug_log_queue_push_line(notice);
    }
    if (!allow) return;
    debug_log_queue_push_line(line);
#endif
}

static void debug_log_flush_timer_cb(lv_timer_t * t)
{
    LV_UNUSED(t);
    if (!debug_log_ta || !debug_screen_visible) return;

    QueueHandle_t q = get_debug_log_queue();
    if (!q) return;

    debug_log_entry_t entry;
    char batch[1024];
    size_t used = 0;
    uint32_t drained = 0;

    batch[0] = '\0';
    while (drained < DEBUG_LOG_FLUSH_BATCH && xQueueReceive(q, &entry, 0) == pdTRUE) {
        int w = snprintf(batch + used, sizeof(batch) - used, "%s\n", entry.line);
        if (w <= 0) break;
        if ((size_t)w >= (sizeof(batch) - used)) break;
        used += (size_t)w;
        drained++;
    }

    if (drained > 0 && used > 0) {
        const char * cur = lv_textarea_get_text(debug_log_ta);
        size_t cur_len = cur ? strlen(cur) : 0;
        if ((cur_len + used) > DEBUG_LOG_TEXT_MAX_CHARS) {
            lv_textarea_set_text(debug_log_ta, "");
        }
        lv_textarea_add_text(debug_log_ta, batch);
        lv_textarea_set_cursor_pos(debug_log_ta, LV_TEXTAREA_CURSOR_LAST);
    }
}

static uint16_t ease_smoothstep_q15(uint16_t t_q15)
{
    // smoothstep(t) = t*t*(3 - 2*t), t in [0,1], fixed-point Q15
    uint32_t t = (uint32_t)t_q15;
    uint32_t t2 = (uint32_t)(((uint64_t)t * (uint64_t)t) >> 15);
    uint32_t three_minus_two_t = (3U << 15) - (2U * t);
    uint32_t out = (uint32_t)(((uint64_t)t2 * (uint64_t)three_minus_two_t) >> 15);
    if (out > 32768U) out = 32768U;
    return (uint16_t)out;
}

static void needle_anim_exec_cb(void * var, int32_t v)
{
    lv_img_set_angle((lv_obj_t *)var, (int16_t)(v % 3600));
}

static int32_t needle_abs_delta(int32_t a, int32_t b)
{
    int32_t delta = a - b;
    return (delta < 0) ? -delta : delta;
}

static bool label_text_matches(lv_obj_t * label, const char * text)
{
    if (!label || !text) return false;
    const char * current = lv_label_get_text(label);
    return current && strcmp(current, text) == 0;
}

static void format_percent_text(char * out, size_t out_len, int32_t value)
{
    if (!out || out_len == 0) return;
    snprintf(out, out_len, "%d%%", (int)value);
}

static int32_t needle_shortest_angle_delta(int32_t start_angle, int32_t target_angle)
{
    int32_t diff = (target_angle % 3600) - (start_angle % 3600);
    if (diff > 1800) diff -= 3600;
    else if (diff < -1800) diff += 3600;
    return diff;
}

static void coolant_needle_anim_ready_cb(lv_anim_t * a)
{
    LV_UNUSED(a);
    coolant_needle_animating = false;

    if (!coolant_pending_valid) return;

    int32_t next = coolant_pending_value;
    coolant_pending_valid = false;
    apply_coolant_gauge_value(next, false);
}

static void battery_needle_anim_ready_cb(lv_anim_t * a)
{
    LV_UNUSED(a);
    battery_needle_animating = false;

    if (!battery_pending_valid) return;

    int32_t next = battery_pending_value;
    battery_pending_valid = false;
    apply_battery_gauge_value(next, false);
}

static bool start_needle_runtime_anim(
    lv_obj_t * needle_obj,
    int32_t target_angle,
    lv_anim_ready_cb_t ready_cb
)
{
    if (!needle_obj) return false;

    int32_t start_angle = lv_img_get_angle(needle_obj);
    int32_t diff = needle_shortest_angle_delta(start_angle, target_angle);
    if (diff == 0) {
        lv_img_set_angle(needle_obj, (int16_t)(target_angle % 3600));
        return false;
    }

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, needle_obj);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)needle_anim_exec_cb);
    lv_anim_set_ready_cb(&a, ready_cb);
    lv_anim_set_values(&a, start_angle, start_angle + diff);
    lv_anim_set_time(&a, NEEDLE_RUNTIME_ANIM_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
    return true;
}

static void set_monitor_item_instant(monitor_item_t * item, int32_t usage)
{
    if (!item || !item->bar || !item->label_val) return;
    if (usage < 0) usage = 0;
    if (usage > 100) usage = 100;

    char text[8];
    format_percent_text(text, sizeof(text), usage);
    bool barSame = (lv_bar_get_value(item->bar) == usage);
    bool labelSame = label_text_matches(item->label_val, text);
    if (barSame && labelSame) return;

    if (!barSame) {
        lv_bar_set_value(item->bar, usage, LV_ANIM_OFF);
    }
    if (!labelSame) {
        lv_label_set_text(item->label_val, text);
    }
}

static void monitor_ceremony_timer_cb(lv_timer_t * t)
{
    LV_UNUSED(t);
    const uint32_t phase_ms = 450;
    const uint32_t total_ms = phase_ms * 2U;
    uint32_t elapsed = lv_tick_elaps(monitor_ceremony_start_ms);

    int32_t value = 0;
    if (elapsed < phase_ms) {
        value = (int32_t)((elapsed * 100U) / phase_ms);
    } else if (elapsed < total_ms) {
        value = 100 - (int32_t)(((elapsed - phase_ms) * 100U) / phase_ms);
    } else {
        monitor_ceremony_active = false;
        if (monitor_ceremony_timer) {
            lv_timer_del(monitor_ceremony_timer);
            monitor_ceremony_timer = NULL;
        }
        update_monitor_ui(&ram_usage, pending_ram_percent);
        update_monitor_ui(&cpu_core1, pending_core1_percent);
        update_monitor_ui(&cpu_core2, pending_core2_percent);
        return;
    }

    set_monitor_item_instant(&ram_usage, value);
    set_monitor_item_instant(&cpu_core1, value);
    set_monitor_item_instant(&cpu_core2, value);
}

static void start_monitor_ceremony(void)
{
    if (monitor_ceremony_timer) {
        lv_timer_del(monitor_ceremony_timer);
        monitor_ceremony_timer = NULL;
    }

    monitor_ceremony_active = true;
    monitor_ceremony_start_ms = lv_tick_get();

    set_monitor_item_instant(&ram_usage, 0);
    set_monitor_item_instant(&cpu_core1, 0);
    set_monitor_item_instant(&cpu_core2, 0);

    monitor_ceremony_timer = lv_timer_create(monitor_ceremony_timer_cb, 16, NULL);
}

static int32_t clamp_coolant(int32_t val)
{
    if (val < 0) return 0;
    if (val > 150) return 150;
    return val;
}

static int32_t clamp_battery(int32_t val)
{
    if (val < 0) return 0;
    if (val > 20) return 20;
    return val;
}

static int32_t coolant_to_angle(int32_t val)
{
    return GAUGE_START_ANGLE + (clamp_coolant(val) * GAUGE_MOVE_RANGE / 150) + GAUGE_MIN_ROT;
}

static int32_t battery_to_angle(int32_t val)
{
    return GAUGE_START_ANGLE + (clamp_battery(val) * GAUGE_MOVE_RANGE / 20) + GAUGE_MIN_ROT;
}

static void set_icon_rendered_visible(lv_obj_t * icon, bool * rendered_visible, bool visible)
{
    if (!icon || !rendered_visible) return;
    if (*rendered_visible == visible) return;

    if (visible) lv_obj_clear_flag(icon, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(icon, LV_OBJ_FLAG_HIDDEN);
    *rendered_visible = visible;
}

static bool get_latched_frost_visible(void)
{
    return outside_temp_rendered_valid && outside_temp_rendered_value < 10;
}

static void update_coolant_warning_icons(bool valid, int32_t coolant_val)
{
    if (needle_ceremony_warning_icons_active) {
        set_icon_rendered_visible(icon_cold, &cold_icon_rendered_visible, true);
    } else if (!valid) {
        set_icon_rendered_visible(icon_cold, &cold_icon_rendered_visible, false);
    }

    if (!valid) {
        if (!startup_warning_icon_sequence_active) {
            set_icon_rendered_visible(icon_overheat, &overheat_icon_rendered_visible, false);
        }
        return;
    }

    coolant_val = clamp_coolant(coolant_val);
    if (!needle_ceremony_warning_icons_active) {
        set_icon_rendered_visible(
            icon_cold,
            &cold_icon_rendered_visible,
            coolant_val <= COOLANT_COLD_THRESHOLD_C
        );
    }
    if (!startup_warning_icon_sequence_active) {
        set_icon_rendered_visible(
            icon_overheat,
            &overheat_icon_rendered_visible,
            coolant_val >= COOLANT_OVERHEAT_THRESHOLD_C
        );
    }
}

static void set_needle_ceremony_warning_icons(bool visible)
{
    needle_ceremony_warning_icons_active = visible;
    set_icon_rendered_visible(icon_cold, &cold_icon_rendered_visible, visible);
}

static void apply_latched_warning_icon_states(void)
{
    set_icon_rendered_visible(icon_frost, &frost_icon_rendered_visible, get_latched_frost_visible());
    set_icon_rendered_visible(icon_service, &service_icon_rendered_visible, service_icon_requested_visible);
    update_coolant_warning_icons(
        obd_connected_latched && coolant_valid_latched,
        coolant_value_latched
    );
}

static void set_startup_warning_icon_sequence_phase(uint8_t phase)
{
    set_icon_rendered_visible(icon_frost, &frost_icon_rendered_visible, phase == 0);
    set_icon_rendered_visible(icon_service, &service_icon_rendered_visible, phase == 1);
    set_icon_rendered_visible(icon_overheat, &overheat_icon_rendered_visible, phase == 2);
}

static void finish_startup_warning_icon_sequence(void)
{
    if (startup_warning_icon_timer) {
        lv_timer_del(startup_warning_icon_timer);
        startup_warning_icon_timer = NULL;
    }

    startup_warning_icon_sequence_active = false;
    startup_warning_icon_sequence_phase = 0;
    set_icon_rendered_visible(icon_frost, &frost_icon_rendered_visible, false);
    set_icon_rendered_visible(icon_service, &service_icon_rendered_visible, false);
    set_icon_rendered_visible(icon_overheat, &overheat_icon_rendered_visible, false);
    apply_latched_warning_icon_states();
}

static void startup_warning_icon_sequence_timer_cb(lv_timer_t * t)
{
    LV_UNUSED(t);
    if (!startup_warning_icon_sequence_active) {
        finish_startup_warning_icon_sequence();
        return;
    }

    startup_warning_icon_sequence_phase++;
    if (startup_warning_icon_sequence_phase >= 3) {
        finish_startup_warning_icon_sequence();
        return;
    }

    set_startup_warning_icon_sequence_phase(startup_warning_icon_sequence_phase);
}

static void start_startup_warning_icon_sequence(void)
{
    if (startup_warning_icon_timer) {
        lv_timer_del(startup_warning_icon_timer);
        startup_warning_icon_timer = NULL;
    }

    startup_warning_icon_sequence_active = true;
    startup_warning_icon_sequence_phase = 0;
    set_startup_warning_icon_sequence_phase(startup_warning_icon_sequence_phase);

    startup_warning_icon_timer = lv_timer_create(
        startup_warning_icon_sequence_timer_cb,
        STARTUP_WARNING_ICON_STEP_MS,
        NULL
    );
    if (!startup_warning_icon_timer) {
        finish_startup_warning_icon_sequence();
    }
}

void update_coolant_gauge(int32_t val)
{
    apply_coolant_gauge_value(val, false);
}

void update_battery_gauge(int32_t val)
{
    apply_battery_gauge_value(val, false);
}

static void set_coolant_gauge_instant(int32_t val)
{
    if (!target_needle) return;
    val = clamp_coolant(val);
    int16_t target_angle = (int16_t)(coolant_to_angle(val) % 3600);
    if (coolant_rendered_value == val &&
        !coolant_needle_animating &&
        !coolant_pending_valid &&
        lv_img_get_angle(target_needle) == (uint16_t)target_angle) {
        return;
    }
    coolant_needle_animating = false;
    coolant_pending_valid = false;
    coolant_rendered_value = val;
    lv_anim_del(target_needle, (lv_anim_exec_xcb_t)needle_anim_exec_cb);
    lv_img_set_angle(target_needle, target_angle);
}

static void set_battery_gauge_instant(int32_t val)
{
    if (!target_batt_needle) return;
    val = clamp_battery(val);
    int16_t target_angle = (int16_t)(battery_to_angle(val) % 3600);
    if (battery_rendered_value == val &&
        !battery_needle_animating &&
        !battery_pending_valid &&
        lv_img_get_angle(target_batt_needle) == (uint16_t)target_angle) {
        return;
    }
    battery_needle_animating = false;
    battery_pending_valid = false;
    battery_rendered_value = val;
    lv_anim_del(target_batt_needle, (lv_anim_exec_xcb_t)needle_anim_exec_cb);
    lv_img_set_angle(target_batt_needle, target_angle);
}

static void apply_coolant_gauge_value(int32_t val, bool force_anim)
{
    if (!target_needle) return;

    val = clamp_coolant(val);
    if (coolant_rendered_value >= 0) {
        int32_t basis = coolant_pending_valid ? coolant_pending_value : coolant_rendered_value;
        int32_t delta = needle_abs_delta(val, basis);
        if (delta == 0) return;
        if (coolant_needle_animating) {
            if (delta <= COOLANT_NEEDLE_IGNORE_DELTA) return;
            coolant_pending_value = val;
            coolant_pending_valid = true;
            return;
        }
        if (!force_anim) {
            if (delta <= COOLANT_NEEDLE_IGNORE_DELTA) return;
            if (delta <= COOLANT_NEEDLE_INSTANT_DELTA) {
                set_coolant_gauge_instant(val);
                return;
            }
        }
    }

    coolant_pending_valid = false;
    coolant_rendered_value = val;
    coolant_needle_animating = start_needle_runtime_anim(
        target_needle,
        coolant_to_angle(val),
        coolant_needle_anim_ready_cb
    );
    if (!coolant_needle_animating) {
        lv_img_set_angle(target_needle, (int16_t)(coolant_to_angle(val) % 3600));
    }
}

static void apply_battery_gauge_value(int32_t val, bool force_anim)
{
    if (!target_batt_needle) return;

    val = clamp_battery(val);
    if (battery_rendered_value >= 0) {
        int32_t basis = battery_pending_valid ? battery_pending_value : battery_rendered_value;
        int32_t delta = needle_abs_delta(val, basis);
        if (delta == 0) return;
        if (battery_needle_animating) {
            if (delta <= BATTERY_NEEDLE_INSTANT_DELTA) return;
            battery_pending_value = val;
            battery_pending_valid = true;
            return;
        }
        if (!force_anim && delta <= BATTERY_NEEDLE_INSTANT_DELTA) {
            set_battery_gauge_instant(val);
            return;
        }
    }

    battery_pending_valid = false;
    battery_rendered_value = val;
    battery_needle_animating = start_needle_runtime_anim(
        target_batt_needle,
        battery_to_angle(val),
        battery_needle_anim_ready_cb
    );
    if (!battery_needle_animating) {
        lv_img_set_angle(target_batt_needle, (int16_t)(battery_to_angle(val) % 3600));
    }
}

static void apply_latched_obd_needles(void)
{
    if (coolant_valid_latched) apply_coolant_gauge_value(coolant_value_latched, true);
    else set_coolant_gauge_instant(0);

    if (battery_valid_latched) apply_battery_gauge_value(battery_value_latched, true);
    else set_battery_gauge_instant(0);
}

static void needle_ceremony_timer_cb(lv_timer_t * t)
{
    LV_UNUSED(t);

    const uint32_t phase_ms = 700;
    const uint32_t hold_ms = 500;
    const uint32_t total_ms = (phase_ms * 2U) + hold_ms;
    uint32_t elapsed = lv_tick_elaps(needle_ceremony_start_ms);

    if (elapsed >= total_ms) {
        needle_ceremony_active = false;
        if (needle_ceremony_timer) {
            lv_timer_del(needle_ceremony_timer);
            needle_ceremony_timer = NULL;
        }
        set_coolant_gauge_instant(0);
        set_battery_gauge_instant(0);
        set_needle_ceremony_warning_icons(false);
        apply_latched_obd_needles();
        return;
    }

    int32_t coolant_val = 0;
    int32_t battery_val = 0;
    if (elapsed < phase_ms) {
        uint16_t t_q15 = (uint16_t)((elapsed * 32768U) / phase_ms);
        uint16_t e_q15 = ease_smoothstep_q15(t_q15);
        coolant_val = (int32_t)(((uint32_t)e_q15 * 150U) >> 15);
        battery_val = (int32_t)(((uint32_t)e_q15 * 20U) >> 15);
    } else if (elapsed < (phase_ms + hold_ms)) {
        coolant_val = 150;
        battery_val = 20;
    } else {
        uint32_t down_elapsed = elapsed - (phase_ms + hold_ms);
        uint16_t t_q15 = (uint16_t)((down_elapsed * 32768U) / phase_ms);
        uint16_t e_q15 = ease_smoothstep_q15(t_q15);
        coolant_val = 150 - (int32_t)(((uint32_t)e_q15 * 150U) >> 15);
        battery_val = 20 - (int32_t)(((uint32_t)e_q15 * 20U) >> 15);
    }

    set_coolant_gauge_instant(coolant_val);
    set_battery_gauge_instant(battery_val);
}

static void start_needle_ceremony(void)
{
    if (needle_ceremony_timer) {
        lv_timer_del(needle_ceremony_timer);
        needle_ceremony_timer = NULL;
    }

    needle_ceremony_pending = false;
    needle_ceremony_active = true;
    needle_ceremony_start_ms = lv_tick_get();
    coolant_needle_animating = false;
    battery_needle_animating = false;
    coolant_pending_valid = false;
    battery_pending_valid = false;
    set_coolant_gauge_instant(0);
    set_battery_gauge_instant(0);
    set_needle_ceremony_warning_icons(true);
    needle_ceremony_timer = lv_timer_create(needle_ceremony_timer_cb, 8, NULL);
    if (!needle_ceremony_timer) {
        needle_ceremony_active = false;
        set_needle_ceremony_warning_icons(false);
        apply_latched_obd_needles();
    }
}

void update_obd_gauges(bool obd_connected, bool coolant_valid, int32_t coolant_val, bool battery_valid, int32_t battery_val)
{
    obd_connected_latched = obd_connected;

    if (coolant_valid) {
        coolant_valid_latched = true;
        coolant_value_latched = clamp_coolant(coolant_val);
    }
    update_coolant_warning_icons(obd_connected && coolant_valid, coolant_val);

    if (battery_valid) {
        battery_valid_latched = true;
        battery_value_latched = clamp_battery(battery_val);
    }

    if (needle_ceremony_pending || needle_ceremony_active) return;

    if (!obd_connected_latched && !coolant_valid_latched && !battery_valid_latched) {
        set_coolant_gauge_instant(0);
        set_battery_gauge_instant(0);
        return;
    }

    apply_latched_obd_needles();
}

void create_outside_temp()
{
    lv_obj_t * temp_cont = lv_obj_create(lv_scr_act());
    lv_obj_set_size(temp_cont, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(temp_cont, LV_ALIGN_TOP_LEFT, 30, 10);
    lv_obj_set_style_bg_opa(temp_cont, 0, 0);
    lv_obj_set_style_border_width(temp_cont, 0, 0);
    lv_obj_set_style_pad_all(temp_cont, 0, 0);
    lv_obj_clear_flag(temp_cont, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_set_flex_flow(temp_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(temp_cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(temp_cont, 10, 0);

    lv_obj_t * label_outside = lv_label_create(temp_cont);
    lv_label_set_text(label_outside, "Outside");
    lv_obj_set_style_text_color(label_outside, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_style_text_font(label_outside, &lv_font_montserrat_24, 0);

    outside_temp_label = lv_label_create(temp_cont);
    lv_label_set_text(outside_temp_label, "--\xC2\xB0" "C");
    lv_obj_set_style_text_color(outside_temp_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(outside_temp_label, &lv_font_montserrat_36, 0);
}

void create_clock()
{
    clock_label = lv_label_create(lv_scr_act());
    lv_label_set_text(clock_label, "--:--");
    lv_obj_set_style_text_color(clock_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(clock_label, &lv_font_montserrat_36, 0);
    lv_obj_align(clock_label, LV_ALIGN_TOP_RIGHT, -30, 10);
    lv_obj_clear_flag(clock_label, LV_OBJ_FLAG_SCROLLABLE);
}

void create_gauge_set(lv_obj_t* parent, bool is_coolant, int32_t x, int32_t y)
{
    const lv_img_dsc_t* bg_dsc = is_coolant ? &coolantGauge : &batteryGauge;

    // Gauge-local container to constrain redraw/invalidation area
    lv_obj_t* gauge_box = lv_obj_create(parent);
    lv_obj_set_size(gauge_box, bg_dsc->header.w, bg_dsc->header.h);
    lv_obj_set_pos(gauge_box, x, y);
    lv_obj_set_style_bg_opa(gauge_box, 0, 0);
    lv_obj_set_style_border_width(gauge_box, 0, 0);
    lv_obj_set_style_pad_all(gauge_box, 0, 0);
    lv_obj_clear_flag(gauge_box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* bg = lv_img_create(gauge_box);
    lv_img_set_src(bg, bg_dsc);
    lv_obj_clear_flag(bg, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(bg, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_pos(bg, 0, 0);

    lv_obj_t* needle_obj = lv_img_create(gauge_box);
    lv_img_set_src(needle_obj, &needle);
    lv_obj_set_style_img_recolor_opa(needle_obj, 0, 0);

    lv_obj_update_layout(bg);
    lv_obj_align_to(needle_obj, bg, LV_ALIGN_CENTER, 0, 0);

    lv_img_set_pivot(needle_obj, GAUGE_PIVOT_X, GAUGE_PIVOT_Y);

    lv_obj_add_flag(needle_obj, LV_OBJ_FLAG_IGNORE_LAYOUT);

    if(is_coolant) {
        target_needle = needle_obj;
        set_coolant_gauge_instant(0);
    } else {
        target_batt_needle = needle_obj;
        set_battery_gauge_instant(0);
    }
}

lv_obj_t* create_icon(lv_obj_t* parent, const void* src, int32_t x, int32_t y, uint16_t zoom)
{
    lv_obj_t* img = lv_img_create(parent);
    lv_img_set_src(img, src);
    lv_img_set_pivot(img, 0, 0);
    lv_img_set_zoom(img, zoom);
    lv_obj_set_pos(img, x, y);
    return img;
}

static void close_oil_popup_overlay(lv_obj_t * overlay)
{
    if (!overlay) return;
    if (overlay == oil_popup_overlay) {
        oil_popup_overlay = NULL;
        oil_popup_replace_mode = false;
        reset_oil_popup_content_refs();
    }
    lv_obj_del_async(overlay);
}

static void reset_oil_popup_content_refs(void)
{
    oil_cycle_input_label = NULL;
}

static void oil_popup_yes_button_event_cb(lv_event_t * e)
{
    lv_obj_t * overlay = (lv_obj_t *)lv_event_get_user_data(e);
    if (!overlay) return;

    if (oil_popup_replace_mode) {
        lv_obj_t * popup = lv_obj_get_child(overlay, 0);
        if (popup) show_oil_popup_edit_content(popup);
        return;
    }

    bool cycle_reset = ui_reset_service_oil_cycle_km();
    bool odo_reset = ui_reset_service_odo();
    if (cycle_reset && odo_reset) {
        update_service_icon(false);
        update_oil_percent(100);
    }
    close_oil_popup_overlay(overlay);
}

static void oil_popup_no_button_event_cb(lv_event_t * e)
{
    lv_obj_t * overlay = (lv_obj_t *)lv_event_get_user_data(e);
    close_oil_popup_overlay(overlay);
}

static void apply_oil_popup_button_style(lv_obj_t * button, int32_t width, int32_t height, lv_color_t bg_color, lv_color_t border_color)
{
    static bool transition_ready = false;
    static lv_style_transition_dsc_t press_transition;
    static const lv_style_prop_t press_props[] = {
        LV_STYLE_TRANSFORM_ZOOM,
        LV_STYLE_BG_COLOR,
        0
    };

    if (!transition_ready) {
        lv_style_transition_dsc_init(
            &press_transition,
            press_props,
            lv_anim_path_ease_out,
            90,
            0,
            NULL
        );
        transition_ready = true;
    }

    lv_color_t pressed_bg = lv_color_lighten(bg_color, LV_OPA_20);
    lv_obj_set_style_bg_color(button, bg_color, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(button, pressed_bg, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(button, 1, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(button, 1, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(button, border_color, LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(button, border_color, LV_STATE_PRESSED);
    lv_obj_set_style_radius(button, 9, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_outline_width(button, 0, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_set_style_transform_pivot_x(button, width / 2, 0);
    lv_obj_set_style_transform_pivot_y(button, height / 2, 0);
    lv_obj_set_style_transform_zoom(button, 256, LV_STATE_DEFAULT);
    lv_obj_set_style_transform_zoom(button, OIL_POPUP_PRESS_ZOOM, LV_STATE_PRESSED);
    lv_obj_set_style_transition(button, &press_transition, LV_STATE_DEFAULT);
    lv_obj_set_style_transition(button, &press_transition, LV_STATE_PRESSED);
    lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t * create_oil_popup_button_base(lv_obj_t * parent, int32_t width, int32_t height, lv_color_t bg_color, lv_color_t border_color)
{
    lv_obj_t * button = lv_btn_create(parent);
    lv_obj_remove_style_all(button);
    lv_obj_set_size(button, width, height);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    apply_oil_popup_button_style(button, width, height, bg_color, border_color);
    return button;
}

static lv_obj_t * create_oil_popup_image_button(lv_obj_t * parent, const void * label_src, lv_color_t bg_color, lv_color_t border_color)
{
    lv_obj_t * button = create_oil_popup_button_base(parent, OIL_POPUP_BUTTON_WIDTH, OIL_POPUP_BUTTON_HEIGHT, bg_color, border_color);

    lv_obj_t * label_img = lv_img_create(button);
    lv_img_set_src(label_img, label_src);
    lv_obj_center(label_img);

    return button;
}

static lv_obj_t * create_oil_popup_text_button(lv_obj_t * parent, const char * text, int32_t width, int32_t height, lv_color_t bg_color, lv_color_t border_color, const lv_font_t * font)
{
    lv_obj_t * button = create_oil_popup_button_base(parent, width, height, bg_color, border_color);
    lv_obj_t * label = lv_label_create(button);
    lv_label_set_text(label, text ? text : "");
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, font ? font : &lv_font_montserrat_24, 0);
    lv_obj_center(label);
    return button;
}

static int32_t oil_popup_img_scaled_w(const lv_img_dsc_t * src, int32_t zoom)
{
    return src ? ((int32_t)src->header.w * zoom) / 256 : 0;
}

static int32_t oil_popup_img_scaled_h(const lv_img_dsc_t * src, int32_t zoom)
{
    return src ? ((int32_t)src->header.h * zoom) / 256 : 0;
}

static void place_oil_popup_replace_prompt(lv_obj_t * prompt, const lv_img_dsc_t * src)
{
    if (!prompt || !src) return;

    int32_t zoom = 256;
    int32_t w = oil_popup_img_scaled_w(src, zoom);
    if (w > OIL_POPUP_PROMPT_MAX_WIDTH) {
        zoom = (OIL_POPUP_PROMPT_MAX_WIDTH * 256) / (int32_t)src->header.w;
        if (zoom < 1) zoom = 1;
    }

    w = oil_popup_img_scaled_w(src, zoom);
    int32_t h = oil_popup_img_scaled_h(src, zoom);
    lv_img_set_pivot(prompt, 0, 0);
    lv_img_set_zoom(prompt, (uint16_t)zoom);
    lv_obj_set_pos(
        prompt,
        (OIL_POPUP_WIDTH - w) / 2,
        ((OIL_POPUP_HEIGHT - h) / 2) - 42
    );
}

static void place_oil_popup_edit_title(lv_obj_t * title, const lv_img_dsc_t * src)
{
    if (!title || !src) return;

    if (oil_popup_replace_mode) {
        lv_img_set_pivot(title, 0, 0);
        lv_img_set_zoom(title, OIL_REPLACE_TITLE_SCALE);
        lv_obj_set_pos(title, OIL_REPLACE_TITLE_X, OIL_REPLACE_TITLE_Y);
    } else {
        lv_obj_align(title, LV_ALIGN_TOP_MID, OIL_POPUP_EDIT_DUE_X_OFFSET, OIL_POPUP_EDIT_DUE_Y);
    }
}

static void update_oil_cycle_input_label(void)
{
    if (oil_cycle_input_label) {
        lv_label_set_text(oil_cycle_input_label, oil_cycle_input_text);
    }
}

static void oil_popup_digit_button_event_cb(lv_event_t * e)
{
    const char * digit = (const char *)lv_event_get_user_data(e);
    if (!digit || digit[0] == '\0') return;

    size_t len = strlen(oil_cycle_input_text);
    if (oil_cycle_input_replace_on_next_digit || (len == 1 && oil_cycle_input_text[0] == '0')) {
        oil_cycle_input_text[0] = digit[0];
        oil_cycle_input_text[1] = '\0';
        oil_cycle_input_replace_on_next_digit = false;
    } else if (len < OIL_CYCLE_INPUT_MAX) {
        oil_cycle_input_text[len] = digit[0];
        oil_cycle_input_text[len + 1] = '\0';
    }

    update_oil_cycle_input_label();
}

static void oil_popup_clear_button_event_cb(lv_event_t * e)
{
    LV_UNUSED(e);
    oil_cycle_input_text[0] = '0';
    oil_cycle_input_text[1] = '\0';
    oil_cycle_input_replace_on_next_digit = true;
    update_oil_cycle_input_label();
}

static void oil_popup_ok_button_event_cb(lv_event_t * e)
{
    lv_obj_t * overlay = (lv_obj_t *)lv_event_get_user_data(e);
    uint32_t inputKm = (uint32_t)strtoul(oil_cycle_input_text, NULL, 10);
    if (oil_popup_replace_mode) {
        ui_set_service_odo_km(inputKm);
    } else {
        ui_set_service_oil_cycle_km(inputKm);
    }
    close_oil_popup_overlay(overlay);
}

static void oil_popup_edit_button_event_cb(lv_event_t * e)
{
    lv_obj_t * popup = (lv_obj_t *)lv_event_get_user_data(e);
    if (!popup) return;
    show_oil_popup_edit_content(popup);
}

static int32_t clamp_percent(int32_t percent)
{
    if (percent < 0) percent = 0;
    else if (percent > 100) percent = 100;
    return percent;
}

static void set_oil_percent_display(int32_t percent, lv_anim_enable_t anim)
{
    percent = clamp_percent(percent);

    char text[8];
    format_percent_text(text, sizeof(text), percent);
    bool labelSame = (!oil_percent_label || label_text_matches(oil_percent_label, text));
    bool barSame = (!oil_life_bar || lv_bar_get_value(oil_life_bar) == percent);
    if (oil_percent_rendered_value == percent && labelSame && barSame) {
        return;
    }

    if (oil_percent_label && !labelSame) {
        lv_label_set_text(oil_percent_label, text);
    }
    if (oil_life_bar && !barSame) {
        lv_bar_set_value(oil_life_bar, percent, anim);
    }
    oil_percent_rendered_value = percent;
}

void update_oil_percent(int32_t percent)
{
    oil_percent_requested_value = clamp_percent(percent);
    if (oil_life_ceremony_active) return;

    set_oil_percent_display(oil_percent_requested_value, LV_ANIM_OFF);
}

static void finish_oil_life_ceremony(void)
{
    if (oil_life_ceremony_timer) {
        lv_timer_del(oil_life_ceremony_timer);
        oil_life_ceremony_timer = NULL;
    }

    oil_life_ceremony_active = false;
    oil_life_ceremony_start_ms = 0;
    set_oil_percent_display(oil_percent_requested_value, LV_ANIM_ON);
}

static void oil_life_ceremony_timer_cb(lv_timer_t * t)
{
    LV_UNUSED(t);
    if (!oil_life_ceremony_active) {
        finish_oil_life_ceremony();
        return;
    }

    const uint32_t total_ms = OIL_LIFE_CEREMONY_PHASE_MS * 2U;
    uint32_t elapsed = lv_tick_elaps(oil_life_ceremony_start_ms);
    if (elapsed >= total_ms) {
        set_oil_percent_display(0, LV_ANIM_OFF);
        finish_oil_life_ceremony();
        return;
    }

    int32_t percent = 0;
    if (elapsed < OIL_LIFE_CEREMONY_PHASE_MS) {
        percent = (int32_t)((elapsed * 100U) / OIL_LIFE_CEREMONY_PHASE_MS);
    } else {
        uint32_t down_elapsed = elapsed - OIL_LIFE_CEREMONY_PHASE_MS;
        percent = 100 - (int32_t)((down_elapsed * 100U) / OIL_LIFE_CEREMONY_PHASE_MS);
    }
    set_oil_percent_display(percent, LV_ANIM_OFF);
}

static void start_oil_life_ceremony(void)
{
    if (oil_life_ceremony_timer) {
        lv_timer_del(oil_life_ceremony_timer);
        oil_life_ceremony_timer = NULL;
    }

    oil_life_ceremony_active = true;
    oil_life_ceremony_start_ms = lv_tick_get();
    set_oil_percent_display(0, LV_ANIM_OFF);

    oil_life_ceremony_timer = lv_timer_create(
        oil_life_ceremony_timer_cb,
        OIL_LIFE_CEREMONY_TIMER_MS,
        NULL
    );
    if (!oil_life_ceremony_timer) {
        finish_oil_life_ceremony();
    }
}

static void show_oil_popup_confirm_content(lv_obj_t * popup)
{
    if (!popup) return;
    reset_oil_popup_content_refs();
    lv_obj_clean(popup);

    lv_obj_t * prompt = lv_img_create(popup);
    const lv_img_dsc_t * prompt_src = oil_popup_replace_mode ? &oil_edit : &oil_reset_prompt;
    lv_img_set_src(prompt, prompt_src);
    if (oil_popup_replace_mode) {
        place_oil_popup_replace_prompt(prompt, prompt_src);
    } else {
        lv_obj_align(prompt, LV_ALIGN_CENTER, 0, -42);
    }

    lv_obj_t * button_row = lv_obj_create(popup);
    lv_obj_set_size(button_row, lv_pct(100), 90);
    lv_obj_align(button_row, LV_ALIGN_BOTTOM_MID, 0, -24);
    lv_obj_set_style_bg_opa(button_row, 0, 0);
    lv_obj_set_style_border_width(button_row, 0, 0);
    lv_obj_set_style_pad_all(button_row, 0, 0);
    lv_obj_set_style_pad_column(button_row, OIL_POPUP_BUTTON_GAP, 0);
    lv_obj_set_flex_flow(button_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(button_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(button_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * yes_button = create_oil_popup_image_button(
        button_row,
        &oil_popup_yes,
        lv_color_hex(0x1C84C6),
        lv_color_hex(0x64C9FF)
    );
    lv_obj_add_event_cb(yes_button, oil_popup_yes_button_event_cb, LV_EVENT_CLICKED, oil_popup_overlay);

    lv_obj_t * no_button = create_oil_popup_image_button(
        button_row,
        &oil_popup_no,
        lv_color_hex(0x2B2B2B),
        lv_color_hex(0x737373)
    );
    lv_obj_add_event_cb(no_button, oil_popup_no_button_event_cb, LV_EVENT_CLICKED, oil_popup_overlay);

    lv_obj_t * edit_button = create_oil_popup_image_button(
        button_row,
        &oil_popup_edit,
        lv_color_hex(0x2B2B2B),
        lv_color_hex(0x737373)
    );
    lv_obj_add_event_cb(edit_button, oil_popup_edit_button_event_cb, LV_EVENT_CLICKED, popup);
}

static void show_oil_popup_edit_content(lv_obj_t * popup)
{
    if (!popup) return;
    reset_oil_popup_content_refs();
    lv_obj_clean(popup);

    uint32_t inputKm = OIL_CYCLE_DEFAULT_KM;
    if (oil_popup_replace_mode) {
        if (!ui_get_service_odo_km(&inputKm)) {
            inputKm = 0;
        }
    } else {
        ui_get_service_oil_cycle_km(&inputKm);
    }
    snprintf(oil_cycle_input_text, sizeof(oil_cycle_input_text), "%lu", (unsigned long)inputKm);
    oil_cycle_input_replace_on_next_digit = true;

    lv_obj_t * due_img = lv_img_create(popup);
    const lv_img_dsc_t * title_src = oil_popup_replace_mode ? &oil_replace : &oil_popup_due;
    lv_img_set_src(due_img, title_src);
    place_oil_popup_edit_title(due_img, title_src);

    lv_obj_t * close_button = create_oil_popup_text_button(
        popup,
        "X",
        OIL_POPUP_EDIT_CLOSE_WIDTH,
        OIL_POPUP_EDIT_CLOSE_HEIGHT,
        lv_color_hex(0x2B2B2B),
        lv_color_hex(0x737373),
        &lv_font_montserrat_28
    );
    lv_obj_align(close_button, LV_ALIGN_TOP_MID, OIL_POPUP_EDIT_CLOSE_X_OFFSET, OIL_POPUP_EDIT_CLOSE_Y);
    lv_obj_add_event_cb(close_button, oil_popup_no_button_event_cb, LV_EVENT_CLICKED, oil_popup_overlay);

    lv_obj_t * input_row = lv_obj_create(popup);
    lv_obj_set_size(input_row, OIL_POPUP_EDIT_INPUT_ROW_WIDTH, OIL_POPUP_EDIT_INPUT_ROW_HEIGHT);
    lv_obj_align(input_row, LV_ALIGN_TOP_MID, 0, OIL_POPUP_EDIT_INPUT_ROW_Y);
    lv_obj_set_style_bg_opa(input_row, 0, 0);
    lv_obj_set_style_border_width(input_row, 0, 0);
    lv_obj_set_style_pad_all(input_row, 0, 0);
    lv_obj_set_style_pad_column(input_row, OIL_POPUP_EDIT_INPUT_GAP, 0);
    lv_obj_set_flex_flow(input_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(input_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(input_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * input_box = lv_obj_create(input_row);
    lv_obj_remove_style_all(input_box);
    lv_obj_set_size(input_box, OIL_POPUP_EDIT_INPUT_WIDTH, OIL_POPUP_EDIT_INPUT_ROW_HEIGHT);
    lv_obj_set_style_bg_color(input_box, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(input_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(input_box, 1, 0);
    lv_obj_set_style_border_color(input_box, lv_color_hex(0x737373), 0);
    lv_obj_set_style_radius(input_box, 8, 0);
    lv_obj_set_style_pad_all(input_box, 0, 0);
    lv_obj_clear_flag(input_box, LV_OBJ_FLAG_SCROLLABLE);

    oil_cycle_input_label = lv_label_create(input_box);
    lv_label_set_text(oil_cycle_input_label, oil_cycle_input_text);
    lv_obj_set_style_text_color(oil_cycle_input_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(oil_cycle_input_label, &lv_font_montserrat_28, 0);
    lv_obj_center(oil_cycle_input_label);

    lv_obj_t * ok_button = create_oil_popup_text_button(
        input_row,
        "OK",
        OIL_POPUP_EDIT_OK_WIDTH,
        OIL_POPUP_EDIT_INPUT_ROW_HEIGHT,
        lv_color_hex(0x1C84C6),
        lv_color_hex(0x64C9FF),
        &lv_font_montserrat_28
    );
    lv_obj_add_event_cb(ok_button, oil_popup_ok_button_event_cb, LV_EVENT_CLICKED, oil_popup_overlay);

    lv_obj_t * clear_button = create_oil_popup_text_button(
        input_row,
        "Clear",
        OIL_POPUP_EDIT_CLEAR_WIDTH,
        OIL_POPUP_EDIT_INPUT_ROW_HEIGHT,
        lv_color_hex(0x2B2B2B),
        lv_color_hex(0x737373),
        &lv_font_montserrat_24
    );
    lv_obj_add_event_cb(clear_button, oil_popup_clear_button_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * keypad = lv_obj_create(popup);
    lv_obj_set_size(keypad, OIL_POPUP_KEYPAD_WIDTH, OIL_POPUP_KEYPAD_HEIGHT);
    lv_obj_align(keypad, LV_ALIGN_BOTTOM_MID, 0, OIL_POPUP_KEYPAD_BOTTOM_OFFSET);
    lv_obj_set_style_bg_opa(keypad, 0, 0);
    lv_obj_set_style_border_width(keypad, 0, 0);
    lv_obj_set_style_pad_all(keypad, 0, 0);
    lv_obj_set_style_pad_row(keypad, OIL_POPUP_KEY_GAP, 0);
    lv_obj_set_style_pad_column(keypad, OIL_POPUP_KEY_GAP, 0);
    lv_obj_set_flex_flow(keypad, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(keypad, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(keypad, LV_OBJ_FLAG_SCROLLABLE);

    static const char * digits[] = { "0", "1", "2", "3", "4", "5", "6", "7", "8", "9" };
    for (size_t i = 0; i < sizeof(digits) / sizeof(digits[0]); ++i) {
        lv_obj_t * key = create_oil_popup_text_button(
            keypad,
            digits[i],
            OIL_POPUP_KEY_WIDTH,
            OIL_POPUP_KEY_HEIGHT,
            lv_color_hex(0x2B2B2B),
            lv_color_hex(0x737373),
            &lv_font_montserrat_28
        );
        lv_obj_add_event_cb(key, oil_popup_digit_button_event_cb, LV_EVENT_CLICKED, (void *)digits[i]);
    }
}

static void show_oil_popup_with_mode(bool edit_mode, bool replace_mode)
{
    if (oil_popup_overlay) return;
    oil_popup_replace_mode = replace_mode;

    oil_popup_overlay = lv_obj_create(lv_scr_act());
    lv_obj_set_size(oil_popup_overlay, SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_obj_set_style_bg_color(oil_popup_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(oil_popup_overlay, LV_OPA_60, 0);
    lv_obj_set_style_border_width(oil_popup_overlay, 0, 0);
    lv_obj_set_style_radius(oil_popup_overlay, 0, 0);
    lv_obj_set_style_pad_all(oil_popup_overlay, 0, 0);
    lv_obj_add_flag(oil_popup_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(oil_popup_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(oil_popup_overlay);

    lv_obj_t * popup = lv_obj_create(oil_popup_overlay);
    lv_obj_set_size(popup, OIL_POPUP_WIDTH, OIL_POPUP_HEIGHT);
    lv_obj_center(popup);
    lv_obj_set_style_bg_color(popup, lv_color_hex(0x161616), 0);
    lv_obj_set_style_bg_opa(popup, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(popup, 1, 0);
    lv_obj_set_style_border_color(popup, lv_color_hex(0x4A4A4A), 0);
    lv_obj_set_style_radius(popup, 8, 0);
    lv_obj_set_style_shadow_width(popup, 27, 0);
    lv_obj_set_style_shadow_opa(popup, LV_OPA_40, 0);
    lv_obj_set_style_shadow_color(popup, lv_color_black(), 0);
    lv_obj_clear_flag(popup, LV_OBJ_FLAG_SCROLLABLE);

    if (edit_mode) show_oil_popup_edit_content(popup);
    else show_oil_popup_confirm_content(popup);
}

static void show_oil_popup(void)
{
    show_oil_popup_with_mode(false, false);
}

static void show_oil_replace_popup(bool edit_mode)
{
    show_oil_popup_with_mode(edit_mode, true);
}

static void oil_touch_area_pressed_event_cb(lv_event_t * e)
{
    LV_UNUSED(e);
    oil_touch_long_pressed = false;
}

static void oil_touch_area_event_cb(lv_event_t * e)
{
    LV_UNUSED(e);
    if (!oil_touch_long_pressed) {
        show_oil_popup();
    }
}

static void oil_touch_area_long_press_event_cb(lv_event_t * e)
{
    LV_UNUSED(e);
    oil_touch_long_pressed = true;
    show_oil_replace_popup(false);
}

static void create_center_status_bar(lv_obj_t* parent)
{
    oil_percent_label = lv_label_create(parent);
    lv_obj_set_width(oil_percent_label, CENTER_STATUS_LABEL_WIDTH);
    lv_obj_set_pos(oil_percent_label, CENTER_STATUS_LABEL_X, CENTER_STATUS_LABEL_Y);
    lv_obj_set_style_text_color(oil_percent_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(oil_percent_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(oil_percent_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_clear_flag(oil_percent_label, LV_OBJ_FLAG_SCROLLABLE);
    update_oil_percent(100);

    oil_life_bar = lv_bar_create(parent);
    lv_obj_set_size(oil_life_bar, CENTER_STATUS_BAR_WIDTH, CENTER_STATUS_BAR_HEIGHT);
    lv_obj_set_pos(oil_life_bar, CENTER_STATUS_BAR_X, CENTER_STATUS_BAR_Y);
    lv_bar_set_range(oil_life_bar, 0, 100);
    lv_obj_set_style_bg_color(oil_life_bar, lv_color_hex(0x07141C), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(oil_life_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(oil_life_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(oil_life_bar, 3, LV_PART_MAIN);
    lv_obj_set_style_pad_all(oil_life_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(oil_life_bar, lv_color_hex(0x18B7FF), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_color(oil_life_bar, lv_color_hex(0x0B4D78), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_dir(oil_life_bar, LV_GRAD_DIR_VER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(oil_life_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_border_width(oil_life_bar, 0, LV_PART_INDICATOR);
    lv_obj_set_style_radius(oil_life_bar, 3, LV_PART_INDICATOR);
    lv_obj_clear_flag(oil_life_bar, LV_OBJ_FLAG_SCROLLABLE);
    set_oil_percent_display(oil_percent_requested_value, LV_ANIM_OFF);

    lv_obj_t * oil_icon = create_icon(parent, &oil, OIL_ICON_X, OIL_ICON_Y, OIL_ICON_SCALE);

    lv_obj_update_layout(parent);

    int32_t x1 = lv_obj_get_x(oil_percent_label);
    int32_t y1 = lv_obj_get_y(oil_percent_label);
    int32_t x2 = x1 + lv_obj_get_width(oil_percent_label);
    int32_t y2 = y1 + lv_obj_get_height(oil_percent_label);

    int32_t obj_x1 = lv_obj_get_x(oil_life_bar);
    int32_t obj_y1 = lv_obj_get_y(oil_life_bar);
    int32_t obj_x2 = obj_x1 + lv_obj_get_width(oil_life_bar);
    int32_t obj_y2 = obj_y1 + lv_obj_get_height(oil_life_bar);
    if (obj_x1 < x1) x1 = obj_x1;
    if (obj_y1 < y1) y1 = obj_y1;
    if (obj_x2 > x2) x2 = obj_x2;
    if (obj_y2 > y2) y2 = obj_y2;

    obj_x1 = lv_obj_get_x(oil_icon);
    obj_y1 = lv_obj_get_y(oil_icon);
    obj_x2 = obj_x1 + lv_obj_get_width(oil_icon);
    obj_y2 = obj_y1 + lv_obj_get_height(oil_icon);
    if (obj_x1 < x1) x1 = obj_x1;
    if (obj_y1 < y1) y1 = obj_y1;
    if (obj_x2 > x2) x2 = obj_x2;
    if (obj_y2 > y2) y2 = obj_y2;

    lv_obj_t * touch_area = lv_obj_create(parent);
    lv_obj_set_pos(touch_area, x1 - CENTER_STATUS_TOUCH_PADDING, y1 - CENTER_STATUS_TOUCH_PADDING);
    lv_obj_set_size(
        touch_area,
        (x2 - x1) + (CENTER_STATUS_TOUCH_PADDING * 2),
        (y2 - y1) + (CENTER_STATUS_TOUCH_PADDING * 2)
    );
    lv_obj_set_style_bg_opa(touch_area, 0, 0);
    lv_obj_set_style_border_width(touch_area, 0, 0);
    lv_obj_set_style_shadow_width(touch_area, 0, 0);
    lv_obj_set_style_radius(touch_area, 0, 0);
    lv_obj_set_style_pad_all(touch_area, 0, 0);
    lv_obj_add_flag(touch_area, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(touch_area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(touch_area, oil_touch_area_pressed_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(touch_area, oil_touch_area_event_cb, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(touch_area, oil_touch_area_long_press_event_cb, LV_EVENT_LONG_PRESSED, NULL);
}

void create_monitor_item(lv_obj_t * parent, monitor_item_t * item, const char * name, lv_color_t color, int32_t ignored)
{
    lv_obj_t * row = lv_obj_create(parent);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, 0, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);

    lv_obj_t * label_name = lv_label_create(row);
    lv_label_set_text(label_name, name);
    lv_obj_set_style_text_color(label_name, color, 0);
    lv_obj_set_width(label_name, 55);
    lv_obj_set_style_text_align(label_name, LV_TEXT_ALIGN_LEFT, 0);

    item->bar = lv_bar_create(row);
    lv_obj_set_size(item->bar, 120, 15);

    lv_obj_set_style_bg_color(item->bar, lv_color_hex(0x101010), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(item->bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(item->bar, LV_GRAD_DIR_NONE, LV_PART_MAIN);
    lv_obj_set_style_radius(item->bar, 2, LV_PART_MAIN);
    lv_obj_set_style_border_width(item->bar, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(item->bar, 0, LV_PART_MAIN);

    lv_obj_set_style_radius(item->bar, 2, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(item->bar, color, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(item->bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_dir(item->bar, LV_GRAD_DIR_NONE, LV_PART_INDICATOR);
    lv_obj_set_style_border_width(item->bar, 0, LV_PART_INDICATOR);
    lv_obj_set_style_outline_width(item->bar, 0, LV_PART_INDICATOR);

    lv_obj_set_style_bg_grad_dir(item->bar, LV_GRAD_DIR_NONE, LV_PART_INDICATOR);

    lv_obj_set_style_anim_time(item->bar, 220, 0);

    item->label_val = lv_label_create(row);
    lv_label_set_text(item->label_val, "0%");
    lv_obj_set_style_text_color(item->label_val, lv_color_white(), 0);
    lv_obj_set_width(item->label_val, 45);
    lv_obj_set_style_text_align(item->label_val, LV_TEXT_ALIGN_RIGHT, 0);
}

void create_sys_monitor_panel()
{
    lv_obj_t * panel = lv_obj_create(lv_scr_act());
    lv_obj_set_size(panel, 260, 55);
    lv_obj_align(panel, LV_ALIGN_BOTTOM_LEFT, 80, -35);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_shadow_width(panel, 0, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(panel, 10, 0);
    lv_obj_set_style_pad_ver(panel, 5, 0);

    create_monitor_item(panel, &ram_usage, "RAM", lv_color_hex(0x87CEEB), 0);
    create_monitor_item(panel, &cpu_core1, "Core 1", lv_color_hex(0x90EE90), 0);
    create_monitor_item(panel, &cpu_core2, "Core 2", lv_color_hex(0xFFFF00), 0);

    lv_obj_move_foreground(panel);
}

void create_gauge()
{
    lv_obj_t* gauge_cont = lv_obj_create(lv_scr_act());
    lv_obj_set_size(gauge_cont, LV_HOR_RES, 480);
    lv_obj_set_style_bg_opa(gauge_cont, 0, 0);
    lv_obj_set_style_border_width(gauge_cont, 0, 0);
    lv_obj_clear_flag(gauge_cont, LV_OBJ_FLAG_SCROLLABLE);

    create_gauge_set(gauge_cont, true, COOLANT_GAUGE_X_POS, COOLANT_GAUGE_Y_POS);
    create_gauge_set(gauge_cont, false, BATTERY_GAUGE_X_POS, BATTERY_GAUGE_Y_POS);
    create_center_status_bar(gauge_cont);

    icon_bt = create_icon(lv_scr_act(), &btOff, BT_X_POS, BT_Y_POS, LV_IMG_ZOOM_NONE);
    icon_obd = create_icon(lv_scr_act(), &obdOff, OBD_X_POS, OBD_Y_POS, LV_IMG_ZOOM_NONE);
    icon_wifi = create_icon(lv_scr_act(), &wifi_off, WIFI_X_POS, WIFI_Y_POS, LV_IMG_ZOOM_NONE);
    icon_frost = create_icon(lv_scr_act(), &frost, FROST_X_POS, FROST_Y_POS, LV_IMG_ZOOM_NONE);
    icon_service = create_icon(lv_scr_act(), &service, SERVICE_X_POS, SERVICE_Y_POS, SERVICE_SCALE);
    icon_cold = create_icon(lv_scr_act(), &cold, COLD_X_POS, COLD_Y_POS, COLD_SCALE);
    icon_overheat = create_icon(lv_scr_act(), &overheat, OVERHEAT_X_POS, OVERHEAT_Y_POS, OVERHEAT_SCALE);
    lv_obj_add_flag(icon_frost, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(icon_service, LV_OBJ_FLAG_HIDDEN);
    if (!needle_ceremony_warning_icons_active) {
        lv_obj_add_flag(icon_cold, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_add_flag(icon_overheat, LV_OBJ_FLAG_HIDDEN);
    frost_icon_rendered_visible = false;
    service_icon_rendered_visible = false;
    cold_icon_rendered_visible = needle_ceremony_warning_icons_active;
    overheat_icon_rendered_visible = false;
}

void update_clock_text(const char* text)
{
    if (!clock_label || !text || text[0] == '\0') return;

    const char* current = lv_label_get_text(clock_label);
    if (current && strcmp(current, text) == 0) return;

    lv_label_set_text(clock_label, text);
}

void update_outside_temp(int32_t temp_c, bool valid)
{
    if (!outside_temp_label) return;

    if (!valid) {
        if (outside_temp_rendered_valid ||
            strcmp(lv_label_get_text(outside_temp_label), "--\xC2\xB0" "C") != 0) {
            lv_label_set_text(outside_temp_label, "--\xC2\xB0" "C");
        }
        outside_temp_rendered_valid = false;
        outside_temp_rendered_value = 1000;
        if (!startup_warning_icon_sequence_active) {
            set_icon_rendered_visible(icon_frost, &frost_icon_rendered_visible, false);
        }
        return;
    }

    if (temp_c > 99) temp_c = 99;
    if (temp_c < -9) temp_c = -9;

    if (!outside_temp_rendered_valid || outside_temp_rendered_value != temp_c) {
        lv_label_set_text_fmt(outside_temp_label, "%d\xC2\xB0" "C", (int)temp_c);
        outside_temp_rendered_valid = true;
        outside_temp_rendered_value = temp_c;
    }

    if (!startup_warning_icon_sequence_active) {
        bool showFrost = (temp_c < 10);
        set_icon_rendered_visible(icon_frost, &frost_icon_rendered_visible, showFrost);
    }
}

void update_service_icon(bool visible)
{
    service_icon_requested_visible = visible;
    if (startup_warning_icon_sequence_active) return;
    set_icon_rendered_visible(icon_service, &service_icon_rendered_visible, visible);
}

static const void* get_wifi_icon_from_rssi(int32_t rssi)
{
    if (rssi <= -70) return &wifi_1;
    if (rssi <= -60) return &wifi_2;
    if (rssi <= -50) return &wifi_3;
    return &wifi_full;
}

void update_wifi_icon_connected(int32_t rssi)
{
    if (!icon_wifi) return;

    const void* target = get_wifi_icon_from_rssi(rssi);
    if (lv_img_get_src(icon_wifi) == target) return;

    lv_img_set_src(icon_wifi, target);
}

void update_wifi_icon_disconnected(void)
{
    if (!icon_wifi) return;
    if (lv_img_get_src(icon_wifi) == &wifi_off) return;
    lv_img_set_src(icon_wifi, &wifi_off);
}

void update_bt_icon_connected(void)
{
    if (!icon_bt) return;
    if (lv_img_get_src(icon_bt) == &btOn) return;
    lv_img_set_src(icon_bt, &btOn);
}

void update_bt_icon_disconnected(void)
{
    if (!icon_bt) return;
    if (lv_img_get_src(icon_bt) == &btOff) return;
    lv_img_set_src(icon_bt, &btOff);
}

void update_obd_icon_connected(void)
{
    if (!icon_obd) return;
    if (lv_img_get_src(icon_obd) == &obdOn) return;
    lv_img_set_src(icon_obd, &obdOn);
}

void update_obd_icon_disconnected(void)
{
    if (!icon_obd) return;
    if (lv_img_get_src(icon_obd) == &obdOff) return;
    lv_img_set_src(icon_obd, &obdOff);
}

void update_monitor_ui(monitor_item_t * item, int32_t usage)
{
    if (!item || !item->bar || !item->label_val) return;
    if (usage < 0) usage = 0;
    if (usage > 100) usage = 100;

    char text[8];
    format_percent_text(text, sizeof(text), usage);
    int32_t current = lv_bar_get_value(item->bar);
    bool labelSame = label_text_matches(item->label_val, text);
    if (current == usage && labelSame) return;

    if (current != usage) {
        lv_bar_set_value(item->bar, usage, LV_ANIM_ON);
    }
    if (!labelSame) {
        lv_label_set_text(item->label_val, text);
    }
}

void UiResetRuntimeState(void)
{
    if (monitor_ceremony_timer) {
        lv_timer_del(monitor_ceremony_timer);
        monitor_ceremony_timer = NULL;
    }
    if (needle_ceremony_timer) {
        lv_timer_del(needle_ceremony_timer);
        needle_ceremony_timer = NULL;
    }
    if (startup_warning_icon_timer) {
        lv_timer_del(startup_warning_icon_timer);
        startup_warning_icon_timer = NULL;
    }
    if (oil_life_ceremony_timer) {
        lv_timer_del(oil_life_ceremony_timer);
        oil_life_ceremony_timer = NULL;
    }
    if (debug_log_flush_timer) {
        lv_timer_del(debug_log_flush_timer);
        debug_log_flush_timer = NULL;
    }

    monitor_ceremony_active = false;
    needle_ceremony_active = false;
    needle_ceremony_pending = false;
    needle_ceremony_warning_icons_active = false;
    startup_warning_icon_sequence_active = false;
    startup_warning_icon_sequence_phase = 0;
    oil_life_ceremony_active = false;
    coolant_needle_animating = false;
    battery_needle_animating = false;
    coolant_pending_valid = false;
    battery_pending_valid = false;

    monitor_ceremony_start_ms = 0;
    needle_ceremony_start_ms = 0;
    oil_life_ceremony_start_ms = 0;

    pending_ram_percent = 0;
    pending_core1_percent = 0;
    pending_core2_percent = 0;

    obd_connected_latched = false;
    coolant_valid_latched = false;
    coolant_value_latched = 0;
    battery_valid_latched = false;
    battery_value_latched = 0;
    coolant_rendered_value = -1;
    battery_rendered_value = -1;
    coolant_pending_value = 0;
    battery_pending_value = 0;

    target_needle = NULL;
    target_batt_needle = NULL;
    icon_bt = NULL;
    icon_obd = NULL;
    icon_wifi = NULL;
    icon_frost = NULL;
    icon_service = NULL;
    icon_cold = NULL;
    icon_overheat = NULL;
    clock_label = NULL;
    outside_temp_label = NULL;
    oil_percent_label = NULL;
    oil_life_bar = NULL;
    oil_popup_overlay = NULL;
    outside_temp_rendered_valid = false;
    outside_temp_rendered_value = 1000;
    frost_icon_rendered_visible = false;
    service_icon_rendered_visible = false;
    cold_icon_rendered_visible = false;
    overheat_icon_rendered_visible = false;
    service_icon_requested_visible = false;
    oil_percent_requested_value = 100;
    oil_percent_rendered_value = -1;
    main_test_panel = NULL;
    debug_container = NULL;
    debug_swipe_zone = NULL;
    debug_close_zone = NULL;
    debug_log_ta = NULL;
    debug_screen_visible = false;
    debug_animation_active = false;
    debug_swipe_tracking = false;
    debug_hide_tracking = false;
    debug_swipe_start.x = 0;
    debug_swipe_start.y = 0;
    debug_hide_start.x = 0;
    debug_hide_start.y = 0;
    debug_log_budget_reset();

    cpu_core1.bar = NULL;
    cpu_core1.label_val = NULL;
    cpu_core2.bar = NULL;
    cpu_core2.label_val = NULL;
    ram_usage.bar = NULL;
    ram_usage.label_val = NULL;
}

void create_weather()
{
    lv_obj_t* weather_cont = lv_obj_create(lv_scr_act());
    lv_obj_set_size(weather_cont, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(weather_cont, LV_ALIGN_TOP_LEFT, 250, 15);

    lv_obj_set_style_bg_opa(weather_cont, 0, 0);
    lv_obj_set_style_border_width(weather_cont, 0, 0);
    lv_obj_set_style_pad_all(weather_cont, 0, 0);
    lv_obj_clear_flag(weather_cont, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_set_flex_flow(weather_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(weather_cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(weather_cont, 10, 0);

    lv_obj_t* location_icon = lv_img_create(weather_cont);
    lv_img_set_src(location_icon, &location_non);
    lv_obj_align(location_icon, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t * location_val = lv_label_create(weather_cont);
    lv_label_set_text(location_val, "Gwangmyeong");
    lv_obj_set_style_text_color(location_val, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_style_text_font(location_val, &lv_font_montserrat_24, 0);

    lv_obj_t * weather_val = lv_label_create(weather_cont);
    lv_label_set_text(weather_val, "Mostly Cloudy");
    lv_obj_set_style_text_color(weather_val, lv_color_white(), 0);
    lv_obj_set_style_text_font(weather_val, &lv_font_montserrat_24, 0);
}

void DisplayColorTest() {
    lv_obj_t * test_cont = lv_obj_create(lv_scr_act());
    lv_obj_set_size(test_cont, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_opa(test_cont, 0, 0);
    lv_obj_set_style_border_width(test_cont, 0, 0);
    lv_obj_set_style_pad_all(test_cont, 0, 0);
    lv_obj_clear_flag(test_cont, LV_OBJ_FLAG_SCROLLABLE);

    lv_color_t colors[] = {
        lv_palette_main(LV_PALETTE_RED),
        lv_palette_main(LV_PALETTE_GREEN),
        lv_palette_main(LV_PALETTE_BLUE),
        lv_color_white(),
        lv_color_black()
    };

    const char * color_names[] = {"RED", "GREEN", "BLUE", "WHITE", "BLACK"};

    int col_count = 5;
    int width = LV_HOR_RES / col_count;

    for(int i = 0; i < col_count; i++) {
        lv_obj_t * obj = lv_obj_create(test_cont);
        lv_obj_set_size(obj, width, LV_VER_RES);
        lv_obj_set_pos(obj, i * width, 0);

        lv_obj_set_style_bg_color(obj, colors[i], 0);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(obj, 1, 0);
        lv_obj_set_style_border_color(obj, lv_palette_main(LV_PALETTE_GREY), 0);
        lv_obj_set_style_radius(obj, 0, 0);

        lv_obj_t * label = lv_label_create(obj);
        lv_label_set_text(label, color_names[i]);
        lv_obj_set_style_text_color(label, (i == 3) ? lv_color_black() : lv_color_white(), 0);
        lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
    }
}

static lv_obj_t* create_goodbye_row(lv_obj_t* parent, const char* title, const char* value)
{
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, 0, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 20, 0);

    lv_obj_t* title_lbl = lv_label_create(row);
    lv_label_set_text(title_lbl, title);
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_opa(title_lbl, 255, 0);
    lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_24, 0);
    /* keep content width so title+value can be centered as a pair */

    lv_obj_t* value_lbl = lv_label_create(row);
    lv_label_set_text(value_lbl, value);
    lv_obj_set_style_text_color(value_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_opa(value_lbl, 255, 0);
    lv_obj_set_style_text_font(value_lbl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_align(value_lbl, LV_TEXT_ALIGN_RIGHT, 0);

    return value_lbl;
}

void create_goodbye_screen(const char* time_text, const char* distance_text)
{
    lv_obj_t* goodbye_container = lv_obj_create(lv_scr_act());
    lv_obj_set_size(goodbye_container, 800, 480);
    lv_obj_clear_flag(goodbye_container, LV_OBJ_FLAG_SCROLLABLE);      /// Flags
    lv_obj_set_style_bg_color(goodbye_container, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(goodbye_container, 255, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_set_style_border_width(goodbye_container, 0, 0);
    lv_obj_set_style_outline_width(goodbye_container, 0, 0);
    lv_obj_set_style_shadow_width(goodbye_container, 0, 0);

    lv_obj_t* main_title = lv_label_create(goodbye_container);
    lv_obj_set_width(main_title, LV_SIZE_CONTENT);   /// 8
    lv_obj_set_height(main_title, LV_SIZE_CONTENT);    /// 1
    lv_obj_set_x(main_title, 0);
    lv_obj_set_y(main_title, -150);
    lv_obj_set_align(main_title, LV_ALIGN_CENTER);
    lv_label_set_text(main_title, "Last Trip Info.");
    lv_obj_set_style_text_color(main_title, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(main_title, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(main_title, &lv_font_montserrat_36, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t* info_cont = lv_obj_create(goodbye_container);
    lv_obj_set_size(info_cont, 600, 250);
    lv_obj_align(info_cont, LV_ALIGN_CENTER, 0, 20);
    lv_obj_set_style_bg_opa(info_cont, 0, 0);
    lv_obj_set_style_border_width(info_cont, 0, 0);
    lv_obj_set_style_pad_all(info_cont, 0, 0);
    lv_obj_clear_flag(info_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(info_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(info_cont, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(info_cont, 0, 0);

    const char* safe_time = (time_text && time_text[0] != '\0') ? time_text : "--:--";
    const char* safe_dist = (distance_text && distance_text[0] != '\0') ? distance_text : "--";

    create_goodbye_row(info_cont, "Time (HH:MM) : ", safe_time);
    create_goodbye_row(info_cont, "Distance (Km) : ", safe_dist);
    create_goodbye_row(info_cont, "Avg. Cons. (Km/L) : ", "15.6");

    lv_obj_t* upper_bar = lv_obj_create(goodbye_container);
    lv_obj_set_width(upper_bar, 600);
    lv_obj_set_height(upper_bar, 4);
    lv_obj_set_x(upper_bar, 0);
    lv_obj_set_y(upper_bar, -100);
    lv_obj_set_align(upper_bar, LV_ALIGN_CENTER);
    lv_obj_clear_flag(upper_bar, LV_OBJ_FLAG_SCROLLABLE);      /// Flags

    lv_obj_t* lower_bar = lv_obj_create(goodbye_container);
    lv_obj_set_width(lower_bar, 600);
    lv_obj_set_height(lower_bar, 4);
    lv_obj_set_x(lower_bar, -1);
    lv_obj_set_y(lower_bar, 150);
    lv_obj_set_align(lower_bar, LV_ALIGN_CENTER);
    lv_obj_clear_flag(lower_bar, LV_OBJ_FLAG_SCROLLABLE);      /// Flags
}

void create_debug_screen(void)
{
#if !UI_DEBUG_SCREEN_ENABLED
    debug_container = NULL;
    debug_swipe_zone = NULL;
    debug_close_zone = NULL;
    debug_log_ta = NULL;
    debug_screen_visible = false;
    debug_animation_active = false;
    return;
#else
    debug_container = lv_obj_create(lv_scr_act());
    lv_obj_set_size(debug_container, SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_obj_clear_flag(debug_container, LV_OBJ_FLAG_SCROLLABLE);      /// Flags
    lv_obj_set_style_bg_color(debug_container, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(debug_container, 255, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_set_style_border_width(debug_container, 0, 0);
    lv_obj_set_style_outline_width(debug_container, 0, 0);
    lv_obj_set_style_shadow_width(debug_container, 0, 0);

    lv_obj_t* main_title = lv_label_create(debug_container);
    lv_obj_set_width(main_title, LV_SIZE_CONTENT);   /// 8
    lv_obj_set_height(main_title, LV_SIZE_CONTENT);    /// 1
    lv_obj_set_x(main_title, 10);
    lv_obj_set_y(main_title, -5);
    lv_obj_set_align(main_title, LV_ALIGN_TOP_LEFT);
    lv_label_set_text(main_title, "Debug Log");
    lv_obj_set_style_text_color(main_title, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(main_title, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(main_title, &lv_font_montserrat_24, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_flag(main_title, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t* upper_bar = lv_obj_create(debug_container);
    lv_obj_set_width(upper_bar, 800);
    lv_obj_set_height(upper_bar, 4);
    lv_obj_set_x(upper_bar, 0);
    lv_obj_set_y(upper_bar, 35);
    lv_obj_set_align(upper_bar, LV_ALIGN_TOP_MID);
    lv_obj_clear_flag(upper_bar, LV_OBJ_FLAG_SCROLLABLE);      /// Flags
    lv_obj_add_flag(upper_bar, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t* lower_bar = lv_obj_create(debug_container);
    lv_obj_set_width(lower_bar, 60);
    lv_obj_set_height(lower_bar, 4);
    lv_obj_set_x(lower_bar, 0);
    lv_obj_set_y(lower_bar, 10);
    lv_obj_set_align(lower_bar, LV_ALIGN_BOTTOM_MID);
    lv_obj_clear_flag(lower_bar, LV_OBJ_FLAG_SCROLLABLE);      /// Flags
    lv_obj_add_flag(lower_bar, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t* log_ta = lv_textarea_create(debug_container);
    lv_obj_set_size(log_ta, 760, 390);
    lv_obj_align(log_ta, LV_ALIGN_TOP_MID, 0, 45);
    lv_textarea_set_one_line(log_ta, false);
    lv_obj_set_scroll_dir(log_ta, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(log_ta, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_clear_flag(log_ta, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_style_bg_color(log_ta, lv_color_hex(0x101010), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(log_ta, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(log_ta, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_outline_width(log_ta, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(log_ta, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(log_ta, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(log_ta, &lv_font_montserrat_20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_line_space(log_ta, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
    debug_log_ta = log_ta;
    create_debug_close_zone();

    if (debug_log_flush_timer) {
        lv_timer_del(debug_log_flush_timer);
        debug_log_flush_timer = NULL;
    }
    debug_log_flush_timer = lv_timer_create(debug_log_flush_timer_cb, DEBUG_LOG_FLUSH_PERIOD_MS, NULL);
    if (debug_log_flush_timer) {
        lv_timer_pause(debug_log_flush_timer);
    }
    debug_log_flush_timer_cb(NULL);

    lv_obj_set_pos(debug_container, 0, -SCREEN_HEIGHT);
    lv_obj_add_flag(debug_container, LV_OBJ_FLAG_HIDDEN);
    debug_screen_visible = false;
    debug_animation_active = false;
    create_debug_swipe_zone();
#endif
}

void GaugeInit()
{
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_black(), 0);

    // DisplayColorTest();
    UiResetRuntimeState();
    needle_ceremony_pending = true;
    needle_ceremony_warning_icons_active = true;

    create_outside_temp();
    create_clock();
    create_sys_monitor_panel();
    create_gauge();
    start_startup_warning_icon_sequence();
    start_oil_life_ceremony();
    //create_weather();
    create_debug_screen();

    pending_ram_percent = 0;
    pending_core1_percent = 0;
    pending_core2_percent = 0;
    start_monitor_ceremony();
    start_needle_ceremony();

    lv_obj_clear_flag(lv_scr_act(), LV_OBJ_FLAG_SCROLLABLE);
}


void update_system_monitor(int32_t ram_percent, int32_t core1_percent, int32_t core2_percent)
{
    if (ram_percent < 0) ram_percent = 0;
    if (ram_percent > 100) ram_percent = 100;
    if (core1_percent < 0) core1_percent = 0;
    if (core1_percent > 100) core1_percent = 100;
    if (core2_percent < 0) core2_percent = 0;
    if (core2_percent > 100) core2_percent = 100;

    pending_ram_percent = ram_percent;
    pending_core1_percent = core1_percent;
    pending_core2_percent = core2_percent;

    if (monitor_ceremony_active) return;

    update_monitor_ui(&ram_usage, ram_percent);
    update_monitor_ui(&cpu_core1, core1_percent);
    update_monitor_ui(&cpu_core2, core2_percent);
}



