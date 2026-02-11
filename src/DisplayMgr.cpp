#include "DisplayMgr.h"
#include <CommonApi.h>
#include <StorageMgr.h>
#include <lvgl.h>
#include <SD.h>
#include <ui.h>
#include "esp_heap_caps.h"
// #include "esp_lcd_panel_rgb.h"
// #include "esp_lcd_panel_io.h"
// #include "esp_lcd_panel_ops.h"
// #include "esp_lcd_panel_interface.h"
// #include "freertos/semphr.h"

// SemaphoreHandle_t vsync_sem;

// static bool IRAM_ATTR on_vsync_callback(esp_lcd_panel_handle_t panel, const esp_lcd_rgb_panel_event_data_t *edata, void *user_ctx) {
//     BaseType_t high_task_wakeup;
//     xSemaphoreGiveFromISR(vsync_sem, &high_task_wakeup);
//     return high_task_wakeup == pdTRUE;
// }

static void* sd_fs_open(lv_fs_drv_t * drv, const char * path, lv_fs_mode_t mode) {
    String fullPath = path;
    if (!fullPath.startsWith("/")) fullPath = "/" + fullPath;

    // 읽기 전용 모드로 고정 (PNG 로드용)
    File f = SD.open(fullPath.c_str(), FILE_READ);
    if (!f || f.isDirectory()) {
        Serial.printf("[FS] Open Failed: %s\n", fullPath.c_str());
        return NULL;
    }

    // File 객체는 복사되면 핸들이 꼬일 수 있으므로 동적 할당
    File* fp = new File(f);
    return (void*)fp;
}

static lv_fs_res_t sd_fs_close(lv_fs_drv_t * drv, void * file_p) {
    File* fp = (File*)file_p;
    fp->close();
    delete fp;
    return LV_FS_RES_OK;
}

static lv_fs_res_t sd_fs_read(lv_fs_drv_t * drv, void * file_p, void * buf, uint32_t btr, uint32_t * br) {
    File* fp = (File*)file_p;
    // SD 라이브러리의 read는 실제로 읽은 바이트 수를 반환합니다.
    size_t read_size = fp->read((uint8_t*)buf, btr);
    if (br) *br = (uint32_t)read_size;

    return (read_size >= 0) ? LV_FS_RES_OK : LV_FS_RES_UNKNOWN;
}

static lv_fs_res_t sd_fs_seek(lv_fs_drv_t * drv, void * file_p, uint32_t pos, lv_fs_whence_t whence) {
    File* fp = (File*)file_p;

    // 버전에 맞게 수정된 열거형 명칭: LV_FS_SEEK_...
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
    File* fp = (File*)file_p;
    *pos_p = fp->position();
    return LV_FS_RES_OK;
}

void DisplayMgr::Init()
{
    // vsync_sem = xSemaphoreCreateBinary();

    #ifdef GPIO_BCKL
        pinMode(GPIO_BCKL, OUTPUT);
    #endif
    this->taskHandler = nullptr;

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
        ST7262_PANEL_CONFIG_TIMINGS_PCLK_HZ, // 수정
        false,
        ST7262_PANEL_CONFIG_TIMINGS_FLAGS_DE_IDLE_HIGH,
        ST7262_PANEL_CONFIG_TIMINGS_FLAGS_PCLK_IDLE_HIGH,
        0
    );

    gfx = new Arduino_RGB_Display(
        SCREEN_WIDTH, SCREEN_HEIGHT, rgbPanel, 0, false
    );

    // 1. 반드시 기존처럼 PCLK 주파수를 명시해서 begin 호출
    bool ok = gfx->begin(ST7262_PANEL_CONFIG_TIMINGS_PCLK_HZ);
    _gfxInitialized = ok;

    if(ok) {

        _fb_pixels = SCREEN_WIDTH * SCREEN_HEIGHT;
        size_t bufferSize = _fb_pixels * sizeof(uint16_t);

        // 2. 중요: 라이브러리가 이미 생성한 내부 버퍼 주소를 0번으로 사용
        _fb_buf[0] = (uint16_t*)gfx->getFramebuffer();

        // 3. 1번 버퍼만 추가로 PSRAM 할당 (더블 버퍼링용)
        _fb_buf[1] = (uint16_t*)heap_caps_aligned_alloc(64, bufferSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

        if(_fb_buf[0] && _fb_buf[1]) {
            memset(_fb_buf[0], 0, bufferSize);
            memset(_fb_buf[1], 0, bufferSize);
            // 라이브러리에게 현재 버퍼 위치 확인 사격
            gfx->setFrameBuffer(_fb_buf[0]);
            Serial.println("[DisplayMgr] Double-buffering initialized with Hardware FB");
        }
        this->BacklightOn();
    }

    xTaskCreate(DisplayMgr::Subscribe, "DisplaySub", 4096, this, 4, &this->taskHandler);
}

