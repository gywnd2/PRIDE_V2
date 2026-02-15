#include "DisplayMgr.h"
#include <CommonApi.h>
#include <lvgl.h>
#include <SD.h>
#include <ui.h>
#include "esp_heap_caps.h"
#include "freertos/task.h"
#include "esp_lcd_panel_rgb.h"
#include "freertos/semphr.h"
#include "esp_freertos_hooks.h"
#if LV_USE_GIF
#include <src/extra/libs/gif/lv_gif.h>
#endif

static SemaphoreHandle_t s_vsyncSem = nullptr;
static bool s_flushFrameStarted = false;
static bool s_backBufferSynced = true;
static bool s_frameHasFullRefreshArea = false;
static lv_area_t s_dirtyAreas[64];
static uint8_t s_dirtyAreaCount = 0;
static volatile uint32_t s_idleLoopCount[2] = {0, 0};
static bool s_idleHooksRegistered = false;

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
        Serial.printf("[DisplayMgr] Idle hook register failed: e0=%d e1=%d\n", (int)e0, (int)e1);
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

static bool IRAM_ATTR on_vsync_callback(
    esp_lcd_panel_handle_t panel,
    esp_lcd_rgb_panel_event_data_t* edata,
    void* user_ctx
) {
    LV_UNUSED(panel);
    LV_UNUSED(edata);
    SemaphoreHandle_t sem = (SemaphoreHandle_t)user_ctx;
    if (!sem) return false;
    BaseType_t high_task_wakeup = pdFALSE;
    xSemaphoreGiveFromISR(sem, &high_task_wakeup);
    return high_task_wakeup == pdTRUE;
}

static void* sd_fs_open(lv_fs_drv_t * drv, const char * path, lv_fs_mode_t mode) {
    LV_UNUSED(drv);
    LV_UNUSED(mode);

    String fullPath = path;
    if (!fullPath.startsWith("/")) fullPath = "/" + fullPath;

    File f = SD.open(fullPath.c_str(), FILE_READ);
    if (!f || f.isDirectory()) {
        Serial.printf("[FS] Open Failed: %s\n", fullPath.c_str());
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
    #ifdef GPIO_BCKL
        pinMode(GPIO_BCKL, OUTPUT);
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
        ST7262_PANEL_CONFIG_TIMINGS_PCLK_HZ,
        false,
        ST7262_PANEL_CONFIG_TIMINGS_FLAGS_DE_IDLE_HIGH,
        ST7262_PANEL_CONFIG_TIMINGS_FLAGS_PCLK_IDLE_HIGH,
        0
    );

    gfx = new Arduino_RGB_Display(
        SCREEN_WIDTH, SCREEN_HEIGHT, rgbPanel, 0, false
    );

    bool ok = gfx->begin(ST7262_PANEL_CONFIG_TIMINGS_PCLK_HZ);
    _gfxInitialized = ok;

    if(ok) {
        _fb_pixels = SCREEN_WIDTH * SCREEN_HEIGHT;
        size_t bufferSize = _fb_pixels * sizeof(uint16_t);
        _frontFbIndex = 0;
        _backFbIndex = 1;

        _fb_buf[0] = (uint16_t*)gfx->getFramebuffer();
        _fb_buf[1] = (uint16_t*)heap_caps_aligned_alloc(64, bufferSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

        if(_fb_buf[0] && _fb_buf[1]) {
            memset(_fb_buf[0], 0, bufferSize);
            memset(_fb_buf[1], 0, bufferSize);
            gfx->setFrameBuffer(_fb_buf[_frontFbIndex]);
            Serial.println("[DisplayMgr] Double-buffering initialized with Hardware FB");
        }

        if (!s_vsyncSem) {
            s_vsyncSem = xSemaphoreCreateBinary();
        }
        if (s_vsyncSem && rgbPanel && rgbPanel->getPanelHandle()) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
            esp_lcd_rgb_panel_event_callbacks_t cbs = {};
            cbs.on_vsync = on_vsync_callback;
            esp_err_t err = esp_lcd_rgb_panel_register_event_callbacks(
                rgbPanel->getPanelHandle(), &cbs, s_vsyncSem
            );
            if (err == ESP_OK) {
                Serial.println("[DisplayMgr] VSYNC callback registered (IDF5 API)");
            } else {
                Serial.printf("[DisplayMgr] VSYNC callback register failed: %d\n", (int)err);
            }
#else
            esp_rgb_panel_t* panel = __containerof(rgbPanel->getPanelHandle(), esp_rgb_panel_t, base);
            if (panel) {
                panel->on_frame_trans_done = on_vsync_callback;
                panel->user_ctx = s_vsyncSem;
                Serial.println("[DisplayMgr] VSYNC callback registered (IDF4 internal)");
            } else {
                Serial.println("[DisplayMgr] VSYNC callback register failed: panel null");
            }
#endif
        }

        this->BacklightOn();
    }

    xTaskCreate(DisplayMgr::Subscribe, "DisplaySub", 4096, this, 4, &this->_eventTaskHandler);
}

