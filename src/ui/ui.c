#include <ui.h>

static lv_obj_t * target_needle = NULL;
static lv_obj_t * target_batt_needle = NULL;
static lv_obj_t * icon_bt = NULL;
static lv_obj_t * icon_obd = NULL;
static lv_obj_t * icon_wifi = NULL;
static lv_obj_t * icon_frost = NULL;
static lv_obj_t * main_test_panel = NULL;

static monitor_item_t cpu_core1, cpu_core2, ram_usage;

// v8.3?êÏÑú??lv_img_set_angle???¨Ïö©?òÎ©∞ ?®ÏúÑ??0.1?ÑÏûÖ?àÎã§.
static void needle_anim_exec_cb(void * var, int32_t v)
{
    lv_img_set_angle((lv_obj_t *)var, (int16_t)(v % 3600));
}

void update_coolant_gauge(int32_t val)
{
    if (!target_needle) return;

    val = (val < 0) ? 0 : (val > 150 ? 150 : val);

    static int32_t last_coolant_val = -1;
    if (val == last_coolant_val) return;
    last_coolant_val = val;

    int32_t target_angle = (GAUGE_START_ANGLE + (val * GAUGE_MOVE_RANGE / 150) + GAUGE_MIN_ROT) % 3600;

    static uint32_t last_update_ms = 0;
    uint32_t now = lv_tick_get();
    if ((now - last_update_ms) < 41) return;
    last_update_ms = now;

    int32_t current = lv_img_get_angle(target_needle) % 3600;
    if (LV_ABS(target_angle - current) < 3) return;

    lv_anim_del(target_needle, (lv_anim_exec_xcb_t)needle_anim_exec_cb);
    lv_img_set_angle(target_needle, (int16_t)target_angle);
    lv_obj_invalidate(target_needle);
}
void update_battery_gauge(int32_t val)
{
    if (!target_batt_needle) return;

    val = (val < 0) ? 0 : (val > 20 ? 20 : val);

    static int32_t last_batt_val = -1;
    if (val == last_batt_val) return;
    last_batt_val = val;

    int32_t target_angle = (GAUGE_START_ANGLE + (val * GAUGE_MOVE_RANGE / 20) + GAUGE_MIN_ROT) % 3600;

    static uint32_t last_update_ms = 0;
    uint32_t now = lv_tick_get();
    if ((now - last_update_ms) < 41) return;
    last_update_ms = now;

    int32_t current = lv_img_get_angle(target_batt_needle) % 3600;
    if (LV_ABS(target_angle - current) < 3) return;

    lv_anim_del(target_batt_needle, (lv_anim_exec_xcb_t)needle_anim_exec_cb);
    lv_img_set_angle(target_batt_needle, (int16_t)target_angle);
    lv_obj_invalidate(target_batt_needle);
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
    lv_label_set_text(label_val, "25¬∞C");
    lv_obj_set_style_text_color(label_val, lv_color_white(), 0);
    lv_obj_set_style_text_font(label_val, &lv_font_montserrat_36, 0);
}

