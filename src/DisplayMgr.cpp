#include "DisplayMgr.h"
#include <CommonApi.h>
#include <ObdMgr.h>
#include <lvgl.h>
#include <SD.h>
#include <ui.h>
#include <stdio.h>
#include "esp_heap_caps.h"
#include "freertos/task.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "esp_freertos_hooks.h"
#if defined(CONFIG_IDF_TARGET_ESP32S3)
#include "esp32s3/rom/cache.h"
extern "C" int Cache_WriteBack_Addr(uint32_t addr, uint32_t size);
#endif
#if LV_USE_GIF
#include <src/extra/libs/gif/lv_gif.h>
#endif

#define TEST_LOG(fmt, ...) UartLogf("[DisplayMgr] " fmt "\n", ##__VA_ARGS__)
#define TEST_LINE() UartLogf("[DisplayMgr] %s\n", __func__)

static constexpr uint32_t GIF_TASK_STACK_WORDS = 8192;

static SemaphoreHandle_t s_vsyncSem = nullptr;
static volatile uint32_t s_vsyncCount = 0;
static volatile uint32_t s_swapReqCount = 0;
static volatile uint32_t s_swapDoneCount = 0;
static volatile uint32_t s_swapSkipTimeoutCount = 0;
static volatile uint32_t s_replayBypassCount = 0;
static volatile uint32_t s_swapForcedOnTimeoutCount = 0;
static volatile uint32_t s_singleFlushAreaCount = 0;
static volatile uint32_t s_singleFlushFrameCount = 0;
static volatile uint32_t s_singleFlushPixelCount = 0;
static volatile uint64_t s_singleFlushCopyTimeUs = 0;
static volatile uint32_t s_singleFlushMaxCopyUs = 0;
static bool s_flushFrameStarted = false;
static bool s_usePseudoDoubleBuffer = false;
static bool s_backBufferSynced = true;
static bool s_frameHasFullRefreshArea = false;
static lv_area_t s_dirtyAreas[64];
static uint8_t s_dirtyAreaCount = 0;
static volatile uint32_t s_idleLoopCount[2] = {0, 0};
static bool s_idleHooksRegistered = false;
static volatile bool s_splashActive = false;
static volatile bool s_splashGifReady = false;
static volatile bool s_goodbyeScreenActive = false;
static bool s_strictVsyncSync = false;
static TickType_t s_monitorResumeTick = 0;
static bool s_loggedNullFrontFb = false;
static bool s_loggedNullBackFb = false;
static uint8_t s_vsyncTimeoutConsecutive = 0;

static constexpr TickType_t MONITOR_UPDATE_PERIOD_TICKS = pdMS_TO_TICKS(200);
static constexpr TickType_t UI_SHARED_UPDATE_PERIOD_TICKS = pdMS_TO_TICKS(100);
static constexpr TickType_t LVGL_TASK_PERIOD_TICKS = pdMS_TO_TICKS(20);
static constexpr uint8_t VSYNC_TIMEOUT_SKIP_BEFORE_FORCE_SWAP = 3;
#ifndef DISPLAY_PCLK_DERATE_PERCENT
#define DISPLAY_PCLK_DERATE_PERCENT 90U
#endif
static constexpr uint32_t DISPLAY_TARGET_PCLK_HZ =
    (uint32_t)(((uint64_t)ST7262_PANEL_CONFIG_TIMINGS_PCLK_HZ * (uint64_t)DISPLAY_PCLK_DERATE_PERCENT) / 100ULL);
static constexpr bool DISPLAY_ROTATE_180 = true;

static inline uint32_t area_pixels(const lv_area_t* a)
{
    if (!a) return 0;
    uint32_t w = lv_area_get_width(a);
    uint32_t h = lv_area_get_height(a);
    return w * h;
}

static inline lv_area_t rotate_area_180(const lv_area_t* a)
{
    lv_area_t out = *a;
    out.x1 = SCREEN_WIDTH - 1 - a->x2;
    out.x2 = SCREEN_WIDTH - 1 - a->x1;
    out.y1 = SCREEN_HEIGHT - 1 - a->y2;
    out.y2 = SCREEN_HEIGHT - 1 - a->y1;
    return out;
}

static inline void cache_writeback_span(const void* ptr, size_t bytes)
{
#if defined(CONFIG_IDF_TARGET_ESP32S3)
    if (!ptr || bytes == 0) return;
    uintptr_t start = (uintptr_t)ptr;
    uintptr_t aligned_start = start & ~((uintptr_t)31);
    uintptr_t end = start + bytes;
    uintptr_t aligned_end = (end + 31) & ~((uintptr_t)31);
    if (aligned_end > aligned_start) {
        Cache_WriteBack_Addr((uint32_t)aligned_start, (uint32_t)(aligned_end - aligned_start));
    }
#else
    LV_UNUSED(ptr);
    LV_UNUSED(bytes);
#endif
}

static inline void cache_writeback_area(uint16_t* fb, const lv_area_t* area)
{
    if (!fb || !area) return;

    uint32_t w = lv_area_get_width(area);
    uint32_t h = lv_area_get_height(area);
    if (w == 0 || h == 0) return;

    size_t rowBytes = (size_t)w * sizeof(uint16_t);
    bool fullWidth = (area->x1 == 0) && (w == SCREEN_WIDTH);
    if (fullWidth) {
        size_t off = (size_t)area->y1 * SCREEN_WIDTH;
        cache_writeback_span(&fb[off], rowBytes * h);
        return;
    }

    for (uint32_t y = 0; y < h; ++y) {
        size_t off = (size_t)(area->y1 + y) * SCREEN_WIDTH + (size_t)area->x1;
        cache_writeback_span(&fb[off], rowBytes);
    }
}

static inline bool area_can_merge(const lv_area_t* a, const lv_area_t* b)
{
    // Overlap or 1px-adjacent areas are merged to reduce replay fragments.
    if ((a->x2 + 1) < b->x1) return false;
    if ((b->x2 + 1) < a->x1) return false;
    if ((a->y2 + 1) < b->y1) return false;
    if ((b->y2 + 1) < a->y1) return false;
    return true;
}

static inline lv_area_t area_union(const lv_area_t* a, const lv_area_t* b)
{
    lv_area_t out;
    out.x1 = (a->x1 < b->x1) ? a->x1 : b->x1;
    out.y1 = (a->y1 < b->y1) ? a->y1 : b->y1;
    out.x2 = (a->x2 > b->x2) ? a->x2 : b->x2;
    out.y2 = (a->y2 > b->y2) ? a->y2 : b->y2;
    return out;
}

