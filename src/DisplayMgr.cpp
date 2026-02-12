#include "DisplayMgr.h"
#include <CommonApi.h>
#include <lvgl.h>
#include <SD.h>
#include <ui.h>
#include "esp_heap_caps.h"
#if LV_USE_GIF
#include <src/extra/libs/gif/lv_gif.h>
#endif

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

        _fb_buf[0] = (uint16_t*)gfx->getFramebuffer();
        _fb_buf[1] = (uint16_t*)heap_caps_aligned_alloc(64, bufferSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

        if(_fb_buf[0] && _fb_buf[1]) {
            memset(_fb_buf[0], 0, bufferSize);
            memset(_fb_buf[1], 0, bufferSize);
            gfx->setFrameBuffer(_fb_buf[0]);
            Serial.println("[DisplayMgr] Double-buffering initialized with Hardware FB");
        }
        this->BacklightOn();
    }

    xTaskCreate(DisplayMgr::Subscribe, "DisplaySub", 4096, this, 4, &this->_eventTaskHandler);
}

void DisplayMgr::StartLVGL() {
    if (_lvglInitialized) return;

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
    Serial.println("[DisplayMgr] LVGL Started with SRAM Draw Buffer");

    size_t total_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    Serial.printf("[PSRAM] Total: %d, Free: %d, Used: %d bytes\n",
                  total_psram, free_psram, total_psram - free_psram);
}

void DisplayMgr::lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p)
{
    DisplayMgr* self = (DisplayMgr*)drv->user_data;

    if (self && self->gfx) {
        uint32_t w = lv_area_get_width(area);
        uint32_t h = lv_area_get_height(area);

        self->gfx->draw16bitRGBBitmap(
            area->x1,
            area->y1,
            (uint16_t*)color_p,
            w,
            h
        );

        if(lv_disp_flush_is_last(drv))
        {
            self->gfx->flush();
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
    lv_timer_set_period(((lv_gif_t*)_splashGif)->timer, 1);
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
    lv_timer_set_period(((lv_gif_t*)_splashGif)->timer, 1);
#endif
    lv_obj_center(_splashGif);
    Serial.printf("[DisplayMgr] GIF started from PSRAM: %u bytes\n", (unsigned int)gifMem.size);
    return true;
}

void DisplayMgr::PlayGifTask(void* pvParameters)
{
    DisplayMgr* self = static_cast<DisplayMgr*>(pvParameters);
    SystemAPI* system = SystemAPI::getInstance();

    self->gfx->setFrameBuffer(self->_fb_buf[0]);
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
    const uint32_t gifSpeedupMsPerLoop = 80;
    const int gifBurstPerLoop = 4;
    while ((xTaskGetTickCount() - startTick) < splashDuration) {
        if (ulTaskNotifyTake(pdTRUE, 0)) break;
        if (system->LockLvgl(pdMS_TO_TICKS(20))) {
            for (int i = 0; i < gifBurstPerLoop; ++i) {
                lv_timer_handler();
#if LV_USE_GIF
                if (self->_splashGif) {
                    lv_gif_t* gifObj = (lv_gif_t*)self->_splashGif;
                    gifObj->last_call -= gifSpeedupMsPerLoop;
                }
#endif
            }
            system->UnlockLvgl();
        }
        vTaskDelay(pdMS_TO_TICKS(1));
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

    while (true) {
        if (system->LockLvgl(pdMS_TO_TICKS(5))) {
            lv_timer_handler();
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
