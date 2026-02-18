#include <ui.h>
#include <string.h>

static lv_obj_t * target_needle = NULL;
static lv_obj_t * target_batt_needle = NULL;
static lv_obj_t * icon_bt = NULL;
static lv_obj_t * icon_obd = NULL;
static lv_obj_t * icon_wifi = NULL;
static lv_obj_t * icon_frost = NULL;
static lv_obj_t * clock_label = NULL;
static lv_obj_t * main_test_panel = NULL;

static monitor_item_t cpu_core1, cpu_core2, ram_usage;
static lv_timer_t * monitor_ceremony_timer = NULL;
static bool monitor_ceremony_active = false;
static uint32_t monitor_ceremony_start_ms = 0;
static int32_t pending_ram_percent = 0;
static int32_t pending_core1_percent = 0;
static int32_t pending_core2_percent = 0;

static lv_timer_t * needle_ceremony_timer = NULL;
static lv_timer_t * needle_ceremony_delay_timer = NULL;
static bool needle_ceremony_active = false;
static bool needle_ceremony_pending = false;
static uint32_t needle_ceremony_start_ms = 0;
static bool obd_connected_latched = false;
static bool coolant_valid_latched = false;
static int32_t coolant_value_latched = 0;
static bool battery_valid_latched = false;
static int32_t battery_value_latched = 0;
static int32_t coolant_rendered_value = -1;
static int32_t battery_rendered_value = -1;

static void set_coolant_gauge_instant(int32_t val);
static void set_battery_gauge_instant(int32_t val);
static void start_needle_ceremony(void);
static void apply_latched_obd_needles(void);
static void needle_ceremony_delay_timer_cb(lv_timer_t * t);

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

static void set_monitor_item_instant(monitor_item_t * item, int32_t usage)
{
    if (!item || !item->bar || !item->label_val) return;
    if (usage < 0) usage = 0;
    if (usage > 100) usage = 100;
    lv_bar_set_value(item->bar, usage, LV_ANIM_OFF);
    lv_label_set_text_fmt(item->label_val, "%d%%", usage);
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

void update_coolant_gauge(int32_t val)
{
    if (!target_needle) return;

    val = clamp_coolant(val);
    if (val == coolant_rendered_value) return;
    coolant_rendered_value = val;

    int32_t target_angle = coolant_to_angle(val);
    int32_t start_angle = lv_img_get_angle(target_needle);

    int32_t diff = (target_angle % 3600) - (start_angle % 3600);
    if (diff > 1800) diff -= 3600;
    else if (diff < -1800) diff += 3600;

    lv_anim_del(target_needle, (lv_anim_exec_xcb_t)needle_anim_exec_cb);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, target_needle);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)needle_anim_exec_cb);
    lv_anim_set_values(&a, start_angle, start_angle + diff);
    lv_anim_set_time(&a, 600);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

void update_battery_gauge(int32_t val)
{
    if (!target_batt_needle) return;

    val = clamp_battery(val);
    if (val == battery_rendered_value) return;
    battery_rendered_value = val;

    int32_t target_angle = battery_to_angle(val);
    int32_t start_angle = lv_img_get_angle(target_batt_needle);

    int32_t diff = (target_angle % 3600) - (start_angle % 3600);
    if (diff > 1800) diff -= 3600;
    else if (diff < -1800) diff += 3600;

    lv_anim_del(target_batt_needle, (lv_anim_exec_xcb_t)needle_anim_exec_cb);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, target_batt_needle);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)needle_anim_exec_cb);
    lv_anim_set_values(&a, start_angle, start_angle + diff);
    lv_anim_set_time(&a, 600);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static void set_coolant_gauge_instant(int32_t val)
{
    if (!target_needle) return;
    val = clamp_coolant(val);
    coolant_rendered_value = val;
    lv_anim_del(target_needle, (lv_anim_exec_xcb_t)needle_anim_exec_cb);
    lv_img_set_angle(target_needle, (int16_t)(coolant_to_angle(val) % 3600));
}

static void set_battery_gauge_instant(int32_t val)
{
    if (!target_batt_needle) return;
    val = clamp_battery(val);
    battery_rendered_value = val;
    lv_anim_del(target_batt_needle, (lv_anim_exec_xcb_t)needle_anim_exec_cb);
    lv_img_set_angle(target_batt_needle, (int16_t)(battery_to_angle(val) % 3600));
}