static void add_dirty_area_merged(const lv_area_t* area)
{
    if (!area) return;
    lv_area_t merged = *area;

    bool merged_any = true;
    while (merged_any) {
        merged_any = false;
        for (uint8_t i = 0; i < s_dirtyAreaCount; ++i) {
            if (area_can_merge(&s_dirtyAreas[i], &merged)) {
                merged = area_union(&s_dirtyAreas[i], &merged);
                // Remove consumed slot by swap-with-last.
                s_dirtyAreas[i] = s_dirtyAreas[s_dirtyAreaCount - 1];
                s_dirtyAreaCount--;
                merged_any = true;
                break;
            }
        }
    }

    if (s_dirtyAreaCount < (uint8_t)(sizeof(s_dirtyAreas) / sizeof(s_dirtyAreas[0]))) {
        s_dirtyAreas[s_dirtyAreaCount++] = merged;
    } else {
        // Fallback: collapse to one full-screen area if list overflows.
        s_dirtyAreaCount = 1;
        s_dirtyAreas[0].x1 = 0;
        s_dirtyAreas[0].y1 = 0;
        s_dirtyAreas[0].x2 = SCREEN_WIDTH - 1;
        s_dirtyAreas[0].y2 = SCREEN_HEIGHT - 1;
    }
}

static bool IRAM_ATTR idle_hook_cpu0(void)
{
    s_idleLoopCount[0]++;
    return false;
}

static bool IRAM_ATTR idle_hook_cpu1(void)
{
    s_idleLoopCount[1]++;
    return false;
}

static void register_idle_hooks_once()
{
    if (s_idleHooksRegistered) return;

    esp_err_t e0 = esp_register_freertos_idle_hook_for_cpu(idle_hook_cpu0, 0);
    esp_err_t e1 = esp_register_freertos_idle_hook_for_cpu(idle_hook_cpu1, 1);
    if (e0 == ESP_OK && e1 == ESP_OK) {
        s_idleHooksRegistered = true;
        Serial.println("[DisplayMgr] Idle hooks registered for CPU monitor");
    } else {
        TEST_LOG("Idle hook register failed: e0=%d e1=%d", (int)e0, (int)e1);
    }
}

static bool sample_cpu_usage(uint8_t* core0_usage, uint8_t* core1_usage)
{
    if (!core0_usage || !core1_usage || !s_idleHooksRegistered) return false;

    static uint32_t prevIdle0 = 0;
    static uint32_t prevIdle1 = 0;
    static uint32_t peakIdle0 = 1;
    static uint32_t peakIdle1 = 1;

    uint32_t nowIdle0 = s_idleLoopCount[0];
    uint32_t nowIdle1 = s_idleLoopCount[1];

    if (prevIdle0 == 0 && prevIdle1 == 0) {
        prevIdle0 = nowIdle0;
        prevIdle1 = nowIdle1;
        return false;
    }

    uint32_t dIdle0 = nowIdle0 - prevIdle0;
    uint32_t dIdle1 = nowIdle1 - prevIdle1;
    prevIdle0 = nowIdle0;
    prevIdle1 = nowIdle1;

    if (dIdle0 > peakIdle0) peakIdle0 = dIdle0;
    if (dIdle1 > peakIdle1) peakIdle1 = dIdle1;

    uint32_t u0 = 100U - ((dIdle0 * 100U) / peakIdle0);
    uint32_t u1 = 100U - ((dIdle1 * 100U) / peakIdle1);
    if (u0 > 100U) u0 = 100U;
    if (u1 > 100U) u1 = 100U;

    *core0_usage = (uint8_t)u0;
    *core1_usage = (uint8_t)u1;
    return true;
}

static void apply_shared_ui_state(const UiSharedState& s)
{
    if (s.wifiConnected) update_wifi_icon_connected(s.wifiRssi);
    else update_wifi_icon_disconnected();

    if (s.clockValid && s.clockText[0] != '\0') {
        update_clock_text(s.clockText);
    }
    update_outside_temp((int32_t)s.outsideTempC, s.outsideTempValid);

    if (s.btConnected) update_bt_icon_connected();
    else update_bt_icon_disconnected();

    if (s.obdStatus == OBD_CONNECTED) update_obd_icon_connected();
    else update_obd_icon_disconnected();

    update_obd_gauges(
        s.obdStatus == OBD_CONNECTED,
        s.coolantValid, (int32_t)s.coolant,
        s.batteryValid, (int32_t)s.batteryVoltage
    );
}

static void goodbye_fade_exec_cb(void* var, int32_t v)
{
    if (!var) return;
    lv_obj_set_style_bg_opa((lv_obj_t*)var, (lv_opa_t)v, 0);
}

static void goodbye_fade_ready_cb(lv_anim_t* a)
{
    if (!a || !a->var) return;
    lv_obj_del((lv_obj_t*)a->var);
}

static void start_goodbye_fade_in(lv_obj_t* screen)
{
    if (!screen) return;

    lv_obj_t* overlay = lv_obj_create(screen);
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_move_foreground(overlay);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, overlay);
    lv_anim_set_exec_cb(&a, goodbye_fade_exec_cb);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_time(&a, 2000);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_ready_cb(&a, goodbye_fade_ready_cb);
    lv_anim_start(&a);
}

static void show_goodbye_screen_locked()
{
    lv_obj_t* screen = lv_scr_act();
    if (!screen) return;

    lv_obj_clean(screen);
    UiResetRuntimeState();
    lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);

    char time_text[12] = "--:--";
    char dist_text[16] = "--";

    SystemAPI* system = SystemAPI::getInstance();
    if (system) {
        ObdData snapshot = {};
        if (system->GetObdDataSnapshot(&snapshot, pdMS_TO_TICKS(20))) {
            uint32_t totalMin = snapshot.drive_time_sec / 60U;
            uint32_t hours = totalMin / 60U;
            uint32_t mins = totalMin % 60U;
            snprintf(time_text, sizeof(time_text), "%02u:%02u",
                     (unsigned int)hours, (unsigned int)mins);
            snprintf(dist_text, sizeof(dist_text), "%u",
                     (unsigned int)snapshot.trip_distance_km);
        }
    }

    create_goodbye_screen(time_text, dist_text);
    start_goodbye_fade_in(screen);

    lv_obj_invalidate(screen);
    lv_refr_now(lv_disp_get_default());
    TEST_LOG("Goodbye screen applied, children=%u", (unsigned int)lv_obj_get_child_cnt(screen));
    s_goodbyeScreenActive = true;
}

static void show_gauge_screen_locked(SystemAPI* system)
{
    lv_obj_t* screen = lv_scr_act();
    if (!screen) return;

    lv_obj_clean(screen);
    GaugeInit();
    if (system) {
        UiSharedState snap = {};
        if (system->GetUiSharedSnapshot(&snap, 0)) {
            apply_shared_ui_state(snap);
        }
    }
    lv_obj_invalidate(screen);
    s_goodbyeScreenActive = false;
}

static void splash_gif_event_cb(lv_event_t* e)
{
    if (!e) return;
    if (lv_event_get_code(e) == LV_EVENT_READY) {
        s_splashGifReady = true;
    }
}

static bool IRAM_ATTR on_vsync_callback_common(
    esp_lcd_panel_handle_t panel,
    void* user_ctx
) {
    LV_UNUSED(panel);
    SemaphoreHandle_t sem = (SemaphoreHandle_t)user_ctx;
    if (!sem) return false;
    s_vsyncCount++;
    BaseType_t high_task_wakeup = pdFALSE;
    xSemaphoreGiveFromISR(sem, &high_task_wakeup);
    return high_task_wakeup == pdTRUE;
}

