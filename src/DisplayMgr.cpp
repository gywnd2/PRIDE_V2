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
#if defined(BOARD_HAS_TOUCH)
#if !defined(TOUCH_MODULES_GT911)
#define TOUCH_MODULES_GT911
#endif
#include <Wire.h>
#include <TouchLib.h>
#endif
#if defined(CONFIG_IDF_TARGET_ESP32S3)
#include "esp32s3/rom/cache.h"
extern "C" int Cache_WriteBack_Addr(uint32_t addr, uint32_t size);
#endif
#include <AnimatedGIF.h>

#define TEST_LOG(fmt, ...) UartLogf("[DisplayMgr] " fmt "\n", ##__VA_ARGS__)
#define TEST_LINE() UartLogf("[DisplayMgr] %s\n", __func__)

static constexpr uint32_t GIF_TASK_STACK_WORDS = 8192;
static constexpr UBaseType_t SPLASH_GIF_TASK_PRIORITY = 6;
static constexpr UBaseType_t LVGL_REUSED_TASK_PRIORITY = 4;

static SemaphoreHandle_t s_vsyncSem = nullptr;
static volatile uint32_t s_vsyncCount = 0;
static volatile uint32_t s_swapReqCount = 0;
static volatile uint32_t s_swapDoneCount = 0;
static volatile uint32_t s_swapSkipTimeoutCount = 0;
static volatile uint32_t s_replayBypassCount = 0;
static volatile uint32_t s_panelRestartCount = 0;
static volatile uint32_t s_singleFlushAreaCount = 0;
static volatile uint32_t s_singleFlushFrameCount = 0;
static volatile uint32_t s_singleFlushPixelCount = 0;
static volatile uint64_t s_singleFlushCopyTimeUs = 0;
static volatile uint32_t s_singleFlushMaxCopyUs = 0;
static bool s_flushFrameStarted = false;
static bool s_usePseudoDoubleBuffer = false;
static bool s_backBufferSynced = true;
static bool s_frameHasFullRefreshArea = false;
static bool s_frameStartedFromSceneReset = false;
static lv_area_t s_dirtyAreas[64];
static uint8_t s_dirtyAreaCount = 0;
static bool s_sceneResetPending = false;
static volatile uint32_t s_idleLoopCount[2] = {0, 0};
static bool s_idleHooksRegistered = false;
static uint32_t s_cpuPrevIdle0 = 0;
static uint32_t s_cpuPrevIdle1 = 0;
static uint32_t s_cpuPeakIdle0 = 1;
static uint32_t s_cpuPeakIdle1 = 1;
static TickType_t s_cpuPrevSampleTick = 0;
static uint8_t s_cpuFilteredUsage0 = 0;
static uint8_t s_cpuFilteredUsage1 = 0;
static bool s_cpuUsageValid = false;
static uint8_t s_cpuWarmupSamples = 3;
static volatile bool s_splashActive = false;
static volatile bool s_splashGifReady = false;
static volatile bool s_goodbyeScreenActive = false;
static bool s_strictVsyncSync = false;
static TickType_t s_monitorResumeTick = 0;
static bool s_loggedNullFrontFb = false;
static bool s_loggedNullBackFb = false;
static uint8_t s_vsyncTimeoutConsecutive = 0;
static bool s_forceFullInvalidatePending = false;
static uint32_t s_diagLastLogMs = 0;
static uint32_t s_successfulPseudoSwapFrameCount = 0;

static constexpr TickType_t MONITOR_UPDATE_PERIOD_TICKS = pdMS_TO_TICKS(1000);
static constexpr TickType_t UI_SHARED_UPDATE_PERIOD_TICKS = pdMS_TO_TICKS(100);
static constexpr uint32_t LVGL_TASK_MIN_SLEEP_MS = 1U;
static constexpr uint32_t LVGL_TASK_MAX_SLEEP_MS = 4U;
static constexpr uint32_t LVGL_TASK_LOCK_FAIL_SLEEP_MS = 2U;
#ifndef DISPLAY_PERIODIC_FULL_SYNC_INTERVAL
#define DISPLAY_PERIODIC_FULL_SYNC_INTERVAL 120U
#endif
static constexpr uint32_t PERIODIC_FULL_SYNC_INTERVAL_FRAMES = DISPLAY_PERIODIC_FULL_SYNC_INTERVAL;
static constexpr uint8_t VSYNC_TIMEOUT_RESTART_THRESHOLD = 3;
static constexpr uint32_t SPLASH_ANIMATED_GIF_SPEED_FACTOR = 10;
#ifndef DISPLAY_PCLK_DERATE_PERCENT
#define DISPLAY_PCLK_DERATE_PERCENT 90U
#endif
static constexpr uint32_t DISPLAY_TARGET_PCLK_HZ =
    (uint32_t)(((uint64_t)ST7262_PANEL_CONFIG_TIMINGS_PCLK_HZ * (uint64_t)DISPLAY_PCLK_DERATE_PERCENT) / 100ULL);
static constexpr uint32_t DISPLAY_TOTAL_H_PIXELS =
    (uint32_t)ST7262_PANEL_CONFIG_TIMINGS_H_RES +
    (uint32_t)ST7262_PANEL_CONFIG_TIMINGS_HSYNC_FRONT_PORCH +
    (uint32_t)ST7262_PANEL_CONFIG_TIMINGS_HSYNC_PULSE_WIDTH +
    (uint32_t)ST7262_PANEL_CONFIG_TIMINGS_HSYNC_BACK_PORCH;
static constexpr uint32_t DISPLAY_TOTAL_V_LINES =
    (uint32_t)ST7262_PANEL_CONFIG_TIMINGS_V_RES +
    (uint32_t)ST7262_PANEL_CONFIG_TIMINGS_VSYNC_FRONT_PORCH +
    (uint32_t)ST7262_PANEL_CONFIG_TIMINGS_VSYNC_PULSE_WIDTH +
    (uint32_t)ST7262_PANEL_CONFIG_TIMINGS_VSYNC_BACK_PORCH;
static constexpr uint32_t DISPLAY_FRAME_PERIOD_MS =
    (uint32_t)((((uint64_t)DISPLAY_TOTAL_H_PIXELS * (uint64_t)DISPLAY_TOTAL_V_LINES * 1000ULL) +
                (uint64_t)DISPLAY_TARGET_PCLK_HZ - 1ULL) /
               (uint64_t)DISPLAY_TARGET_PCLK_HZ);