void DisplayMgr::StartLVGL() {
    if (_lvglInitialized) return;

    lv_init();

    uint32_t sram_lines = 60;
    size_t sram_buf_size = SCREEN_WIDTH * sram_lines * sizeof(lv_color_t);

    static lv_color_t* sram_work_buf1 = (lv_color_t*)heap_caps_malloc(
        sram_buf_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA
    );
    static lv_color_t* sram_work_buf2 = (lv_color_t*)heap_caps_malloc(
        sram_buf_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA
    );

    // 실패 시 PSRAM으로 Fallback (SRAM 부족 대비)
    if (!sram_work_buf1) sram_work_buf1 = (lv_color_t*)heap_caps_malloc(sram_buf_size, MALLOC_CAP_SPIRAM);
    if (!sram_work_buf2) sram_work_buf2 = (lv_color_t*)heap_caps_malloc(sram_buf_size, MALLOC_CAP_SPIRAM);

    // 3. LVGL 드로잉 버퍼 설정 (SRAM 더블 버퍼링)
    lv_disp_draw_buf_init(&_draw_buf, sram_work_buf1, sram_work_buf2, SCREEN_WIDTH * sram_lines);

    // 4. 디스플레이 드라이버 설정
    lv_disp_drv_init(&_disp_drv);
    _disp_drv.hor_res = SCREEN_WIDTH;
    _disp_drv.ver_res = SCREEN_HEIGHT;
    _disp_drv.flush_cb = DisplayMgr::lvgl_flush_cb; // 앞서 작성한 부분 복사 콜백
    _disp_drv.draw_buf = &_draw_buf;
    _disp_drv.user_data = this;

    // 중요: 0(부분 업데이트)으로 설정해야 SRAM 버퍼를 활용해 필요한 곳만 복사함
    _disp_drv.full_refresh = 0;
    _disp_drv.direct_mode = 0;

    // 안티앨리어싱 비활성화 (CPU 부하 감소)
    _disp_drv.antialiasing = 0;

    lv_disp_drv_register(&_disp_drv);

    // 4. 파일 시스템 드라이버 설정
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

    // 5. PNG 디코더 초기화
    lv_png_init();

    _lvglInitialized = true;
    Serial.println("[DisplayMgr] LVGL Started with SRAM Draw Buffer");

    // PSRAM 사용량 로그
    size_t total_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    Serial.printf("[PSRAM] Total: %d, Free: %d, Used: %d bytes\n",
                  total_psram, free_psram, total_psram - free_psram);
}

void DisplayMgr::lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p)
{
    DisplayMgr* self = (DisplayMgr*)drv->user_data;

    // drv->user_data가 정상적으로 전달되었는지 확인
    if (self && self->gfx) {
        /* * [핵심] SRAM 작업 버퍼(color_p)에 그려진 사각형 영역만
         * PSRAM 프레임버퍼(현재 출력 중인 버퍼)로 복사합니다.
         * Arduino_GFX의 draw16bitRGBBitmap은 내부적으로 최적화된 복사를 수행합니다.
         */
        uint32_t w = lv_area_get_width(area);
        uint32_t h = lv_area_get_height(area);

        self->gfx->draw16bitRGBBitmap(
            area->x1,       // 시작 X 좌표
            area->y1,       // 시작 Y 좌표
            (uint16_t*)color_p,
            w,
            h
        );

        #if CONFIG_IDF_TARGET_ESP32S3
        uint32_t size = w * h * sizeof(uint16_t);
        #endif

        // 하드웨어에 전송 완료(V-Sync 동기화 등) 알림
        if(lv_disp_flush_is_last(drv))
        {
            self->gfx->flush();
            delay(8);
        }
    }

    // LVGL에 플러시 완료를 알려 다음 렌더링을 진행하게 함
    lv_disp_flush_ready(drv);
}