#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
static bool IRAM_ATTR on_vsync_callback(
    esp_lcd_panel_handle_t panel,
    const esp_lcd_rgb_panel_event_data_t* edata,
    void* user_ctx
) {
    LV_UNUSED(edata);
    return on_vsync_callback_common(panel, user_ctx);
}
#else
static bool IRAM_ATTR on_vsync_callback(
    esp_lcd_panel_handle_t panel,
    esp_lcd_rgb_panel_event_data_t* edata,
    void* user_ctx
) {
    LV_UNUSED(edata);
    return on_vsync_callback_common(panel, user_ctx);
}
#endif

static void* sd_fs_open(lv_fs_drv_t * drv, const char * path, lv_fs_mode_t mode) {
    LV_UNUSED(drv);
    LV_UNUSED(mode);

    String fullPath = path;
    if (!fullPath.startsWith("/")) fullPath = "/" + fullPath;

    File f = SD.open(fullPath.c_str(), FILE_READ);
    if (!f || f.isDirectory()) {
        TEST_LOG("[FS] Open Failed: %s", fullPath.c_str());
        return NULL;
    }

    File* fp = new File(f);
    return (void*)fp;
}

static lv_fs_res_t sd_fs_close(lv_fs_drv_t * drv, void * file_p) {
    LV_UNUSED(drv);
    File* fp = (File*)file_p;
    fp->close();
    delete fp;
    return LV_FS_RES_OK;
}

static lv_fs_res_t sd_fs_read(lv_fs_drv_t * drv, void * file_p, void * buf, uint32_t btr, uint32_t * br) {
    LV_UNUSED(drv);
    File* fp = (File*)file_p;
    size_t read_size = fp->read((uint8_t*)buf, btr);
    if (br) *br = (uint32_t)read_size;

    return LV_FS_RES_OK;
}

static lv_fs_res_t sd_fs_seek(lv_fs_drv_t * drv, void * file_p, uint32_t pos, lv_fs_whence_t whence) {
    LV_UNUSED(drv);
    File* fp = (File*)file_p;

    if (whence == LV_FS_SEEK_SET) {
        fp->seek(pos);
    }
    else if (whence == LV_FS_SEEK_CUR) {
        fp->seek(fp->position() + pos);
    }
    else if (whence == LV_FS_SEEK_END) {
        fp->seek(fp->size() + pos);
    }

    return LV_FS_RES_OK;
}

static lv_fs_res_t sd_fs_tell(lv_fs_drv_t * drv, void * file_p, uint32_t * pos_p) {
    LV_UNUSED(drv);
    File* fp = (File*)file_p;
    *pos_p = fp->position();
    return LV_FS_RES_OK;
}

void DisplayMgr::Init()
{
    TEST_LINE();
    const uint32_t panelPclkHz = (DISPLAY_TARGET_PCLK_HZ > 0U) ? DISPLAY_TARGET_PCLK_HZ : ST7262_PANEL_CONFIG_TIMINGS_PCLK_HZ;
#ifdef GPIO_BCKL
    pinMode(GPIO_BCKL, OUTPUT);
    TEST_LOG("GPIO_BCKL configured");
#endif

#if defined(DISPLAY_RGB_BOUNCE_BUFFER_PIXELS)
    constexpr size_t bouncePixels = (size_t)DISPLAY_RGB_BOUNCE_BUFFER_PIXELS;
#else
    constexpr size_t bouncePixels = 0;
#endif

    this->rgbPanel = new Arduino_ESP32RGBPanel(
        ST7262_PANEL_CONFIG_DE_GPIO_NUM,
        ST7262_PANEL_CONFIG_VSYNC_GPIO_NUM,
        ST7262_PANEL_CONFIG_HSYNC_GPIO_NUM,
        ST7262_PANEL_CONFIG_PCLK_GPIO_NUM,
        ST7262_PANEL_CONFIG_DATA_GPIO_R0,
        ST7262_PANEL_CONFIG_DATA_GPIO_R1,
        ST7262_PANEL_CONFIG_DATA_GPIO_R2,
        ST7262_PANEL_CONFIG_DATA_GPIO_R3,
        ST7262_PANEL_CONFIG_DATA_GPIO_R4,
        ST7262_PANEL_CONFIG_DATA_GPIO_G0,
        ST7262_PANEL_CONFIG_DATA_GPIO_G1,
        ST7262_PANEL_CONFIG_DATA_GPIO_G2,
        ST7262_PANEL_CONFIG_DATA_GPIO_G3,
        ST7262_PANEL_CONFIG_DATA_GPIO_G4,
        ST7262_PANEL_CONFIG_DATA_GPIO_G5,
        ST7262_PANEL_CONFIG_DATA_GPIO_B0,
        ST7262_PANEL_CONFIG_DATA_GPIO_B1,
        ST7262_PANEL_CONFIG_DATA_GPIO_B2,
        ST7262_PANEL_CONFIG_DATA_GPIO_B3,
        ST7262_PANEL_CONFIG_DATA_GPIO_B4,
        ST7262_PANEL_CONFIG_TIMINGS_FLAGS_HSYNC_IDLE_LOW,
        ST7262_PANEL_CONFIG_TIMINGS_HSYNC_FRONT_PORCH,
        ST7262_PANEL_CONFIG_TIMINGS_HSYNC_PULSE_WIDTH,
        ST7262_PANEL_CONFIG_TIMINGS_HSYNC_BACK_PORCH,
        ST7262_PANEL_CONFIG_TIMINGS_FLAGS_VSYNC_IDLE_LOW,
        ST7262_PANEL_CONFIG_TIMINGS_VSYNC_FRONT_PORCH,
        ST7262_PANEL_CONFIG_TIMINGS_VSYNC_PULSE_WIDTH,
        ST7262_PANEL_CONFIG_TIMINGS_VSYNC_BACK_PORCH,
        ST7262_PANEL_CONFIG_TIMINGS_FLAGS_PCLK_ACTIVE_NEG,
        panelPclkHz,
        false,
        ST7262_PANEL_CONFIG_TIMINGS_FLAGS_DE_IDLE_HIGH,
        ST7262_PANEL_CONFIG_TIMINGS_FLAGS_PCLK_IDLE_HIGH,
        bouncePixels
    );

    if (!this->rgbPanel) {
        Serial.println("[DisplayMgr] Critical: rgbPanel allocation failed");
        return;
    }
    TEST_LOG("rgbPanel allocated: %p", this->rgbPanel);

    gfx = new Arduino_RGB_Display(SCREEN_WIDTH, SCREEN_HEIGHT, rgbPanel, 0, false);
    if (!gfx) {
        Serial.println("[DisplayMgr] Critical: gfx allocation failed");
        return;
    }
    TEST_LOG("gfx allocated: %p", gfx);

    TEST_LOG("RGB panel pclk=%u (base=%u derate=%u%%)",
             (unsigned int)panelPclkHz,
             (unsigned int)ST7262_PANEL_CONFIG_TIMINGS_PCLK_HZ,
             (unsigned int)DISPLAY_PCLK_DERATE_PERCENT);
    bool ok = gfx->begin(panelPclkHz);
    _gfxInitialized = ok;
    TEST_LOG("gfx->begin result=%d", ok ? 1 : 0);

    if (ok) {
        _fb_pixels = SCREEN_WIDTH * SCREEN_HEIGHT;
        size_t bufferSize = _fb_pixels * sizeof(uint16_t);
        _frontFbIndex = 0;
        _backFbIndex = 1;
        TEST_LOG("framebuffer pixels=%u bytes=%u",
                 (unsigned int)_fb_pixels, (unsigned int)bufferSize);

        _fb_buf[0] = (uint16_t*)gfx->getFramebuffer();
        _fb_buf[1] = nullptr;
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
        if (rgbPanel && rgbPanel->getPanelHandle()) {
            void* hwFb0 = nullptr;
            void* hwFb1 = nullptr;
            esp_err_t fbErr = esp_lcd_rgb_panel_get_frame_buffer(
                rgbPanel->getPanelHandle(), 2, &hwFb0, &hwFb1
            );
            if (fbErr == ESP_OK && hwFb0 && hwFb1 && hwFb0 != hwFb1) {
                _fb_buf[0] = (uint16_t*)hwFb0;
                _fb_buf[1] = (uint16_t*)hwFb1;
                Serial.println("[DisplayMgr] Hardware double-FB detected from RGB panel");
            } else {
                TEST_LOG("Hardware double-FB unavailable (err=%d, fb0=%p, fb1=%p)",
                         (int)fbErr, hwFb0, hwFb1);
            }
        }
#endif

        if (!_fb_buf[0]) {
            Serial.println("[DisplayMgr] Critical: front framebuffer is null");
            _gfxInitialized = false;
        } else {
            memset(_fb_buf[0], 0, bufferSize);
            if (_fb_buf[1]) {
                memset(_fb_buf[1], 0, bufferSize);
            }
        }

        if (_fb_buf[0]) {
            gfx->setFrameBuffer(_fb_buf[_frontFbIndex]);
        }

        if (!s_vsyncSem) {
            s_vsyncSem = xSemaphoreCreateBinary();
        }

        if (!s_vsyncSem) {
            Serial.println("[DisplayMgr] Warning: VSYNC semaphore creation failed");
        } else if (rgbPanel && rgbPanel->getPanelHandle()) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
            esp_lcd_rgb_panel_event_callbacks_t cbs = {};
            cbs.on_vsync = on_vsync_callback;
            esp_err_t err = esp_lcd_rgb_panel_register_event_callbacks(
                rgbPanel->getPanelHandle(), &cbs, s_vsyncSem
            );
            if (err == ESP_OK) {
                s_strictVsyncSync = true;
                Serial.println("[DisplayMgr] VSYNC callback registered (IDF5 API)");
            } else {
                TEST_LOG("VSYNC callback register failed: %d", (int)err);
            }
#else
            esp_rgb_panel_t* panel = __containerof(rgbPanel->getPanelHandle(), esp_rgb_panel_t, base);
            if (panel) {
                panel->on_frame_trans_done = on_vsync_callback;
                panel->user_ctx = s_vsyncSem;
                s_strictVsyncSync = false;
                Serial.println("[DisplayMgr] VSYNC callback registered (IDF4 internal)");
            } else {
                Serial.println("[DisplayMgr] VSYNC callback register failed: panel null");
            }
#endif
        }

        // Enable pseudo path only when the RGB driver actually provided two hardware frame buffers.
        s_usePseudoDoubleBuffer = (_fb_buf[1] != nullptr);
        s_backBufferSynced = true;
        TEST_LOG("Pseudo double-buffer: %s", s_usePseudoDoubleBuffer ? "ON" : "OFF");

        if (_gfxInitialized) {
            this->BacklightOn();
            TEST_LOG("backlight enabled");
        }
    } else {
        Serial.println("[DisplayMgr] Critical: gfx->begin failed");
    }

    BaseType_t taskRet = xTaskCreatePinnedToCore(
        DisplayMgr::Subscribe,
        "DisplaySub",
        6144,
        this,
        4,
        &this->_eventTaskHandler,
        1
    );
    if (taskRet != pdPASS) {
        Serial.println("[DisplayMgr] Critical: DisplaySub task create failed");
    } else {
        TEST_LOG("DisplaySub task started");
    }
}