static constexpr uint32_t DISPLAY_VSYNC_WAIT_BUDGET_MS =
    ((DISPLAY_FRAME_PERIOD_MS + 12U) < 40U) ? 40U : (DISPLAY_FRAME_PERIOD_MS + 12U);
static constexpr bool DISPLAY_ROTATE_180 = true;

#if defined(BOARD_HAS_TOUCH)
#ifndef GT911_I2C_CONFIG_SDA_IO_NUM
#define GT911_I2C_CONFIG_SDA_IO_NUM 19
#endif
#ifndef GT911_I2C_CONFIG_SCL_IO_NUM
#define GT911_I2C_CONFIG_SCL_IO_NUM 20
#endif
#ifndef GT911_I2C_CONFIG_MASTER_CLK_SPEED
#define GT911_I2C_CONFIG_MASTER_CLK_SPEED 400000U
#endif
#ifndef GT911_TOUCH_CONFIG_RST_GPIO_NUM
#define GT911_TOUCH_CONFIG_RST_GPIO_NUM -1
#endif
#ifndef TOUCH_SWAP_XY
#define TOUCH_SWAP_XY false
#endif
#ifndef TOUCH_MIRROR_X
#define TOUCH_MIRROR_X false
#endif
#ifndef TOUCH_MIRROR_Y
#define TOUCH_MIRROR_Y false
#endif
#ifndef TOUCH_EXTRA_MIRROR_Y
#define TOUCH_EXTRA_MIRROR_Y true
#endif

static constexpr int32_t TOUCH_I2C_SDA_PIN = GT911_I2C_CONFIG_SDA_IO_NUM;
static constexpr int32_t TOUCH_I2C_SCL_PIN = GT911_I2C_CONFIG_SCL_IO_NUM;
static constexpr int32_t TOUCH_RST_PIN = GT911_TOUCH_CONFIG_RST_GPIO_NUM;
static constexpr uint32_t TOUCH_I2C_CLOCK_HZ = (uint32_t)GT911_I2C_CONFIG_MASTER_CLK_SPEED;
static constexpr uint8_t TOUCH_GT911_ADDR = 0x5D;
static constexpr uint8_t TOUCH_GT911_ADDR_ALT = 0x14;

static TouchLib* s_touchPanel = nullptr;
static bool s_touchReady = false;
static uint8_t s_touchAddrInUse = 0;
static int16_t s_touchLastX = 0;
static int16_t s_touchLastY = 0;

static inline int16_t clamp_touch_coord(int32_t v, int32_t max_v)
{
    if (v < 0) return 0;
    if (v > max_v) return (int16_t)max_v;
    return (int16_t)v;
}

static void transform_touch_point(int16_t* x, int16_t* y)
{
    if (!x || !y) return;

    int32_t tx = *x;
    int32_t ty = *y;

#if TOUCH_SWAP_XY
    int32_t tmp = tx;
    tx = ty;
    ty = tmp;
#endif

#if TOUCH_MIRROR_X
    tx = (SCREEN_WIDTH - 1) - tx;
#endif

#if TOUCH_MIRROR_Y
    ty = (SCREEN_HEIGHT - 1) - ty;
#endif

#if DISPLAY_ROTATE_180
    tx = (SCREEN_WIDTH - 1) - tx;
    ty = (SCREEN_HEIGHT - 1) - ty;
#endif

#if TOUCH_EXTRA_MIRROR_Y
    ty = (SCREEN_HEIGHT - 1) - ty;
#endif

    *x = clamp_touch_coord(tx, SCREEN_WIDTH - 1);
    *y = clamp_touch_coord(ty, SCREEN_HEIGHT - 1);
}

static bool init_touch_panel()
{
    if (s_touchReady) return true;

    Wire.begin((int)TOUCH_I2C_SDA_PIN, (int)TOUCH_I2C_SCL_PIN);
    Wire.setClock(TOUCH_I2C_CLOCK_HZ);

    const uint8_t addr_candidates[] = {TOUCH_GT911_ADDR, TOUCH_GT911_ADDR_ALT};
    for (size_t i = 0; i < (sizeof(addr_candidates) / sizeof(addr_candidates[0])); ++i) {
        uint8_t addr = addr_candidates[i];
        if (i > 0 && addr == addr_candidates[0]) continue;

        if (s_touchPanel) {
            delete s_touchPanel;
            s_touchPanel = nullptr;
        }

        s_touchPanel = new TouchLib(Wire, TOUCH_I2C_SDA_PIN, TOUCH_I2C_SCL_PIN, addr, TOUCH_RST_PIN);
        if (!s_touchPanel) {
            Serial.println("[DisplayMgr] touch alloc failed");
            break;
        }

        if (s_touchPanel->init()) {
            s_touchReady = true;
            s_touchAddrInUse = addr;
            break;
        }
    }

    if (!s_touchReady) {
        Serial.printf("[DisplayMgr] touch init failed (sda=%d scl=%d tried=0x%02X,0x%02X)\n",
                      (int)TOUCH_I2C_SDA_PIN,
                      (int)TOUCH_I2C_SCL_PIN,
                      (unsigned int)TOUCH_GT911_ADDR,
                      (unsigned int)TOUCH_GT911_ADDR_ALT);
        TEST_LOG("touch init failed");
        return false;
    }

    Serial.printf("[DisplayMgr] touch init done (sda=%d scl=%d addr=0x%02X clk=%u)\n",
                  (int)TOUCH_I2C_SDA_PIN,
                  (int)TOUCH_I2C_SCL_PIN,
                  (unsigned int)s_touchAddrInUse,
                  (unsigned int)TOUCH_I2C_CLOCK_HZ);
    TEST_LOG("touch init done (sda=%d scl=%d addr=0x%02X clk=%u)",
             (int)TOUCH_I2C_SDA_PIN,
             (int)TOUCH_I2C_SCL_PIN,
             (unsigned int)s_touchAddrInUse,
             (unsigned int)TOUCH_I2C_CLOCK_HZ);
    return true;
}

static void lvgl_touch_read_cb(lv_indev_drv_t* indev_drv, lv_indev_data_t* data)
{
    LV_UNUSED(indev_drv);
    if (!data) return;

    if (!s_touchReady) {
        data->state = LV_INDEV_STATE_REL;
        data->point.x = s_touchLastX;
        data->point.y = s_touchLastY;
        return;
    }

    if (s_touchPanel && s_touchPanel->read()) {
        TP_Point p = s_touchPanel->getPoint(0);
        int16_t x = (int16_t)p.x;
        int16_t y = (int16_t)p.y;
        transform_touch_point(&x, &y);

        s_touchLastX = x;
        s_touchLastY = y;

        data->state = LV_INDEV_STATE_PR;
        data->point.x = x;
        data->point.y = y;
    } else {
        data->state = LV_INDEV_STATE_REL;
        data->point.x = s_touchLastX;
        data->point.y = s_touchLastY;
    }
}
#endif

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