static void apply_latched_obd_needles(void)
{
    if (coolant_valid_latched) update_coolant_gauge(coolant_value_latched);
    else set_coolant_gauge_instant(0);

    if (battery_valid_latched) update_battery_gauge(battery_value_latched);
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
    set_coolant_gauge_instant(0);
    set_battery_gauge_instant(0);
    needle_ceremony_timer = lv_timer_create(needle_ceremony_timer_cb, 8, NULL);
}

static void needle_ceremony_delay_timer_cb(lv_timer_t * t)
{
    LV_UNUSED(t);
    if (needle_ceremony_delay_timer) {
        lv_timer_del(needle_ceremony_delay_timer);
        needle_ceremony_delay_timer = NULL;
    }
    start_needle_ceremony();
}

void update_obd_gauges(bool obd_connected, bool coolant_valid, int32_t coolant_val, bool battery_valid, int32_t battery_val)
{
    obd_connected_latched = obd_connected;

    if (coolant_valid) {
        coolant_valid_latched = true;
        coolant_value_latched = clamp_coolant(coolant_val);
    }

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

    lv_obj_t * label_val = lv_label_create(temp_cont);
    lv_label_set_text(label_val, "--°C");
    lv_obj_set_style_text_color(label_val, lv_color_white(), 0);
    lv_obj_set_style_text_font(label_val, &lv_font_montserrat_36, 0);
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

lv_obj_t* create_icon(lv_obj_t* parent, const void* src, int32_t x, int32_t y)
{
    lv_obj_t* img = lv_img_create(parent);
    lv_img_set_src(img, src);
    lv_img_set_pivot(img, 0, 0);
    lv_obj_set_pos(img, x, y);
    return img;
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

    icon_bt = create_icon(lv_scr_act(), &btOff, BT_X_POS, BT_Y_POS);
    icon_obd = create_icon(lv_scr_act(), &obdOff, OBD_X_POS, OBD_Y_POS);
    icon_wifi = create_icon(lv_scr_act(), &wifi_off, WIFI_X_POS, WIFI_Y_POS);
    icon_frost = create_icon(lv_scr_act(), &frost, FROST_X_POS, FROST_Y_POS);
}

void update_clock_text(const char* text)
{
    if (!clock_label || !text || text[0] == '\0') return;

    const char* current = lv_label_get_text(clock_label);
    if (current && strcmp(current, text) == 0) return;

    lv_label_set_text(clock_label, text);
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
    int32_t current = lv_bar_get_value(item->bar);
    if (current == usage) return;

    lv_bar_set_value(item->bar, usage, LV_ANIM_ON);
    lv_label_set_text_fmt(item->label_val, "%d%%", usage);
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
    if (needle_ceremony_delay_timer) {
        lv_timer_del(needle_ceremony_delay_timer);
        needle_ceremony_delay_timer = NULL;
    }

    monitor_ceremony_active = false;
    needle_ceremony_active = false;
    needle_ceremony_pending = false;

    monitor_ceremony_start_ms = 0;
    needle_ceremony_start_ms = 0;

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

    target_needle = NULL;
    target_batt_needle = NULL;
    icon_bt = NULL;
    icon_obd = NULL;
    icon_wifi = NULL;
    icon_frost = NULL;
    clock_label = NULL;
    main_test_panel = NULL;

    cpu_core1.bar = NULL;
    cpu_core1.label_val = NULL;
    cpu_core2.bar = NULL;
    cpu_core2.label_val = NULL;
    ram_usage.bar = NULL;
    ram_usage.label_val = NULL;
}

void DrawGoodbyeScreenDummy(void)
{
    // Placeholder. The user will provide the actual goodbye screen rendering.
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

void GaugeInit()
{
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_black(), 0);

    // DisplayColorTest();
    UiResetRuntimeState();
    needle_ceremony_pending = true;

    create_outside_temp();
    create_clock();
    create_sys_monitor_panel();
    create_gauge();

    pending_ram_percent = 0;
    pending_core1_percent = 0;
    pending_core2_percent = 0;
    start_monitor_ceremony();
    needle_ceremony_delay_timer = lv_timer_create(needle_ceremony_delay_timer_cb, 3000, NULL);
    if (needle_ceremony_delay_timer) {
        lv_timer_set_repeat_count(needle_ceremony_delay_timer, 1);
    } else {
        needle_ceremony_pending = false;
        start_needle_ceremony();
    }

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