void DisplayMgr::StartLVGL() {
    TEST_LINE();
    if (_lvglInitialized) {
        TEST_LOG("already initialized");
        return;
    }
    if (!_gfxInitialized || !gfx) {
        Serial.println("[DisplayMgr] StartLVGL skipped: gfx is not initialized");
        return;
    }

    register_idle_hooks_once();

    lv_init();
    TEST_LOG("lv_init done");

    // Use a larger draw chunk first, then fallback to smaller chunks if allocation fails.
    static lv_color_t* sram_work_buf1 = nullptr;
    static lv_color_t* sram_work_buf2 = nullptr;
    static uint32_t sram_lines = 0;
    if (!sram_work_buf1 || !sram_work_buf2) {
        const uint32_t lineOptions[] = {180, 160, 140, 112};
        for (size_t i = 0; i < (sizeof(lineOptions) / sizeof(lineOptions[0])); ++i) {
            uint32_t lines = lineOptions[i];
            size_t bufSize = SCREEN_WIDTH * lines * sizeof(lv_color_t);

            lv_color_t* b1 = (lv_color_t*)heap_caps_malloc(
                bufSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA
            );
            lv_color_t* b2 = (lv_color_t*)heap_caps_malloc(
                bufSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA
            );
            if (!b1) b1 = (lv_color_t*)heap_caps_malloc(bufSize, MALLOC_CAP_SPIRAM);
            if (!b2) b2 = (lv_color_t*)heap_caps_malloc(bufSize, MALLOC_CAP_SPIRAM);

            if (b1 && b2) {
                sram_work_buf1 = b1;
                sram_work_buf2 = b2;
                sram_lines = lines;
                break;
            }

            if (b1) heap_caps_free(b1);
            if (b2) heap_caps_free(b2);
        }
    }

    if (!sram_work_buf1 || !sram_work_buf2 || sram_lines == 0) {
        TEST_LOG("Critical: LVGL draw buffer alloc failed (%p, %p)",
                 sram_work_buf1, sram_work_buf2);
        return;
    }
    lv_disp_draw_buf_init(&_draw_buf, sram_work_buf1, sram_work_buf2, SCREEN_WIDTH * sram_lines);
    TEST_LOG("lv_disp_draw_buf_init done, lines=%u", (unsigned int)sram_lines);

    lv_disp_drv_init(&_disp_drv);
    _disp_drv.hor_res = SCREEN_WIDTH;
    _disp_drv.ver_res = SCREEN_HEIGHT;
    _disp_drv.flush_cb = DisplayMgr::lvgl_flush_cb;
    _disp_drv.draw_buf = &_draw_buf;
    _disp_drv.user_data = this;
    _disp_drv.full_refresh = 0;
    _disp_drv.direct_mode = 0;
    _disp_drv.antialiasing = 0;

    lv_disp_drv_register(&_disp_drv);
    TEST_LOG("lv_disp_drv_register done");

    static lv_fs_drv_t fs_drv;
    lv_fs_drv_init(&fs_drv);
    fs_drv.letter = 'S';
    fs_drv.open_cb = sd_fs_open;
    fs_drv.close_cb = sd_fs_close;
    fs_drv.read_cb = sd_fs_read;
    fs_drv.seek_cb = sd_fs_seek;
    fs_drv.tell_cb = sd_fs_tell;
    fs_drv.user_data = this;
    lv_fs_drv_register(&fs_drv);
    TEST_LOG("lv_fs_drv_register done");

    #if LV_USE_PNG
    lv_png_init();
    #endif

    _lvglInitialized = true;
    Serial.println("[DisplayMgr] LVGL Started with SRAM Partial Draw Buffer + VSYNC sync");

    size_t total_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    TEST_LOG("[PSRAM] Total: %d, Free: %d, Used: %d bytes",
             total_psram, free_psram, total_psram - free_psram);
}