static inline void request_full_invalidate();
static void display_diag_log(const char* tag);
static bool wait_for_vsync_edge(TickType_t timeoutTicks);
static void request_rgb_panel_restart(esp_lcd_panel_handle_t panel, const char* tag);

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
        request_full_invalidate();
        display_diag_log("dirty-overflow");
    }
}

static inline void mark_scene_reset_pending()
{
    s_sceneResetPending = true;
    s_backBufferSynced = false;
    s_frameHasFullRefreshArea = false;
    s_dirtyAreaCount = 0;
    s_flushFrameStarted = false;
}

static inline void request_full_invalidate()
{
    s_forceFullInvalidatePending = true;
}

static void display_diag_log(const char* tag)
{
    uint32_t nowMs = (uint32_t)(esp_timer_get_time() / 1000ULL);
    if ((nowMs - s_diagLastLogMs) < 500U) return;
    s_diagLastLogMs = nowMs;

    Serial.printf(
        "[DisplayMgr][DIAG] %s req=%u done=%u skip=%u restart=%u bypass=%u dirty=%u backSync=%d frameFull=%d sceneReset=%d vsync=%u\n",
        tag ? tag : "(null)",
        (unsigned int)s_swapReqCount,
        (unsigned int)s_swapDoneCount,
        (unsigned int)s_swapSkipTimeoutCount,
        (unsigned int)s_panelRestartCount,
        (unsigned int)s_replayBypassCount,
        (unsigned int)s_dirtyAreaCount,
        s_backBufferSynced ? 1 : 0,
        s_frameHasFullRefreshArea ? 1 : 0,
        s_sceneResetPending ? 1 : 0,
        (unsigned int)s_vsyncCount
    );
}

static bool wait_for_vsync_edge(TickType_t timeoutTicks)
{
    if (!s_vsyncSem) return false;

    uint32_t before = s_vsyncCount;
    xSemaphoreTake(s_vsyncSem, 0);
    TickType_t t0 = xTaskGetTickCount();
    while (s_vsyncCount == before &&
           (xTaskGetTickCount() - t0) < timeoutTicks) {
        xSemaphoreTake(s_vsyncSem, pdMS_TO_TICKS(2));
    }
    return s_vsyncCount != before;
}

