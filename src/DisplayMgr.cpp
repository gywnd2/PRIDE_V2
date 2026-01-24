#include "DisplayMgr.h"
#include <CommonApi.h>
#include <StorageMgr.h>
#include <lvgl.h>

void DisplayMgr::Init()
{
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
        ST7262_PANEL_CONFIG_TIMINGS_PCLK_HZ,
        false,
        ST7262_PANEL_CONFIG_TIMINGS_FLAGS_DE_IDLE_HIGH,
        ST7262_PANEL_CONFIG_TIMINGS_FLAGS_PCLK_IDLE_HIGH,
        0
    );

    gfx = new Arduino_RGB_Display(
        SCREEN_WIDTH, SCREEN_HEIGHT, rgbPanel, 0, true
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

void DisplayMgr::StartLVGL()
{
    if (_lvglInitialized) return;

    lv_init();

    // GIF가 남긴 찌꺼기를 완전히 지우기 위해 검은색으로 밀어버림
    memset(_fb_buf[0], 0, _fb_pixels * sizeof(uint16_t));
    memset(_fb_buf[1], 0, _fb_pixels * sizeof(uint16_t));

    // Full Refresh 모드에서는 버퍼 두 개를 넘겨주는 것이 정석입니다.
    lv_disp_draw_buf_init(&_draw_buf, _fb_buf[0], _fb_buf[1], _fb_pixels);

    lv_disp_drv_init(&_disp_drv);
    _disp_drv.hor_res = SCREEN_WIDTH;
    _disp_drv.ver_res = SCREEN_HEIGHT;
    _disp_drv.flush_cb = DisplayMgr::lvgl_flush_cb;
    _disp_drv.draw_buf = &_draw_buf;

    _disp_drv.full_refresh = 0;
    _disp_drv.user_data = this;

    lv_disp_drv_register(&_disp_drv);
    _lvglInitialized = true;

    gfx->setFrameBuffer(_fb_buf[0]);
    gfx->fillScreen(0x0000); // 일단 검은색으로 밀기
    gfx->flush();

    Serial.println("[DisplayMgr] LVGL Started and Driver Registered");
}

void DisplayMgr::lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p)
{
    DisplayMgr* self = (DisplayMgr*)drv->user_data;
    if (self && self->gfx) {
        // [수정] Full Refresh가 아닐 때를 대비해 영역 복사
        uint16_t w = area->x2 - area->x1 + 1;
        uint16_t h = area->y2 - area->y1 + 1;

        // 하드웨어가 읽고 있는 Framebuffer에 직접 쓰기
        self->gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t*)color_p, w, h);

        if (lv_disp_flush_is_last(drv)) {
            self->gfx->flush(); // 마지막 조각일 때 실제 화면 갱신
        }
    }
    lv_disp_flush_ready(drv);
}

void DisplayMgr::PlayGifTask(void* pvParameters)
{
    DisplayMgr* self = static_cast<DisplayMgr*>(pvParameters);
    SystemAPI* system = SystemAPI::getInstance();

    // 1. 데이터 로드 대기
    int timeout = 50;
    while (self->_pendingGifData == nullptr && timeout-- > 0) vTaskDelay(pdMS_TO_TICKS(100));

    if (self->_pendingGifData != nullptr) {
        if (system->LockGif()) {
            Serial.println("[DisplayMgr] === GIF Playback Start ===");
            self->PlayGifFromMemory(self->_pendingGifData, self->_pendingGifSize, false);
            system->UnlockGif();
            Serial.println("[DisplayMgr] === GIF Playback End ===");
        }
    } else {
        Serial.println("[DisplayMgr] No GIF data to play!");
    }

    // 2. 중요: GIF 종료 후 LCD 컨트롤러가 버퍼를 완전히 잡을 수 있도록 아주 짧은 휴식
    vTaskDelay(pdMS_TO_TICKS(100));

    // 3. LVGL 시작
    self->StartLVGL();

    // 4. LVGL UI 생성
    // 생성할 때만 짧게 Lock
    if(system->LockLvgl(pdMS_TO_TICKS(100))) {
        lv_obj_t* scr = lv_scr_act();
        lv_obj_set_style_bg_color(scr, lv_color_hex(0x0000FF), 0); // 파란색으로 테스트
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

        lv_obj_t* label = lv_label_create(scr);
        lv_label_set_text(label, "LVGL READY");
        lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_center(label);

        system->UnlockLvgl();
        // 여기서 lv_timer_handler()를 호출하지 마세요! loop()에서 처리하게 둡니다.
        Serial.println("[DisplayMgr] LVGL UI Created");
    }

    // 5. 메모리 정리 전 대기 (LVGL이 첫 스캔라인을 다 그릴 시간 확보)
    vTaskDelay(pdMS_TO_TICKS(1000));
    system->storageSubscriber.SetEvent(STORAGE_CLEAR_LOADED_PSRAM);

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