void create_clock()
{
    lv_obj_t* clock = lv_label_create(lv_scr_act());
    lv_label_set_text(clock, "12:45");
    lv_obj_set_style_text_color(clock, lv_color_white(), 0);
    lv_obj_set_style_text_font(clock, &lv_font_montserrat_36, 0);
    lv_obj_align(clock, LV_ALIGN_TOP_RIGHT, -30, 10);
    lv_obj_clear_flag(clock, LV_OBJ_FLAG_SCROLLABLE);
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

    // 1. Î∞∞Í≤Ω ?ùÏÑ± (Í≤åÏù¥ÏßÄ Ïª®ÌÖå?¥ÎÑà ?¥Î?)
    lv_obj_t* bg = lv_img_create(gauge_box);
    lv_img_set_src(bg, bg_dsc);
    lv_obj_clear_flag(bg, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(bg, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_pos(bg, 0, 0);

    // 2. Î∞îÎäò ?ùÏÑ± (Í≤åÏù¥ÏßÄ Ïª®ÌÖå?¥ÎÑà ?¥Î?)
    lv_obj_t* needle_obj = lv_img_create(gauge_box);
    lv_img_set_src(needle_obj, &needle);
    lv_img_set_antialias(needle_obj, false);
    lv_obj_set_style_img_recolor_opa(needle_obj, 0, 0);

    // 3. Î∞îÎäò??Î∞∞Í≤Ω ?¥Î?ÏßÄ Ï§ëÏïô???ïÎ†¨
    lv_obj_update_layout(bg);
    lv_obj_align_to(needle_obj, bg, LV_ALIGN_CENTER, 0, 0);

    // 4. ?åÏ†ÑÏ∂?Pivot) ?§Ï†ï
    // ?¥Î?ÏßÄ ?êÏ≤¥ ?¨Í∏∞??ÎßûÎäî Pivot Ï¢åÌëú (?? Î∞îÎäò ?¥Î?ÏßÄ Í∞ÄÎ°?2, ?∏Î°ú/2 ?πÏ? ?πÏ†ï ?åÏ†Ñ Ï§ëÏã¨)
    lv_img_set_pivot(needle_obj, GAUGE_PIVOT_X, GAUGE_PIVOT_Y);

    // Layout engine ?ÅÌñ• ÏµúÏÜå??    lv_obj_clear_flag(needle_obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(needle_obj, LV_OBJ_FLAG_IGNORE_LAYOUT);

    // 5. ?ÑÏó≠ ?¨Ïù∏???†Îãπ Î∞?Ï¥àÍ∏∞??
    if(is_coolant) {
        target_needle = needle_obj;
        update_coolant_gauge(0);
    } else {
        target_batt_needle = needle_obj;
        update_battery_gauge(0);
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

    lv_obj_set_style_bg_color(item->bar, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(item->bar, lv_color_hex(0x222222), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(item->bar, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_radius(item->bar, 4, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(item->bar, 8, LV_PART_MAIN);
    lv_obj_set_style_shadow_ofs_y(item->bar, 2, LV_PART_MAIN);
    lv_obj_set_style_shadow_spread(item->bar, 1, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(item->bar, lv_color_black(), LV_PART_MAIN);

    lv_obj_set_style_radius(item->bar, 3, LV_PART_INDICATOR);
    lv_color_t color_mid = lv_color_lighten(color, LV_OPA_60);
    lv_color_t color_bottom = lv_color_darken(color, LV_OPA_60);
    lv_obj_set_style_bg_color(item->bar, color_mid, LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_color(item->bar, color_bottom, LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_dir(item->bar, LV_GRAD_DIR_VER, LV_PART_INDICATOR);
    lv_obj_set_style_border_side(item->bar, LV_BORDER_SIDE_TOP, LV_PART_INDICATOR);
    lv_obj_set_style_border_width(item->bar, 2, LV_PART_INDICATOR);
    lv_obj_set_style_border_color(item->bar, lv_color_white(), LV_PART_INDICATOR);
    lv_obj_set_style_border_opa(item->bar, LV_OPA_40, LV_PART_INDICATOR);
    lv_obj_set_style_outline_width(item->bar, 1, LV_PART_INDICATOR);
    lv_obj_set_style_outline_color(item->bar, lv_color_black(), LV_PART_INDICATOR);
    lv_obj_set_style_outline_pad(item->bar, -1, LV_PART_INDICATOR);

    lv_obj_set_style_bg_grad_dir(item->bar, LV_GRAD_DIR_VER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_main_stop(item->bar, 0, LV_PART_INDICATOR);     // ?úÏûë??
    lv_obj_set_style_bg_grad_stop(item->bar, 255, LV_PART_INDICATOR);

    lv_obj_set_style_anim_time(item->bar, 600, 0);

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

    icon_bt = create_icon(lv_scr_act(), &btOn, BT_X_POS, BT_Y_POS);
    icon_obd = create_icon(lv_scr_act(), &obdOn, OBD_X_POS, OBD_Y_POS);
    icon_wifi = create_icon(lv_scr_act(), &wifi_full, WIFI_X_POS, WIFI_Y_POS);
    icon_frost = create_icon(lv_scr_act(), &frost, FROST_X_POS, FROST_Y_POS);
}

void update_monitor_ui(monitor_item_t * item, int32_t usage)
{
    lv_bar_set_value(item->bar, usage, LV_ANIM_ON);
    lv_label_set_text_fmt(item->label_val, "%d%%", usage);
}

void DisplayColorTest() {
    lv_obj_t * test_cont = lv_obj_create(lv_scr_act());
    lv_obj_set_size(test_cont, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_opa(test_cont, 0, 0);
    lv_obj_set_style_border_width(test_cont, 0, 0);
    lv_obj_set_style_pad_all(test_cont, 0, 0);
    lv_obj_clear_flag(test_cont, LV_OBJ_FLAG_SCROLLABLE);

    // v8.3 Î∞©Ïãù???âÏÉÅ Î∞∞Ïó¥
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
        // Í∏Ä?êÏÉâ ?§Ï†ï: Î∞∞Í≤Ω???∞ÏÉâ???åÎßå Í≤Ä?Ä??Í∏Ä??
        lv_obj_set_style_text_color(label, (i == 3) ? lv_color_black() : lv_color_white(), 0);
        lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
    }
}

void GaugeInit()
{
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_black(), 0);

    // DisplayColorTest();

    create_outside_temp();
    create_clock();
    create_sys_monitor_panel();
    create_gauge();

    update_monitor_ui(&ram_usage, 45);
    update_monitor_ui(&cpu_core1, 70);
    update_monitor_ui(&cpu_core2, 30);

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

    update_monitor_ui(&ram_usage, ram_percent);
    update_monitor_ui(&cpu_core1, core1_percent);
    update_monitor_ui(&cpu_core2, core2_percent);
}