void DisplayMgr::lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p)
{
    DisplayMgr* self = (DisplayMgr*)drv->user_data;

    if (!self || !self->gfx) {
        lv_disp_flush_ready(drv);
        return;
    }

    if (self && self->gfx) {
        if (!s_usePseudoDoubleBuffer) {
            // Single-FB path: align write burst to frame boundary.
            if (!s_flushFrameStarted && s_vsyncSem) {
                uint32_t before = s_vsyncCount;
                xSemaphoreTake(s_vsyncSem, 0);
                TickType_t t0 = xTaskGetTickCount();
                while (s_vsyncCount == before &&
                       (xTaskGetTickCount() - t0) < pdMS_TO_TICKS(30)) {
                    xSemaphoreTake(s_vsyncSem, pdMS_TO_TICKS(2));
                }
                s_flushFrameStarted = true;
            }

            uint16_t* fb = self->_fb_buf[0];
            if (fb) {
                uint32_t w = lv_area_get_width(area);
                uint32_t h = lv_area_get_height(area);
                uint16_t* src = (uint16_t*)color_p;
                size_t rowBytes = w * sizeof(uint16_t);
                uint32_t copyStartUs = (uint32_t)esp_timer_get_time();
                if (!DISPLAY_ROTATE_180) {
                    bool fullWidth = (area->x1 == 0) && (w == SCREEN_WIDTH);
                    uintptr_t firstRow = 0;
                    if (fullWidth && h > 0) {
                        size_t firstOff = (size_t)area->y1 * SCREEN_WIDTH;
                        firstRow = (uintptr_t)(&fb[firstOff]);
                    }

                    for (uint32_t y = 0; y < h; ++y) {
                        size_t dstOff = (size_t)(area->y1 + y) * SCREEN_WIDTH + (size_t)area->x1;
                        uint16_t* dst = &fb[dstOff];
                        memcpy(dst, &src[y * w], rowBytes);
                        if (!fullWidth) {
                            cache_writeback_span(dst, rowBytes);
                        }
                    }

                    if (fullWidth && firstRow) {
                        cache_writeback_span((const void*)firstRow, rowBytes * h);
                    }
                } else {
                    lv_area_t dstArea = rotate_area_180(area);
                    for (uint32_t sy = 0; sy < h; ++sy) {
                        uint32_t dy = (uint32_t)dstArea.y1 + (h - 1 - sy);
                        uint16_t* dst = &fb[(size_t)dy * SCREEN_WIDTH + (size_t)dstArea.x1];
                        uint16_t* d = dst + (w - 1);
                        const uint16_t* s = &src[sy * w];
                        for (uint32_t sx = 0; sx < w; ++sx) {
                            *d-- = *s++;
                        }
                        cache_writeback_span(dst, rowBytes);
                    }
                }

                uint32_t copyUs = (uint32_t)((uint64_t)esp_timer_get_time() - (uint64_t)copyStartUs);
                s_singleFlushAreaCount++;
                s_singleFlushPixelCount += (w * h);
                s_singleFlushCopyTimeUs += copyUs;
                if (copyUs > s_singleFlushMaxCopyUs) {
                    s_singleFlushMaxCopyUs = copyUs;
                }
            } else if (!s_loggedNullFrontFb) {
                s_loggedNullFrontFb = true;
                Serial.println("[DisplayMgr] Warning: front FB is null in flush");
            }

            if (lv_disp_flush_is_last(drv)) {
                s_swapReqCount++;
                s_swapDoneCount++;
                s_singleFlushFrameCount++;
                s_flushFrameStarted = false;
            }

            lv_disp_flush_ready(drv);
            return;
        }

        if (!s_flushFrameStarted) {
            s_flushFrameStarted = true;
            s_frameHasFullRefreshArea = false;
            s_dirtyAreaCount = 0;
        }

        bool isFullFrame =
            (area->x1 == 0) &&
            (area->y1 == 0) &&
            (area->x2 == (SCREEN_WIDTH - 1)) &&
            (area->y2 == (SCREEN_HEIGHT - 1));
        lv_area_t writeArea = DISPLAY_ROTATE_180 ? rotate_area_180(area) : *area;

        if (isFullFrame) {
            s_frameHasFullRefreshArea = true;
            // No need to keep dirty list for full-frame redraw.
            s_dirtyAreaCount = 0;
        } else {
            // If the back buffer is stale and we render only partial areas,
            // recover once by mirroring front->back.
            if (!s_backBufferSynced) {
                uint16_t* front = self->_fb_buf[self->_frontFbIndex];
                uint16_t* back = self->_fb_buf[self->_backFbIndex];
                if (front && back) {
                    memcpy(back, front, self->_fb_pixels * sizeof(uint16_t));
                    cache_writeback_span(back, self->_fb_pixels * sizeof(uint16_t));
                }
                s_backBufferSynced = true;
            }

            // Store dirty area for replay; merge fragments to reduce copy cost.
            add_dirty_area_merged(&writeArea);
        }

        uint16_t* fb = self->_fb_buf[self->_backFbIndex];
        if (fb) {
            uint32_t w = lv_area_get_width(area);
            uint32_t h = lv_area_get_height(area);
            uint16_t* src = (uint16_t*)color_p;
            if (!DISPLAY_ROTATE_180) {
                for (uint32_t y = 0; y < h; ++y) {
                    size_t dstOff = (size_t)(area->y1 + y) * SCREEN_WIDTH + (size_t)area->x1;
                    memcpy(&fb[dstOff], &src[y * w], w * sizeof(uint16_t));
                }
            } else {
                for (uint32_t sy = 0; sy < h; ++sy) {
                    uint32_t dy = (uint32_t)writeArea.y1 + (h - 1 - sy);
                    uint16_t* dst = &fb[(size_t)dy * SCREEN_WIDTH + (size_t)writeArea.x1];
                    uint16_t* d = dst + (w - 1);
                    const uint16_t* s = &src[sy * w];
                    for (uint32_t sx = 0; sx < w; ++sx) {
                        *d-- = *s++;
                    }
                }
            }
            cache_writeback_area(fb, &writeArea);
        } else if (!s_loggedNullBackFb) {
            s_loggedNullBackFb = true;
            Serial.println("[DisplayMgr] Warning: back FB is null in flush");
        }

        if (lv_disp_flush_is_last(drv)) {
            s_swapReqCount++;
            // Swap only after observing a new VSYNC edge.
            bool sawVsync = true;
            if (s_vsyncSem) {
                uint32_t before = s_vsyncCount;
                xSemaphoreTake(s_vsyncSem, 0); // drain stale token

                TickType_t waitBudget = s_strictVsyncSync ? pdMS_TO_TICKS(25) : pdMS_TO_TICKS(30);
                TickType_t t0 = xTaskGetTickCount();
                while (s_vsyncCount == before &&
                       (xTaskGetTickCount() - t0) < waitBudget) {
                    xSemaphoreTake(s_vsyncSem, pdMS_TO_TICKS(2));
                }
                sawVsync = (s_vsyncCount != before);
            }

            if (!sawVsync) {
                s_vsyncTimeoutConsecutive++;
                bool forceSwapAfterSkip = (!s_strictVsyncSync) &&
                    (s_vsyncTimeoutConsecutive >= VSYNC_TIMEOUT_SKIP_BEFORE_FORCE_SWAP);

                if (!forceSwapAfterSkip) {
                    // Skip this frame to avoid out-of-phase swap and visible horizontal noise.
                    s_swapSkipTimeoutCount++;
                    uint16_t* front = self->_fb_buf[self->_frontFbIndex];
                    uint16_t* back = self->_fb_buf[self->_backFbIndex];
                    if (front && back) {
                        memcpy(back, front, self->_fb_pixels * sizeof(uint16_t));
                        cache_writeback_span(back, self->_fb_pixels * sizeof(uint16_t));
                    }
                    s_backBufferSynced = true;
                    s_dirtyAreaCount = 0;
                    s_flushFrameStarted = false;
                    lv_disp_flush_ready(drv);
                    return;
                }

                // Keep pipeline alive on repeated timeout bursts.
                s_swapForcedOnTimeoutCount++;
                s_vsyncTimeoutConsecutive = 0;
            } else {
                s_vsyncTimeoutConsecutive = 0;
            }

            uint16_t* swapFb = self->_fb_buf[self->_backFbIndex];
            if (!swapFb) {
                swapFb = self->_fb_buf[self->_frontFbIndex];
            }
            if (!swapFb) {
                lv_disp_flush_ready(drv);
                return;
            }

            self->gfx->setFrameBuffer(swapFb);
            self->gfx->flush();
            s_swapDoneCount++;

            uint8_t oldFront = self->_frontFbIndex;
            self->_frontFbIndex = self->_backFbIndex;
            self->_backFbIndex = oldFront;

            if (s_frameHasFullRefreshArea) {
                // Keep stale back buffer during continuous full-frame streams (e.g. GIF),
                // and resync only when partial update appears later.
                s_backBufferSynced = false;
            } else {
                // Replay this frame's dirty regions into the new back buffer so
                // next partial frame starts from identical content without full copy.
                uint32_t dirtyPixels = 0;
                for (uint8_t i = 0; i < s_dirtyAreaCount; ++i) {
                    dirtyPixels += area_pixels(&s_dirtyAreas[i]);
                }
                const uint32_t replayThresholdPixels = (SCREEN_WIDTH * SCREEN_HEIGHT * 35U) / 100U;
                if (dirtyPixels > replayThresholdPixels) {
                    // Too much replay work: defer and resync lazily on next partial frame.
                    s_backBufferSynced = false;
                    s_replayBypassCount++;
                    s_flushFrameStarted = false;
                    lv_disp_flush_ready(drv);
                    return;
                }

                uint16_t* front = self->_fb_buf[self->_frontFbIndex];
                uint16_t* back = self->_fb_buf[self->_backFbIndex];
                if (front && back) {
                    for (uint8_t i = 0; i < s_dirtyAreaCount; ++i) {
                        const lv_area_t* a = &s_dirtyAreas[i];
                        uint32_t w = lv_area_get_width(a);
                        uint32_t h = lv_area_get_height(a);
                        for (uint32_t y = 0; y < h; ++y) {
                            size_t off = (size_t)(a->y1 + y) * SCREEN_WIDTH + (size_t)a->x1;
                            memcpy(&back[off], &front[off], w * sizeof(uint16_t));
                        }
                        cache_writeback_area(back, a);
                    }
                }
                s_backBufferSynced = true;
            }

            s_flushFrameStarted = false;
        }
    }

    lv_disp_flush_ready(drv);
}