static void request_rgb_panel_restart(esp_lcd_panel_handle_t panel, const char* tag)
{
    if (!panel) return;

    esp_err_t err = esp_lcd_rgb_panel_restart(panel);
    if (err == ESP_OK) {
        s_panelRestartCount++;
        display_diag_log(tag ? tag : "panel-restart");
    } else {
        TEST_LOG("RGB panel restart failed: %d", (int)err);
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

static void reset_cpu_usage_sampler()
{
    s_cpuPrevIdle0 = 0;
    s_cpuPrevIdle1 = 0;
    s_cpuPeakIdle0 = 1;
    s_cpuPeakIdle1 = 1;
    s_cpuPrevSampleTick = 0;
    s_cpuFilteredUsage0 = 0;
    s_cpuFilteredUsage1 = 0;
    s_cpuUsageValid = false;
    s_cpuWarmupSamples = 3;
}

static bool sample_cpu_usage(uint8_t* core0_usage, uint8_t* core1_usage)
{
    if (!core0_usage || !core1_usage || !s_idleHooksRegistered) return false;
    const TickType_t maxGapTicks = MONITOR_UPDATE_PERIOD_TICKS * 3;
    TickType_t nowTick = xTaskGetTickCount();

    uint32_t nowIdle0 = s_idleLoopCount[0];
    uint32_t nowIdle1 = s_idleLoopCount[1];

    if (s_cpuPrevSampleTick == 0 || (s_cpuPrevIdle0 == 0 && s_cpuPrevIdle1 == 0)) {
        s_cpuPrevIdle0 = nowIdle0;
        s_cpuPrevIdle1 = nowIdle1;
        s_cpuPrevSampleTick = nowTick;
        return false;
    }

    TickType_t dt = nowTick - s_cpuPrevSampleTick;
    if (dt > maxGapTicks) {
        // Ignore stale deltas after long pauses (e.g. debug screen open),
        // otherwise one oversized idle delta corrupts peak normalization.
        s_cpuPrevIdle0 = nowIdle0;
        s_cpuPrevIdle1 = nowIdle1;
        s_cpuPrevSampleTick = nowTick;
        if (s_cpuUsageValid) {
            *core0_usage = s_cpuFilteredUsage0;
            *core1_usage = s_cpuFilteredUsage1;
            return true;
        }
        return false;
    }

    uint32_t dIdle0 = nowIdle0 - s_cpuPrevIdle0;
    uint32_t dIdle1 = nowIdle1 - s_cpuPrevIdle1;
    s_cpuPrevIdle0 = nowIdle0;
    s_cpuPrevIdle1 = nowIdle1;
    s_cpuPrevSampleTick = nowTick;

    // Keep a monotonic "fully idle" baseline.
    // With the 1 second sample period, this gives a more useful instantaneous load
    // than a decaying baseline, which tends to chase the current sample and
    // eventually drives the displayed usage toward 0%.
    if (dIdle0 > s_cpuPeakIdle0) s_cpuPeakIdle0 = dIdle0;
    if (dIdle1 > s_cpuPeakIdle1) s_cpuPeakIdle1 = dIdle1;
    if (s_cpuPeakIdle0 == 0) s_cpuPeakIdle0 = 1;
    if (s_cpuPeakIdle1 == 0) s_cpuPeakIdle1 = 1;

    uint32_t idle0Pct = (uint32_t)(((uint64_t)dIdle0 * 100ULL + ((uint64_t)s_cpuPeakIdle0 / 2ULL)) / (uint64_t)s_cpuPeakIdle0);
    uint32_t idle1Pct = (uint32_t)(((uint64_t)dIdle1 * 100ULL + ((uint64_t)s_cpuPeakIdle1 / 2ULL)) / (uint64_t)s_cpuPeakIdle1);
    uint32_t u0 = (idle0Pct >= 100U) ? 0U : (100U - idle0Pct);
    uint32_t u1 = (idle1Pct >= 100U) ? 0U : (100U - idle1Pct);
    if (u0 > 100U) u0 = 100U;
    if (u1 > 100U) u1 = 100U;

    if (s_cpuWarmupSamples > 0) {
        s_cpuFilteredUsage0 = (uint8_t)u0;
        s_cpuFilteredUsage1 = (uint8_t)u1;
        s_cpuWarmupSamples--;
        if (s_cpuWarmupSamples == 0) {
            s_cpuUsageValid = true;
            *core0_usage = s_cpuFilteredUsage0;
            *core1_usage = s_cpuFilteredUsage1;
            return true;
        }
        return false;
    }

    s_cpuFilteredUsage0 = (uint8_t)u0;
    s_cpuFilteredUsage1 = (uint8_t)u1;
    s_cpuUsageValid = true;

    *core0_usage = s_cpuFilteredUsage0;
    *core1_usage = s_cpuFilteredUsage1;
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
    lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    mark_scene_reset_pending();
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

struct SplashAnimatedGifFileHandle {
    File file;
};

struct SplashAnimatedGifDrawContext {
    uint16_t* fb = nullptr;
    int32_t dstX = 0;
    int32_t dstY = 0;
    bool canvasFullyVisible = false;
    lv_area_t dirtyRect = {0, 0, 0, 0};
    bool haveDirtyRect = false;
    uint64_t drawUs = 0;
    uint32_t lineCount = 0;
    uint16_t frameWidth = 0;
    uint16_t frameHeight = 0;
    int16_t frameX = 0;
    int16_t frameY = 0;
    uint8_t disposal = 0;
    uint8_t hasTransparency = 0;
};

static inline void splash_copy_row_reversed_rgb565(uint16_t* dstEnd, const uint16_t* src, int32_t pixelCount)
{
    while (pixelCount >= 2) {
        uint32_t pair = 0;
        memcpy(&pair, src, sizeof(pair));
        pair = (pair << 16) | (pair >> 16);
        memcpy(dstEnd - 1, &pair, sizeof(pair));
        src += 2;
        dstEnd -= 2;
        pixelCount -= 2;
    }

    if (pixelCount == 1) {
        *dstEnd = *src;
    }
}

static bool make_splash_screen_area(int32_t dstX, int32_t dstY, int32_t x, int32_t y, int32_t w, int32_t h, lv_area_t* out)
{
    if (!out || w <= 0 || h <= 0) return false;

    int32_t x1 = dstX + x;
    int32_t y1 = dstY + y;
    int32_t x2 = x1 + w - 1;
    int32_t y2 = y1 + h - 1;

    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 >= SCREEN_WIDTH) x2 = SCREEN_WIDTH - 1;
    if (y2 >= SCREEN_HEIGHT) y2 = SCREEN_HEIGHT - 1;
    if (x1 > x2 || y1 > y2) return false;

    out->x1 = (lv_coord_t)x1;
    out->y1 = (lv_coord_t)y1;
    out->x2 = (lv_coord_t)x2;
    out->y2 = (lv_coord_t)y2;
    return true;
}

static void writeback_splash_rect(uint16_t* fb, const lv_area_t* rect)
{
    if (!fb || !rect) return;
    lv_area_t cacheArea = DISPLAY_ROTATE_180 ? rotate_area_180(rect) : *rect;
    cache_writeback_area(fb, &cacheArea);
}

static void* splash_animated_gif_open_file(const char* fname, int32_t* pSize)
{
    if (!fname) return nullptr;

    auto* handle = new SplashAnimatedGifFileHandle();
    if (!handle) return nullptr;

    handle->file = SD.open(fname, FILE_READ);
    if (!handle->file || handle->file.isDirectory()) {
        delete handle;
        return nullptr;
    }

    if (pSize) {
        *pSize = (int32_t)handle->file.size();
    }
    return handle;
}

static void splash_animated_gif_close_file(void* pHandle)
{
    auto* handle = static_cast<SplashAnimatedGifFileHandle*>(pHandle);
    if (!handle) return;
    if (handle->file) {
        handle->file.close();
    }
    delete handle;
}

static int32_t splash_animated_gif_read_file(GIFFILE* pFile, uint8_t* pBuf, int32_t iLen)
{
    if (!pFile || !pFile->fHandle || !pBuf || iLen <= 0) return 0;

    auto* handle = static_cast<SplashAnimatedGifFileHandle*>(pFile->fHandle);
    if (!handle || !handle->file) return 0;

    int32_t bytesToRead = iLen;
    const int32_t remaining = pFile->iSize - pFile->iPos;
    if (remaining <= 1) {
        return 0;
    }
    if (remaining < bytesToRead) {
        bytesToRead = remaining - 1;
    }
    if (bytesToRead <= 0) {
        return 0;
    }

    bytesToRead = (int32_t)handle->file.read(pBuf, (size_t)bytesToRead);
    pFile->iPos = (int32_t)handle->file.position();
    return bytesToRead;
}

static int32_t splash_animated_gif_seek_file(GIFFILE* pFile, int32_t iPosition)
{
    if (!pFile || !pFile->fHandle) return -1;

    auto* handle = static_cast<SplashAnimatedGifFileHandle*>(pFile->fHandle);
    if (!handle || !handle->file) return -1;

    if (iPosition < 0) {
        iPosition = 0;
    }
    handle->file.seek((uint32_t)iPosition);
    pFile->iPos = (int32_t)handle->file.position();
    return pFile->iPos;
}

static void splash_animated_gif_draw(GIFDRAW* pDraw)
{
    if (!pDraw || !pDraw->pUser || !pDraw->pPixels) return;

    auto* ctx = static_cast<SplashAnimatedGifDrawContext*>(pDraw->pUser);
    if (!ctx || !ctx->fb) return;

    if (pDraw->y == 0) {
        ctx->drawUs = 0;
        ctx->lineCount = 0;
        ctx->frameWidth = (uint16_t)pDraw->iWidth;
        ctx->frameHeight = (uint16_t)pDraw->iHeight;
        ctx->frameX = (int16_t)pDraw->iX;
        ctx->frameY = (int16_t)pDraw->iY;
        ctx->disposal = pDraw->ucDisposalMethod;
        ctx->hasTransparency = pDraw->ucHasTransparency;
        ctx->haveDirtyRect = make_splash_screen_area(
            ctx->dstX,
            ctx->dstY,
            pDraw->iX,
            pDraw->iY,
            pDraw->iWidth,
            pDraw->iHeight,
            &ctx->dirtyRect
        );
    }

    const uint64_t drawStartUs = (uint64_t)esp_timer_get_time();

    if (ctx->canvasFullyVisible) {
        const int32_t screenY = ctx->dstY + pDraw->iY + pDraw->y;
        const int32_t dstX = ctx->dstX + pDraw->iX;
        const int32_t drawWidth = pDraw->iWidth;
        const uint16_t* src = reinterpret_cast<const uint16_t*>(pDraw->pPixels);

        if (!DISPLAY_ROTATE_180) {
            uint16_t* dst = &ctx->fb[(size_t)screenY * SCREEN_WIDTH + (size_t)dstX];
            memcpy(dst, src, (size_t)drawWidth * sizeof(uint16_t));
        } else {
            uint16_t* dstEnd = &ctx->fb[(size_t)(SCREEN_HEIGHT - 1 - screenY) * SCREEN_WIDTH +
                                        (size_t)(SCREEN_WIDTH - 1 - dstX)];
            splash_copy_row_reversed_rgb565(dstEnd, src, drawWidth);
        }

        ctx->lineCount++;
        ctx->drawUs += (uint64_t)esp_timer_get_time() - drawStartUs;
        return;
    }

    const int32_t screenY = ctx->dstY + pDraw->iY + pDraw->y;
    const int32_t screenX0 = ctx->dstX + pDraw->iX;
    int32_t drawWidth = pDraw->iWidth;
    if (screenY >= 0 && screenY < SCREEN_HEIGHT && drawWidth > 0) {
        int32_t clipLeft = 0;
        int32_t dstX = screenX0;
        if (dstX < 0) {
            clipLeft = -dstX;
            dstX = 0;
        }
        if ((dstX + drawWidth - clipLeft) > SCREEN_WIDTH) {
            drawWidth = SCREEN_WIDTH - dstX + clipLeft;
        }
        drawWidth -= clipLeft;

        if (drawWidth > 0) {
            const uint16_t* src = reinterpret_cast<const uint16_t*>(pDraw->pPixels) + clipLeft;
            if (!DISPLAY_ROTATE_180) {
                uint16_t* dst = &ctx->fb[(size_t)screenY * SCREEN_WIDTH + (size_t)dstX];
                memcpy(dst, src, (size_t)drawWidth * sizeof(uint16_t));
            } else {
                uint16_t* dstEnd = &ctx->fb[(size_t)(SCREEN_HEIGHT - 1 - screenY) * SCREEN_WIDTH +
                                            (size_t)(SCREEN_WIDTH - 1 - dstX)];
                splash_copy_row_reversed_rgb565(dstEnd, src, drawWidth);
            }
            ctx->lineCount++;
        }
    }

    ctx->drawUs += (uint64_t)esp_timer_get_time() - drawStartUs;
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
    TEST_LOG("Estimated frame period=%u ms, VSYNC wait budget=%u ms",
             (unsigned int)DISPLAY_FRAME_PERIOD_MS,
             (unsigned int)DISPLAY_VSYNC_WAIT_BUDGET_MS);
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
        const uint32_t lineOptions[] = {240, 220, 200, 180, 160, 140, 112};
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

    lv_disp_drv_register(&_disp_drv);
    TEST_LOG("lv_disp_drv_register done");

#if defined(BOARD_HAS_TOUCH)
    if (init_touch_panel()) {
        static lv_indev_drv_t indev_drv;
        lv_indev_drv_init(&indev_drv);
        indev_drv.type = LV_INDEV_TYPE_POINTER;
        indev_drv.read_cb = lvgl_touch_read_cb;
        lv_indev_t* indev = lv_indev_drv_register(&indev_drv);
        Serial.printf("[DisplayMgr] LVGL touch indev register %s (addr=0x%02X)\n",
                      indev ? "ok" : "fail",
                      (unsigned int)s_touchAddrInUse);
        TEST_LOG("lv_indev_drv_register done (touch)");
    } else {
        Serial.println("[DisplayMgr] touch init failed; LVGL touch input disabled");
        TEST_LOG("touch init failed; LVGL touch input disabled");
    }
#endif

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
                       (xTaskGetTickCount() - t0) < pdMS_TO_TICKS(DISPLAY_VSYNC_WAIT_BUDGET_MS)) {
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
            s_frameStartedFromSceneReset = s_sceneResetPending;
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
            s_sceneResetPending = false;
            // No need to keep dirty list for full-frame redraw.
            s_dirtyAreaCount = 0;
        } else {
            // If the back buffer is stale and we render only partial areas,
            // recover once by mirroring front->back.
            if (!s_backBufferSynced) {
                if (!s_splashActive) {
                    uint16_t* front = self->_fb_buf[self->_frontFbIndex];
                    uint16_t* back = self->_fb_buf[self->_backFbIndex];
                    if (front && back) {
                        if (s_sceneResetPending) {
                            // Scene transition (e.g., splash->gauge): don't reuse old frame.
                            memset(back, 0, self->_fb_pixels * sizeof(uint16_t));
                            display_diag_log("resync-scene-reset");
                        } else {
                            memcpy(back, front, self->_fb_pixels * sizeof(uint16_t));
                            display_diag_log("resync-front-to-back");
                        }
                        cache_writeback_span(back, self->_fb_pixels * sizeof(uint16_t));
                    }
                    s_backBufferSynced = true;
                    s_sceneResetPending = false;
                }
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

                TickType_t waitBudget = pdMS_TO_TICKS(DISPLAY_VSYNC_WAIT_BUDGET_MS);
                TickType_t t0 = xTaskGetTickCount();
                while (s_vsyncCount == before &&
                       (xTaskGetTickCount() - t0) < waitBudget) {
                    xSemaphoreTake(s_vsyncSem, pdMS_TO_TICKS(2));
                }
                sawVsync = (s_vsyncCount != before);
            }

            if (!sawVsync) {
                s_vsyncTimeoutConsecutive++;
                if (s_vsyncTimeoutConsecutive >= VSYNC_TIMEOUT_RESTART_THRESHOLD) {
                    request_rgb_panel_restart(
                        (self->rgbPanel) ? self->rgbPanel->getPanelHandle() : nullptr,
                        "vsync-timeout-restart"
                    );
                    s_vsyncTimeoutConsecutive = 0;
                }
                // Skip this frame to avoid out-of-phase swap and keep buffers identical.
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
                request_full_invalidate();
                display_diag_log("vsync-timeout-skip");
                lv_disp_flush_ready(drv);
                return;
            }
            s_vsyncTimeoutConsecutive = 0;

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

            uint32_t dirtyPixels = 0;
            for (uint8_t i = 0; i < s_dirtyAreaCount; ++i) {
                dirtyPixels += area_pixels(&s_dirtyAreas[i]);
            }
            if (s_splashActive) {
                // During splash GIF playback we prefer decoder cadence over keeping
                // both full-screen framebuffers identical on every frame. Scene reset
                // will resync deterministically before the UI becomes active.
                s_backBufferSynced = false;
                s_sceneResetPending = false;
                s_frameStartedFromSceneReset = false;
                s_dirtyAreaCount = 0;
                s_flushFrameStarted = false;
                lv_disp_flush_ready(drv);
                return;
            }
            const uint32_t replayThresholdPixels = (SCREEN_WIDTH * SCREEN_HEIGHT * 35U) / 100U;
            bool doPeriodicFullSync = false;
            if (PERIODIC_FULL_SYNC_INTERVAL_FRAMES > 0U) {
                s_successfulPseudoSwapFrameCount++;
                doPeriodicFullSync =
                    (s_successfulPseudoSwapFrameCount % PERIODIC_FULL_SYNC_INTERVAL_FRAMES) == 0U;
            }
            bool doFullSync =
                s_frameHasFullRefreshArea ||
                s_frameStartedFromSceneReset ||
                doPeriodicFullSync ||
                (dirtyPixels > replayThresholdPixels);

            uint16_t* front = self->_fb_buf[self->_frontFbIndex];
            uint16_t* back = self->_fb_buf[self->_backFbIndex];
            if (front && back) {
                if (doFullSync) {
                    memcpy(back, front, self->_fb_pixels * sizeof(uint16_t));
                    cache_writeback_span(back, self->_fb_pixels * sizeof(uint16_t));

                    if (s_frameHasFullRefreshArea) {
                        display_diag_log("fullframe-full-sync");
                    } else if (s_frameStartedFromSceneReset) {
                        display_diag_log("scene-reset-full-sync");
                    } else if (doPeriodicFullSync) {
                        display_diag_log("periodic-full-sync");
                    } else {
                        display_diag_log("threshold-full-sync");
                    }
                } else {
                    // Replay this frame's dirty regions into the new back buffer so
                    // the next partial frame always starts from identical content.
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
            } else {
                s_backBufferSynced = false;
            }

            s_sceneResetPending = false;
            s_frameStartedFromSceneReset = false;
            s_flushFrameStarted = false;
        }
    }

    lv_disp_flush_ready(drv);
}

#if defined(__GNUC__)
#pragma GCC push_options
#pragma GCC optimize ("O3")
#endif
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
    s_splashGifReady = false;

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

    auto prepare_splash_buffers = [&]() {
        const size_t bufferSize = self->_fb_pixels * sizeof(uint16_t);
        for (size_t i = 0; i < 2; ++i) {
            if (self->_fb_buf[i]) {
                memset(self->_fb_buf[i], 0, bufferSize);
                cache_writeback_span(self->_fb_buf[i], bufferSize);
            }
        }
        self->gfx->setFrameBuffer(self->_fb_buf[self->_frontFbIndex]);
        s_dirtyAreaCount = 0;
        s_flushFrameStarted = false;
    };

    auto present_splash_frame = [&]() -> bool {
        return (self->_fb_buf[self->_frontFbIndex] != nullptr);
    };

    auto alloc_splash_buffer = [&](size_t bytes) -> uint8_t* {
        uint8_t* buf = (uint8_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!buf) {
            buf = (uint8_t*)heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
        }
        return buf;
    };

    prepare_splash_buffers();
    TEST_LOG("splash AnimatedGIF buffers prepared");

    GIFMemory gifMem = {};
    if (system->LockGif(pdMS_TO_TICKS(100))) {
        GIFMemory* shared = system->GetPsramObjPtr();
        if (shared && shared->data && shared->size) {
            gifMem = *shared;
        }
        system->UnlockGif();
    }

    AnimatedGIF* splashGif = new AnimatedGIF();
    if (!splashGif) {
        Serial.println("[DisplayMgr] Critical: AnimatedGIF alloc failed");
        s_splashActive = false;
        vTaskDelete(NULL);
        return;
    }
    splashGif->begin(GIF_PALETTE_RGB565_LE);

    String sdPath = self->_pendingGifPath;
    if (sdPath.startsWith("S:")) {
        sdPath = sdPath.substring(2);
    }
    if (!sdPath.startsWith("/")) {
        sdPath = "/" + sdPath;
    }

    bool splashOpened = false;
    if (gifMem.data && gifMem.size) {
        splashOpened = splashGif->open((uint8_t*)gifMem.data, (int)gifMem.size, splash_animated_gif_draw) != 0;
        if (splashOpened) {
            TEST_LOG("AnimatedGIF opened from PSRAM: %u bytes", (unsigned int)gifMem.size);
            Serial.printf("[DisplayMgr] Splash GIF source: PSRAM (%u bytes)\n", (unsigned int)gifMem.size);
        }
    }
    if (!splashOpened && SD.exists(sdPath.c_str())) {
        splashOpened = splashGif->open(
            sdPath.c_str(),
            splash_animated_gif_open_file,
            splash_animated_gif_close_file,
            splash_animated_gif_read_file,
            splash_animated_gif_seek_file,
            splash_animated_gif_draw
        ) != 0;
        if (splashOpened) {
            TEST_LOG("AnimatedGIF opened from SD: %s", sdPath.c_str());
            Serial.printf("[DisplayMgr] Splash GIF source: SD (%s)\n", sdPath.c_str());
        }
    }

    uint8_t* splashFrameBuf = nullptr;
    uint8_t* splashTurboBuf = nullptr;
    bool turboEnabled = false;
    int32_t splashGifDstX = 0;
    int32_t splashGifDstY = 0;
    SplashAnimatedGifDrawContext drawCtx = {};

    if (!splashOpened) {
        TEST_LOG("AnimatedGIF open failed: %s", self->_pendingGifPath.c_str());
        Serial.printf("[DisplayMgr] Splash GIF open failed: %s\n", self->_pendingGifPath.c_str());
    } else {
        const int canvasW = splashGif->getCanvasWidth();
        const int canvasH = splashGif->getCanvasHeight();
        const size_t canvasPixels = (size_t)canvasW * (size_t)canvasH;
        splashGifDstX = (SCREEN_WIDTH - canvasW) / 2;
        splashGifDstY = (SCREEN_HEIGHT - canvasH) / 2;

        const size_t splashFrameBufBytes = canvasPixels * sizeof(uint16_t);
        splashFrameBuf = alloc_splash_buffer(splashFrameBufBytes);
        if (splashFrameBuf) {
            memset(splashFrameBuf, 0, splashFrameBufBytes);
            splashGif->setDrawType(GIF_DRAW_COOKED);
            splashGif->setFrameBuf(splashFrameBuf);

            const size_t turboBytes = (size_t)TURBO_BUFFER_SIZE + canvasPixels;
            splashTurboBuf = alloc_splash_buffer(turboBytes);
            if (splashTurboBuf) {
                memset(splashTurboBuf, 0, turboBytes);
                splashGif->setTurboBuf(splashTurboBuf);
                turboEnabled = true;
            }

            TEST_LOG(
                "AnimatedGIF configured canvas=%dx%d mode=COOKED turbo=%s",
                canvasW,
                canvasH,
                turboEnabled ? "ON" : "OFF"
            );
        } else {
            Serial.println("[DisplayMgr] Splash GIF buffer alloc failed; splash playback disabled");
            TEST_LOG("AnimatedGIF frame buffer alloc failed");
            splashGif->close();
            splashOpened = false;
        }

        drawCtx.fb = self->_fb_buf[self->_frontFbIndex];
        drawCtx.dstX = splashGifDstX;
        drawCtx.dstY = splashGifDstY;
        drawCtx.canvasFullyVisible =
            (splashGifDstX >= 0) &&
            (splashGifDstY >= 0) &&
            ((splashGifDstX + canvasW) <= SCREEN_WIDTH) &&
            ((splashGifDstY + canvasH) <= SCREEN_HEIGHT);
    }

    TickType_t startTick = xTaskGetTickCount();
    const TickType_t minSplashDuration = pdMS_TO_TICKS(1200);
    const TickType_t maxSplashDuration = pdMS_TO_TICKS(12000);
    constexpr uint32_t SPLASH_PROFILE_LOG_FRAMES = 10U;
    uint32_t profiledFrames = 0;
    uint64_t splashDecodeUsTotal = 0;
    uint64_t splashDrawUsTotal = 0;
    uint64_t splashWorkUsTotal = 0;
    uint64_t splashPacedUsTotal = 0;
    uint32_t splashDecodeUsMax = 0;
    uint32_t splashDrawUsMax = 0;
    uint32_t splashWorkUsMax = 0;

    while (true) {
        if (ulTaskNotifyTake(pdTRUE, 0)) {
            Serial.println("[DisplayMgr] Splash interrupted by notify");
            break;
        }

        TickType_t elapsed = xTaskGetTickCount() - startTick;
        if (s_splashGifReady) {
            if (elapsed >= minSplashDuration) {
                Serial.println("[DisplayMgr] Splash finished by AnimatedGIF completion");
                break;
            }

            TickType_t waitTicks = minSplashDuration - elapsed;
            if (waitTicks > pdMS_TO_TICKS(20)) {
                waitTicks = pdMS_TO_TICKS(20);
            }
            if (ulTaskNotifyTake(pdTRUE, waitTicks)) {
                Serial.println("[DisplayMgr] Splash interrupted while holding final frame");
                break;
            }
            continue;
        }

        if (elapsed >= maxSplashDuration) {
            Serial.println("[DisplayMgr] Splash finished by timeout");
            break;
        }

        if (!splashOpened) {
            if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20))) {
                Serial.println("[DisplayMgr] Splash interrupted without GIF decoder");
            }
            break;
        }

        drawCtx.drawUs = 0;
        drawCtx.lineCount = 0;
        drawCtx.haveDirtyRect = false;
        drawCtx.frameWidth = 0;
        drawCtx.frameHeight = 0;
        drawCtx.frameX = 0;
        drawCtx.frameY = 0;
        drawCtx.disposal = 0;
        drawCtx.hasTransparency = 0;

        int frameDelayMs = 0;
        const uint64_t workStartUs = (uint64_t)esp_timer_get_time();
        const int playStatus = splashGif->playFrame(false, &frameDelayMs, &drawCtx);
        uint32_t workUs = (uint32_t)((uint64_t)esp_timer_get_time() - workStartUs);
        uint32_t drawUs = (uint32_t)drawCtx.drawUs;
        if (drawUs > workUs) {
            drawUs = workUs;
        }
        uint32_t decodeUs = workUs - drawUs;

        const bool frameRendered = drawCtx.haveDirtyRect && (drawCtx.lineCount > 0U);
        if (frameRendered && drawCtx.fb) {
            writeback_splash_rect(drawCtx.fb, &drawCtx.dirtyRect);
            present_splash_frame();

            profiledFrames++;
            splashDecodeUsTotal += decodeUs;
            splashDrawUsTotal += drawUs;
            splashWorkUsTotal += workUs;
            if (decodeUs > splashDecodeUsMax) splashDecodeUsMax = decodeUs;
            if (drawUs > splashDrawUsMax) splashDrawUsMax = drawUs;
            if (workUs > splashWorkUsMax) splashWorkUsMax = workUs;
        }

        if (playStatus < 0) {
            Serial.printf("[DisplayMgr] AnimatedGIF decode error: %d\n", splashGif->getLastError());
            TEST_LOG("AnimatedGIF decode error=%d", splashGif->getLastError());
            break;
        }

        if (playStatus == 0) {
            s_splashGifReady = true;
        }

        if (!frameRendered) {
            if (s_splashGifReady) {
                continue;
            }
            if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1))) {
                Serial.println("[DisplayMgr] Splash interrupted during empty frame wait");
                break;
            }
            continue;
        }

        if (SPLASH_ANIMATED_GIF_SPEED_FACTOR > 1U) {
            frameDelayMs /= (int)SPLASH_ANIMATED_GIF_SPEED_FACTOR;
        }
        if (frameDelayMs <= 0) {
            frameDelayMs = 10;
        }

        const uint32_t frameElapsedMs = (uint32_t)((workUs + 999ULL) / 1000ULL);
        const uint32_t waitMs = (frameElapsedMs < (uint32_t)frameDelayMs) ? ((uint32_t)frameDelayMs - frameElapsedMs) : 1U;
        const uint32_t pacedUs = workUs + (waitMs * 1000U);
        splashPacedUsTotal += pacedUs;

        if (profiledFrames <= SPLASH_PROFILE_LOG_FRAMES) {
            Serial.printf(
                "[DisplayMgr][GIFPROF] f=%u rect=%ux%u@%d,%d delay_ms=%u decode_us=%u draw_us=%u work_us=%u paced_us=%u disp=%u trans=%u turbo=%u\n",
                (unsigned int)profiledFrames,
                (unsigned int)drawCtx.frameWidth,
                (unsigned int)drawCtx.frameHeight,
                (int)drawCtx.frameX,
                (int)drawCtx.frameY,
                (unsigned int)frameDelayMs,
                (unsigned int)decodeUs,
                (unsigned int)drawUs,
                (unsigned int)workUs,
                (unsigned int)pacedUs,
                (unsigned int)drawCtx.disposal,
                (unsigned int)drawCtx.hasTransparency,
                turboEnabled ? 1U : 0U
            );
        }

        if (s_splashGifReady) {
            continue;
        }

        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(waitMs))) {
            Serial.println("[DisplayMgr] Splash interrupted during frame wait");
            break;
        }
    }

    if (splashOpened) {
        if (profiledFrames > 0U) {
            Serial.printf(
                "[DisplayMgr][GIFPROF] avg decode_us=%u draw_us=%u work_us=%u paced_us=%u max decode_us=%u draw_us=%u work_us=%u frames=%u turbo=%u\n",
                (unsigned int)(splashDecodeUsTotal / profiledFrames),
                (unsigned int)(splashDrawUsTotal / profiledFrames),
                (unsigned int)(splashWorkUsTotal / profiledFrames),
                (unsigned int)(splashPacedUsTotal / profiledFrames),
                (unsigned int)splashDecodeUsMax,
                (unsigned int)splashDrawUsMax,
                (unsigned int)splashWorkUsMax,
                (unsigned int)profiledFrames,
                turboEnabled ? 1U : 0U
            );
        }
        splashGif->close();
    }
    delete splashGif;
    splashGif = nullptr;
    if (splashTurboBuf) {
        heap_caps_free(splashTurboBuf);
        splashTurboBuf = nullptr;
    }
    if (splashFrameBuf) {
        heap_caps_free(splashFrameBuf);
        splashFrameBuf = nullptr;
    }

    bool uiReady = false;
    for (int i = 0; i < 30; ++i) {
        if(system->LockLvgl(pdMS_TO_TICKS(100))) {
            TEST_LOG("LockLvgl acquired for UI transition, try=%d", i);
            lv_obj_t* screen = lv_scr_act();
            show_gauge_screen_locked(system);

            lv_obj_invalidate(screen);
            // Force at least one render pass before leaving splash task.
            lv_timer_handler();
            request_rgb_panel_restart(
                (self->rgbPanel) ? self->rgbPanel->getPanelHandle() : nullptr,
                "ui-transition-restart"
            );
            wait_for_vsync_edge(pdMS_TO_TICKS(DISPLAY_VSYNC_WAIT_BUDGET_MS));
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
    vTaskPrioritySet(self->_lvglTaskHandler, LVGL_REUSED_TASK_PRIORITY);
    Serial.println("[DisplayMgr] Reusing GifTask as LvglTask");
    DisplayMgr::HandleLvglTask(self);
}

#if defined(__GNUC__)
#pragma GCC pop_options
#endif

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
                            DisplayMgr::PlayGifTask, "GifTask", GIF_TASK_STACK_WORDS, self, SPLASH_GIF_TASK_PRIORITY, &self->_gifTaskHandler, 1
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
    TickType_t now = xTaskGetTickCount();
    TickType_t lastMonTick = now;
    TickType_t lastUiTick = now;
    TickType_t lastSyncLogTick = now;
    bool prevDebugBusy = false;
    Serial.println("[DisplayMgr] LvglTask started");

    while (true) {
        TickType_t sleepTicks = pdMS_TO_TICKS(LVGL_TASK_LOCK_FAIL_SLEEP_MS);
        if (sleepTicks == 0) {
            sleepTicks = 1;
        }

        if (system->LockLvgl(pdMS_TO_TICKS(5))) {
            uint32_t nextDelayMs = lv_timer_handler();
            if (s_forceFullInvalidatePending && !s_splashActive) {
                lv_obj_t* screen = lv_scr_act();
                if (screen) {
                    lv_obj_invalidate(screen);
                    display_diag_log("full-invalidate-requested");
                }
                s_forceFullInvalidatePending = false;
            }

            TickType_t now = xTaskGetTickCount();
            bool debugVisible = ui_debug_log_capture_enabled();
            bool debugAnimating = ui_debug_overlay_animating();
            bool debugBusy = debugVisible || debugAnimating;
            if (debugBusy != prevDebugBusy) {
                reset_cpu_usage_sampler();
            }
            if (prevDebugBusy && !debugBusy) {
                lastUiTick = now - UI_SHARED_UPDATE_PERIOD_TICKS;
                lastMonTick = now - MONITOR_UPDATE_PERIOD_TICKS;
            }
            if (!s_splashActive && !debugBusy &&
                (now - lastUiTick) >= UI_SHARED_UPDATE_PERIOD_TICKS) {
                UiSharedState snap = {};
                if (system->GetUiSharedSnapshot(&snap, 0)) {
                    apply_shared_ui_state(snap);
                }
                lastUiTick = now;
            }

            if (!s_splashActive && !debugBusy &&
                (now - lastMonTick) >= MONITOR_UPDATE_PERIOD_TICKS &&
                (s_monitorResumeTick == 0 || now >= s_monitorResumeTick)) {
                size_t total = heap_caps_get_total_size(MALLOC_CAP_8BIT);
                size_t free = heap_caps_get_free_size(MALLOC_CAP_8BIT);
                int32_t ram = (total > 0) ? (int32_t)(((total - free) * 100U) / total) : 0;

                uint8_t core0 = 0;
                uint8_t core1 = 0;
                if (sample_cpu_usage(&core0, &core1)) {
                    update_system_monitor(ram, core0, core1);
                } else if (s_cpuUsageValid) {
                    update_system_monitor(ram, s_cpuFilteredUsage0, s_cpuFilteredUsage1);
                } else {
                    update_system_monitor(ram, 0, 0);
                }
                lastMonTick = now;
            }

            if ((now - lastSyncLogTick) >= pdMS_TO_TICKS(2000)) {
                s_singleFlushMaxCopyUs = 0;
                lastSyncLogTick = now;
            }
            prevDebugBusy = debugBusy;

            system->UnlockLvgl();

            if (nextDelayMs == 0U) {
                sleepTicks = pdMS_TO_TICKS(LVGL_TASK_MIN_SLEEP_MS);
            } else {
                if (nextDelayMs > LVGL_TASK_MAX_SLEEP_MS) {
                    nextDelayMs = LVGL_TASK_MAX_SLEEP_MS;
                }
                sleepTicks = pdMS_TO_TICKS(nextDelayMs);
            }
            if (sleepTicks == 0) {
                sleepTicks = 1;
            }
        }

        vTaskDelay(sleepTicks);
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