void DisplayMgr::StartLVGL() {
    if (_lvglInitialized) return;

    register_idle_hooks_once();

    lv_init();

    uint32_t sram_lines = 200;
    size_t sram_buf_size = SCREEN_WIDTH * sram_lines * sizeof(lv_color_t);

    static lv_color_t* sram_work_buf1 = (lv_color_t*)heap_caps_malloc(
        sram_buf_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA
    );
    static lv_color_t* sram_work_buf2 = (lv_color_t*)heap_caps_malloc(
        sram_buf_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA
    );

    if (!sram_work_buf1) sram_work_buf1 = (lv_color_t*)heap_caps_malloc(sram_buf_size, MALLOC_CAP_SPIRAM);
    if (!sram_work_buf2) sram_work_buf2 = (lv_color_t*)heap_caps_malloc(sram_buf_size, MALLOC_CAP_SPIRAM);

    lv_disp_draw_buf_init(&_draw_buf, sram_work_buf1, sram_work_buf2, SCREEN_WIDTH * sram_lines);

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

    #if LV_USE_PNG
    lv_png_init();
    #endif

    _lvglInitialized = true;
    Serial.println("[DisplayMgr] LVGL Started with SRAM Partial Draw Buffer + VSYNC sync");

    size_t total_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    Serial.printf("[PSRAM] Total: %d, Free: %d, Used: %d bytes\n",
                  total_psram, free_psram, total_psram - free_psram);
}

void DisplayMgr::lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p)
{
    DisplayMgr* self = (DisplayMgr*)drv->user_data;

    if (self && self->gfx) {
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
                }
                s_backBufferSynced = true;
            }

            // Store dirty area to replay onto the next back buffer after swap.
            if (s_dirtyAreaCount < (uint8_t)(sizeof(s_dirtyAreas) / sizeof(s_dirtyAreas[0]))) {
                s_dirtyAreas[s_dirtyAreaCount++] = *area;
            }
        }

        uint16_t* fb = self->_fb_buf[self->_backFbIndex];
        if (fb) {
            uint32_t w = lv_area_get_width(area);
            uint32_t h = lv_area_get_height(area);
            uint16_t* src = (uint16_t*)color_p;
            for (uint32_t y = 0; y < h; ++y) {
                size_t dstOff = (size_t)(area->y1 + y) * SCREEN_WIDTH + (size_t)area->x1;
                memcpy(&fb[dstOff], &src[y * w], w * sizeof(uint16_t));
            }
        }

        if (lv_disp_flush_is_last(drv)) {
            // Swap only on VSYNC boundary.
            if (s_vsyncSem) {
                xSemaphoreTake(s_vsyncSem, 0);
                xSemaphoreTake(s_vsyncSem, pdMS_TO_TICKS(20));
            }

            self->gfx->setFrameBuffer(self->_fb_buf[self->_backFbIndex]);
            self->gfx->flush();

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
        Serial.printf("[DisplayMgr] GIF not found on SD: %s\n", sdPath.c_str());
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

    lv_gif_set_src(_splashGif, lvPath.c_str());
#if LV_USE_GIF
    lv_timer_set_period(((lv_gif_t*)_splashGif)->timer, 5);
#endif
    lv_obj_center(_splashGif);
    Serial.printf("[DisplayMgr] GIF started: %s\n", lvPath.c_str());
    return true;
}