bool DisplayMgr::PlayGifFromSD(const char* path)
{
    TEST_LOG("PlayGifFromSD path=%s", path ? path : "(null)");
    if (!_lvglInitialized || path == nullptr || path[0] == '\0') return false;

    String lvPath(path);
    String sdPath = lvPath;
    if (sdPath.startsWith("S:")) {
        sdPath = sdPath.substring(2);
    }
    if (!sdPath.startsWith("/")) {
        sdPath = "/" + sdPath;
    }

    if (!SD.exists(sdPath.c_str())) {
        TEST_LOG("GIF not found on SD: %s", sdPath.c_str());
        return false;
    }

    lv_obj_t* screen = lv_scr_act();
    if (_splashGif != nullptr) {
        lv_obj_del(_splashGif);
        _splashGif = nullptr;
    }

    _splashGif = lv_gif_create(screen);
    if (_splashGif == nullptr) {
        Serial.println("[DisplayMgr] lv_gif_create failed");
        return false;
    }
    lv_obj_add_event_cb(_splashGif, splash_gif_event_cb, LV_EVENT_READY, nullptr);

    lv_gif_set_src(_splashGif, lvPath.c_str());
#if LV_USE_GIF
    lv_timer_set_period(((lv_gif_t*)_splashGif)->timer, 1);
#endif
    lv_obj_center(_splashGif);
    TEST_LOG("GIF started: %s", lvPath.c_str());
    return true;
}

bool DisplayMgr::PlayGifFromMemory(const GIFMemory& gifMem)
{
    TEST_LOG("PlayGifFromMemory size=%u", (unsigned int)gifMem.size);
    if (!_lvglInitialized || gifMem.data == nullptr || gifMem.size == 0) return false;

    lv_obj_t* screen = lv_scr_act();
    if (_splashGif != nullptr) {
        lv_obj_del(_splashGif);
        _splashGif = nullptr;
    }

    _splashGif = lv_gif_create(screen);
    if (_splashGif == nullptr) {
        Serial.println("[DisplayMgr] lv_gif_create failed");
        return false;
    }
    lv_obj_add_event_cb(_splashGif, splash_gif_event_cb, LV_EVENT_READY, nullptr);

    _splashGifDsc.header.always_zero = 0;
    _splashGifDsc.header.cf = LV_IMG_CF_RAW;
    _splashGifDsc.header.w = 1;
    _splashGifDsc.header.h = 1;
    _splashGifDsc.data_size = gifMem.size;
    _splashGifDsc.data = gifMem.data;

    lv_gif_set_src(_splashGif, &_splashGifDsc);
#if LV_USE_GIF
    lv_timer_set_period(((lv_gif_t*)_splashGif)->timer, 3);
#endif
    lv_obj_center(_splashGif);
    TEST_LOG("GIF started from PSRAM: %u bytes", (unsigned int)gifMem.size);
    return true;
}