void DisplayMgr::PlayGifTask(void* pvParameters)
{
    DisplayMgr* self = static_cast<DisplayMgr*>(pvParameters);
    SystemAPI* system = SystemAPI::getInstance();

    if (self->_pendingGifData != nullptr) {
        if (system->LockGif()) {
            Serial.println("[DisplayMgr] === GIF Playback Start ===");
            self->PlayGifFromMemory(self->_pendingGifData, self->_pendingGifSize, false);
            system->UnlockGif();
            Serial.println("[DisplayMgr] === GIF Playback End ===\n");
        }
    }

    // --- 추가 및 수정된 로직 시작 ---

    // 1. GIF가 사용하던 더블 버퍼링 상태를 정리하고 LVGL이 쓸 기본 버퍼로 고정
    // LVGL 드라이버는 보통 _fb_buf[0]을 베이스로 복사하므로 0번으로 맞춥니다.
    self->gfx->setFrameBuffer(self->_fb_buf[0]);
    self->gfx->fillScreen(0x0000); // 검은색으로 밀기
    self->gfx->flush();

    Serial.println("[DisplayMgr] Transitioning to LVGL...");
    self->StartLVGL();

    if(system->LockLvgl(pdMS_TO_TICKS(100))) {
        GaugeInit(); // UI 생성

        // 2. 중요: UI가 생성된 직후에 LVGL에게 "전체 화면이 더러워졌으니 다시 그려라"라고 명령
        // 이 명령이 있어야 LVGL이 flush_cb를 즉시 호출하여 화면을 업데이트합니다.
        lv_obj_invalidate(lv_scr_act());

        system->UnlockLvgl();
        Serial.println("[DisplayMgr] LVGL UI Created & Invalidated");
    }

    // 3. LVGL 핸들러 실행
    xTaskCreatePinnedToCore(DisplayMgr::HandleLvglTask, "LvglTask", 8192, self, 10, nullptr, 1);

    // --- 로직 끝 ---

    vTaskDelay(pdMS_TO_TICKS(500));
    system->storageSubscriber.SetEvent(STORAGE_CLEAR_LOADED_PSRAM);

    self->_splashFinished = true;
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
                    system->storageSubscriber.SetEvent(STORAGE_LOAD_TO_PSRAM, event.data);
                    int wait = 0;
                    while (!system->isGifLoaded && wait++ < 100) vTaskDelay(pdMS_TO_TICKS(100));
                    if(system->isGifLoaded) {
                        if (system->LockGif()) {
                            GIFMemory* gifObj = system->GetPsramObjPtr();
                            self->_pendingGifData = gifObj->data;
                            self->_pendingGifSize = gifObj->size;
                            system->UnlockGif();
                        }
                        Serial.println("[DisplayMgr] Starting GIF Task");
                        xTaskCreatePinnedToCore(DisplayMgr::PlayGifTask, "GifTask", 16384, self, 5, &self->taskHandler, 1);
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
            // LV_TICK_CUSTOM이 켜져 있으면 내부에서 알아서 millis()를 참조합니다.
            lv_timer_handler();
            system->UnlockLvgl();
        }

        // 60fps (1000ms / 60 = 16.6ms)를 위한 정밀 대기
        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(16));
    }
}

void DisplayMgr::GifDrawStatic(GIFDRAW *pDraw)
{
    DisplayMgr* self = static_cast<DisplayMgr*>(pDraw->pUser);
    // 현재 하드웨어가 출력 중이지 않은 'Back Buffer'에 그립니다.
    uint16_t* backBuf = self->_fb_buf[self->_fb_active ^ 1];
    if (!backBuf) return;

    // 중앙 정렬 계산 (iCanvasWidth/Height 대신 메서드 활용)
    int canvasW = self->_gif.getCanvasWidth();
    int canvasH = self->_gif.getCanvasHeight();
    int offsetX = (SCREEN_WIDTH - canvasW) / 2;
    int offsetY = (SCREEN_HEIGHT - canvasH) / 2;

    int dstY = pDraw->iY + pDraw->y + offsetY;
    int dstX = pDraw->iX + offsetX;

    if (dstY < 0 || dstY >= SCREEN_HEIGHT) return;

    uint16_t* pDest = &backBuf[dstY * SCREEN_WIDTH + dstX];
    uint8_t* pSrc = pDraw->pPixels;
    uint16_t* pPalette = pDraw->pPalette;

    for (int i = 0; i < pDraw->iWidth; i++) {
        uint8_t idx = pSrc[i];
        if (pDraw->ucHasTransparency && idx == pDraw->ucTransparent) {
            pDest++;
        } else {
            *pDest++ = pPalette[idx];
        }
    }
}