bool DisplayMgr::PlayGifFromMemory(const GIFMemory& gifMem)
{
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

    _splashGifDsc.header.always_zero = 0;
    _splashGifDsc.header.cf = LV_IMG_CF_RAW;
    _splashGifDsc.header.w = 1;
    _splashGifDsc.header.h = 1;
    _splashGifDsc.data_size = gifMem.size;
    _splashGifDsc.data = gifMem.data;

    lv_gif_set_src(_splashGif, &_splashGifDsc);
#if LV_USE_GIF
    lv_timer_set_period(((lv_gif_t*)_splashGif)->timer, 5);
#endif
    lv_obj_center(_splashGif);
    Serial.printf("[DisplayMgr] GIF started from PSRAM: %u bytes\n", (unsigned int)gifMem.size);
    return true;
}

void DisplayMgr::PlayGifTask(void* pvParameters)
{
    DisplayMgr* self = static_cast<DisplayMgr*>(pvParameters);
    SystemAPI* system = SystemAPI::getInstance();

    self->gfx->setFrameBuffer(self->_fb_buf[self->_frontFbIndex]);
    self->gfx->fillScreen(0x0000);
    self->gfx->flush();

    self->StartLVGL();

    bool splashStarted = false;
    if (system->LockLvgl(pdMS_TO_TICKS(100))) {
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
    }

    if (!splashStarted) {
        Serial.printf("[DisplayMgr] GIF start failed: %s\n", self->_pendingGifPath.c_str());
    }

    TickType_t startTick = xTaskGetTickCount();
    const TickType_t splashDuration = pdMS_TO_TICKS(12000);
    while ((xTaskGetTickCount() - startTick) < splashDuration) {
        if (ulTaskNotifyTake(pdTRUE, 0)) break;
        if (system->LockLvgl(pdMS_TO_TICKS(20))) {
            lv_timer_handler();
            system->UnlockLvgl();
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    bool uiReady = false;
    for (int i = 0; i < 30; ++i) {
        if(system->LockLvgl(pdMS_TO_TICKS(100))) {
            lv_obj_t* screen = lv_scr_act();
            if (self->_splashGif) {
                lv_obj_del(self->_splashGif);
                self->_splashGif = nullptr;
            }
            lv_obj_clean(screen);
            GaugeInit();
            lv_obj_invalidate(screen);
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

    if (self->_lvglTaskHandler == nullptr) {
        xTaskCreatePinnedToCore(DisplayMgr::HandleLvglTask, "LvglTask", 8192, self, 4, &self->_lvglTaskHandler, 1);
    }

    self->_splashFinished = true;
    self->_gifTaskHandler = nullptr;
    vTaskDelete(NULL);
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

                    if (self->_gifTaskHandler == nullptr) {
                        Serial.printf("[DisplayMgr] Starting LVGL GIF Task: %s\n", self->_pendingGifPath.c_str());
                        xTaskCreatePinnedToCore(DisplayMgr::PlayGifTask, "GifTask", 8192, self, 5, &self->_gifTaskHandler, 1);
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
    TickType_t xLastWakeTime = xTaskGetTickCount();
    TickType_t lastMonTick = xLastWakeTime;

    while (true) {
        if (system->LockLvgl(pdMS_TO_TICKS(5))) {
            lv_timer_handler();

            TickType_t now = xTaskGetTickCount();
            if ((now - lastMonTick) >= pdMS_TO_TICKS(1000)) {
                size_t total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
                size_t free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
                int32_t ram = (total > 0) ? (int32_t)(((total - free) * 100U) / total) : 0;

                uint8_t core0 = 0;
                uint8_t core1 = 0;
                if (sample_cpu_usage(&core0, &core1)) {
                    update_system_monitor(ram, core0, core1);
                } else {
                    update_system_monitor(ram, 0, 0);
                }
                lastMonTick = now;
            }

            system->UnlockLvgl();
        }

        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(41));
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