void DisplayMgr::PlayGifTask(void* pvParameters)
{
    DisplayMgr* self = static_cast<DisplayMgr*>(pvParameters);
    SystemAPI* system = SystemAPI::getInstance();
    TEST_LINE();
    if (!self || !system) {
        Serial.println("[DisplayMgr] Critical: PlayGifTask self/system null");
        vTaskDelete(NULL);
        return;
    }
    s_splashActive = true;

    if (!self->_gfxInitialized || !self->gfx || !self->_fb_buf[self->_frontFbIndex]) {
        Serial.println("[DisplayMgr] Critical: PlayGifTask entered without valid gfx/front FB");
        s_splashActive = false;
        vTaskDelete(NULL);
        return;
    }

    self->gfx->setFrameBuffer(self->_fb_buf[self->_frontFbIndex]);
    self->gfx->fillScreen(0x0000);
    self->gfx->flush();
    TEST_LOG("splash pre-clear done");

    self->StartLVGL();
    if (!self->_lvglInitialized) {
        Serial.println("[DisplayMgr] Critical: StartLVGL failed in PlayGifTask");
        s_splashActive = false;
        vTaskDelete(NULL);
        return;
    }

    bool splashStarted = false;
    if (system->LockLvgl(pdMS_TO_TICKS(100))) {
        TEST_LOG("LockLvgl acquired for splash start");
        lv_obj_t* screen = lv_scr_act();
        lv_obj_clean(screen);
        lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

        GIFMemory gifMem;
        if (system->LockGif(pdMS_TO_TICKS(100))) {
            GIFMemory* shared = system->GetPsramObjPtr();
            if (shared && shared->data && shared->size) {
                gifMem = *shared;
            }
            system->UnlockGif();
        }

        if (gifMem.data && gifMem.size) {
            splashStarted = self->PlayGifFromMemory(gifMem);
        } else {
            splashStarted = self->PlayGifFromSD(self->_pendingGifPath.c_str());
        }
        lv_obj_invalidate(screen);

        system->UnlockLvgl();
        TEST_LOG("LockLvgl released after splash start");
    } else {
        TEST_LOG("LockLvgl timeout at splash start");
    }

    if (!splashStarted) {
        TEST_LOG("GIF start failed: %s", self->_pendingGifPath.c_str());
    }

    s_splashGifReady = false;
    TickType_t startTick = xTaskGetTickCount();
    const TickType_t minSplashDuration = pdMS_TO_TICKS(1200);
    const TickType_t maxSplashDuration = pdMS_TO_TICKS(12000);
    while (true) {
        if (ulTaskNotifyTake(pdTRUE, 0)) break;

        bool splashReady = false;
        if (system->LockLvgl(pdMS_TO_TICKS(20))) {
            lv_timer_handler();
            splashReady = s_splashGifReady;
            system->UnlockLvgl();
        }

        TickType_t elapsed = xTaskGetTickCount() - startTick;
        if (splashReady && elapsed >= minSplashDuration) {
            Serial.println("[DisplayMgr] Splash finished by GIF READY");
            break;
        }
        if (elapsed >= maxSplashDuration) {
            Serial.println("[DisplayMgr] Splash finished by timeout");
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(2));
    }

    bool uiReady = false;
    for (int i = 0; i < 30; ++i) {
        if(system->LockLvgl(pdMS_TO_TICKS(100))) {
            TEST_LOG("LockLvgl acquired for UI transition, try=%d", i);
            lv_obj_t* screen = lv_scr_act();
            if (self->_splashGif) {
                lv_obj_del(self->_splashGif);
                self->_splashGif = nullptr;
            }
            show_gauge_screen_locked(system);

            lv_obj_invalidate(screen);
            // Force at least one render pass before leaving splash task.
            lv_timer_handler();
            system->UnlockLvgl();
            Serial.println("[DisplayMgr] LVGL UI Created");
            uiReady = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    if (!uiReady) {
        Serial.println("[DisplayMgr] LVGL UI transition timeout");
    }

    // Reclaim PSRAM used by splash GIF as soon as splash phase is done.
    system->storageSubscriber.SetEvent(STORAGE_CLEAR_LOADED_PSRAM);
    // Allow monitor updates immediately; UI layer handles startup ceremony blending.
    s_monitorResumeTick = 0;

    // Reuse GifTask as the persistent LVGL task to avoid dynamic task allocation failures.
    self->_lvglTaskHandler = xTaskGetCurrentTaskHandle();
    self->_gifTaskHandler = nullptr;
    self->_splashFinished = true;
    s_splashActive = false;
    vTaskPrioritySet(self->_lvglTaskHandler, 4);
    Serial.println("[DisplayMgr] Reusing GifTask as LvglTask");
    DisplayMgr::HandleLvglTask(self);
}

void DisplayMgr::Subscribe(void* pvParameters)
{
    DisplayMgr* self = static_cast<DisplayMgr*>(pvParameters);
    SystemAPI* system = SystemAPI::getInstance();
    DisplayEventData event;

    while(true) {
        if(system->displaySubscriber.ReceiveEvent(&event, portMAX_DELAY)) {
            switch(event.type) {
                case DISPLAY_SHOW_SPLASH:
                {
                    self->_pendingGifPath = String("S:") + String(event.data);
                    TEST_LOG("DISPLAY_SHOW_SPLASH path=%s", self->_pendingGifPath.c_str());

                    if (self->_gifTaskHandler == nullptr) {
                        TEST_LOG("Starting LVGL GIF Task: %s", self->_pendingGifPath.c_str());
                        BaseType_t ret = xTaskCreatePinnedToCore(
                            DisplayMgr::PlayGifTask, "GifTask", GIF_TASK_STACK_WORDS, self, 5, &self->_gifTaskHandler, 1
                        );
                        if (ret != pdPASS) {
                            Serial.println("[DisplayMgr] Critical: GifTask create failed");
                        } else {
                            TEST_LOG("GifTask created");
                        }
                    }
                    break;
                }
                case DISPLAY_SHOW_GOODBYE:
                {
                    TEST_LOG("DISPLAY_SHOW_GOODBYE");
                    bool applied = false;
                    for (int i = 0; i < 3; ++i) {
                        if (system->LockLvgl(pdMS_TO_TICKS(120))) {
                            show_goodbye_screen_locked();
                            lv_timer_handler();
                            system->UnlockLvgl();
                            applied = true;
                            break;
                        }
                        vTaskDelay(pdMS_TO_TICKS(20));
                    }
                    if (!applied) {
                        TEST_LOG("DISPLAY_SHOW_GOODBYE lock timeout");
                    }
                    break;
                }
                case DISPLAY_SHOW_GAUGE_REBOOT:
                {
                    TEST_LOG("DISPLAY_SHOW_GAUGE_REBOOT");
                    if (system->LockLvgl(pdMS_TO_TICKS(180))) {
                        show_gauge_screen_locked(system);
                        lv_timer_handler();
                        system->UnlockLvgl();
                    } else {
                        TEST_LOG("DISPLAY_SHOW_GAUGE_REBOOT lock timeout");
                    }
                    break;
                }
                case DISPLAY_UPDATE_OBD_STATUS:
                    self->Println("OBD: " + String(event.data));
                    break;
                case DISPLAY_UPDATE_VOLTAGE:
                    self->Println("Volt: " + String(event.data));
                    break;
                case DISPLAY_UPDATE_COOLANT:
                    self->Println("Cool: " + String(event.data));
                    break;
                case DISPLAY_UPDATE_CPU_USAGE:
                    self->Println("CPU: " + String(event.data));
                    break;
                case DISPLAY_UPDATE_RAM_USAGE:
                    self->Println("RAM: " + String(event.data));
                    break;
                default:
                    break;
            }
        }
    }
}

void DisplayMgr::HandleLvglTask(void *pvParameters)
{
    DisplayMgr* self = static_cast<DisplayMgr*>(pvParameters);
    SystemAPI* system = SystemAPI::getInstance();
    TEST_LINE();
    if (!self || !system) {
        Serial.println("[DisplayMgr] Critical: HandleLvglTask self/system null");
        vTaskDelete(NULL);
        return;
    }
    TickType_t xLastWakeTime = xTaskGetTickCount();
    TickType_t lastMonTick = xLastWakeTime;
    TickType_t lastUiTick = xLastWakeTime;
    TickType_t lastSyncLogTick = xLastWakeTime;
    uint32_t prevReq = 0;
    uint32_t prevDone = 0;
    uint32_t prevSkip = 0;
    uint32_t prevForced = 0;
    uint32_t prevBypass = 0;
    uint32_t prevVsync = 0;
    uint32_t prevSingleArea = 0;
    uint32_t prevSingleFrame = 0;
    uint32_t prevSinglePixel = 0;
    uint64_t prevSingleCopyUs = 0;
    Serial.println("[DisplayMgr] LvglTask started");

    while (true) {
        if (system->LockLvgl(pdMS_TO_TICKS(5))) {
            lv_timer_handler();

            TickType_t now = xTaskGetTickCount();
            if (!s_splashActive &&
                (now - lastUiTick) >= UI_SHARED_UPDATE_PERIOD_TICKS) {
                UiSharedState snap = {};
                if (system->GetUiSharedSnapshot(&snap, 0)) {
                    apply_shared_ui_state(snap);
                }
                lastUiTick = now;
            }

            if (!s_splashActive &&
                (now - lastMonTick) >= MONITOR_UPDATE_PERIOD_TICKS &&
                (s_monitorResumeTick == 0 || now >= s_monitorResumeTick)) {
                // if (lv_anim_count_running() == 0) {
                    size_t total = heap_caps_get_total_size(MALLOC_CAP_8BIT);
                    size_t free = heap_caps_get_free_size(MALLOC_CAP_8BIT);
                    int32_t ram = (total > 0) ? (int32_t)(((total - free) * 100U) / total) : 0;

                    uint8_t core0 = 0;
                    uint8_t core1 = 0;
                    if (sample_cpu_usage(&core0, &core1)) {
                        update_system_monitor(ram, core0, core1);
                    } else {
                        update_system_monitor(ram, 0, 0);
                    }
                    lastMonTick = now;
                // }
            }

            if ((now - lastSyncLogTick) >= pdMS_TO_TICKS(2000)) {
                uint32_t req = s_swapReqCount;
                uint32_t done = s_swapDoneCount;
                uint32_t skip = s_swapSkipTimeoutCount;
                uint32_t bypass = s_replayBypassCount;
                uint32_t forced = s_swapForcedOnTimeoutCount;
                uint32_t vs = s_vsyncCount;
                uint32_t sfArea = s_singleFlushAreaCount;
                uint32_t sfFrame = s_singleFlushFrameCount;
                uint32_t sfPixel = s_singleFlushPixelCount;
                uint64_t sfCopyUs = s_singleFlushCopyTimeUs;
                uint32_t sfMaxUs = s_singleFlushMaxCopyUs;
                // Serial.printf("[DisplayMgr][SYNC] req:+%u done:+%u skip:+%u forced:+%u bypass:+%u vsync:+%u\n",
                //               (unsigned int)(req - prevReq),
                //               (unsigned int)(done - prevDone),
                //               (unsigned int)(skip - prevSkip),
                //               (unsigned int)(forced - prevForced),
                //               (unsigned int)(bypass - prevBypass),
                //               (unsigned int)(vs - prevVsync));
                // Serial.printf("[DisplayMgr][FLUSH1] mode:%s frame:+%u area:+%u px:+%u copy_us:+%llu max_us:%u\n",
                //               s_usePseudoDoubleBuffer ? "DBL" : "SGL",
                //               (unsigned int)(sfFrame - prevSingleFrame),
                //               (unsigned int)(sfArea - prevSingleArea),
                //               (unsigned int)(sfPixel - prevSinglePixel),
                //               (unsigned long long)(sfCopyUs - prevSingleCopyUs),
                //               (unsigned int)sfMaxUs);
                prevReq = req;
                prevDone = done;
                prevSkip = skip;
                prevForced = forced;
                prevBypass = bypass;
                prevVsync = vs;
                prevSingleArea = sfArea;
                prevSingleFrame = sfFrame;
                prevSinglePixel = sfPixel;
                prevSingleCopyUs = sfCopyUs;
                s_singleFlushMaxCopyUs = 0;
                lastSyncLogTick = now;
            }

            system->UnlockLvgl();
        }

        vTaskDelayUntil(&xLastWakeTime, LVGL_TASK_PERIOD_TICKS);
    }
}

void DisplayMgr::BacklightOn() { digitalWrite(GPIO_BCKL, HIGH); }
void DisplayMgr::BacklightOff() { digitalWrite(GPIO_BCKL, LOW); }
void DisplayMgr::Println(const String& text) { PushLine(text); if (_gfxInitialized) Redraw(); }
void DisplayMgr::Printf(const String& text) { AppendToLastLine(text); if (_gfxInitialized) Redraw(); }
void DisplayMgr::PushLine(const String& line) { _lines.push_back(line); if (_lines.size() > CONSOLE_ROWS) _lines.erase(_lines.begin()); }
void DisplayMgr::AppendToLastLine(const String& text) { if (_lines.empty()) _lines.push_back(text); else _lines.back() += text; }

void DisplayMgr::Redraw() {
    if (_lvglInitialized) return;
    if (!_gfxInitialized) return;
    gfx->fillScreen(RGB565_BLACK);
    gfx->setTextColor(RGB565_WHITE);
    gfx->setTextSize(2);
    int y = 1;
    for (const String& line : _lines) {
        gfx->setCursor(1, y);
        gfx->print(line);
        y += 20;
    }
    gfx->flush();
}

void DisplayMgr::Clear() { _lines.clear(); if (_gfxInitialized) gfx->fillScreen(RGB565_BLACK); }
void DisplayMgr::StopGif() { if (this->_gifTaskHandler) xTaskNotifyGive(this->_gifTaskHandler); }

void DisplayMgr::CreateMainBackground()
{
}