bool DisplayMgr::PlayGifFromMemory(uint8_t* pData, size_t iSize, bool loop)
{
    if (!_gfxInitialized || !pData) return false;

    _gif.begin(GIF_PALETTE_RGB565_LE);
    if (!_gif.open(pData, (int)iSize, DisplayMgr::GifDrawStatic)) {
        Serial.println("[DisplayMgr] GIF Open Failed!");
        return false;
    }

    Serial.printf("[DisplayMgr] GIF Started: %dx%d (Speed: %.1fx)\n",
                  _gif.getCanvasWidth(), _gif.getCanvasHeight(), 30.0f);

    int frameCount = 0;
    do {
        int delayMs = 0;
        while (_gif.playFrame(true, &delayMs, this) > 0) {
            _fb_active ^= 1;
            gfx->setFrameBuffer(_fb_buf[_fb_active]);
            gfx->flush();

            memcpy(_fb_buf[_fb_active ^ 1], _fb_buf[_fb_active], _fb_pixels * sizeof(uint16_t));

            frameCount++;
            if (ulTaskNotifyTake(pdTRUE, 0)) { loop = false; break; }

            // --- 속도 조절 핵심 로직 ---
            int adjustedDelay = (int)(delayMs / 45);

            // 1ms 이하면 yield()만 수행하여 CPU 점유를 방지하고 즉시 다음 프레임 진행
            if (adjustedDelay > 0) {
                vTaskDelay(pdMS_TO_TICKS(adjustedDelay));
            } else {
                yield();
            }
        }
        if (loop) _gif.reset();
    } while (loop);

    _gif.close();
    Serial.printf("[DisplayMgr] GIF Finished. Total Frames: %d\n", frameCount);
    return true;
}

void DisplayMgr::BacklightOn() { digitalWrite(GPIO_BCKL, HIGH); }
void DisplayMgr::BacklightOff() { digitalWrite(GPIO_BCKL, LOW); }
void DisplayMgr::Println(const String& text) { PushLine(text); if (_gfxInitialized) Redraw(); }
void DisplayMgr::Printf(const String& text) { AppendToLastLine(text); if (_gfxInitialized) Redraw(); }
void DisplayMgr::PushLine(const String& line) { _lines.push_back(line); if (_lines.size() > CONSOLE_ROWS) _lines.erase(_lines.begin()); }
void DisplayMgr::AppendToLastLine(const String& text) { if (_lines.empty()) _lines.push_back(text); else _lines.back() += text; }

void DisplayMgr::Redraw() {
    if (_lvglInitialized) return; // LVGL 시작 후에는 기존 Redraw 사용 안함
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
void DisplayMgr::StopGif() { if (this->taskHandler) xTaskNotifyGive(this->taskHandler); }

void DisplayMgr::CreateMainBackground()
{
    // lv_obj_t* battGauge = lv_img_create(lv_scr_act());
    // lv_img_set_src(battGauge, "S:/assets/batteryGauge.png");
    // lv_obj_center(battGauge);
    // lv_obj_set_size(battGauge, 100, 200);

    // // 2. 바늘 (C-Array 방식)
    // lv_obj_t* ui_needle = lv_img_create(lv_scr_act());
    // lv_img_set_src(ui_needle, &needle);

    // // 게이지 중앙에 바늘 정렬
    // lv_obj_align_to(ui_needle, battGauge, LV_ALIGN_CENTER, 0, 0);

    // // 피벗 설정 (이미지 원본 크기에 따라 조정 필요)
    // // 예: 바늘 이미지 폭이 20, 높이가 100이라면 중앙 하단은 (10, 90)
    // lv_img_set_pivot(ui_needle, 10, 50);

    // // 각도 (0.1도 단위, 50도는 500)
    // lv_img_set_angle(ui_needle, 500);

    // Serial.println("[DisplayMgr] Main Background Created");
}