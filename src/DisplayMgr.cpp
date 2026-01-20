#include "DisplayMgr.h"
#include <CommonApi.h>
#include <StorageMgr.h>
#include <AnimatedGIF.h>

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
    Serial.println("[DisplayMgr] rgb_panel created");

    gfx = new Arduino_RGB_Display(
        SCREEN_WIDTH,
        SCREEN_HEIGHT,
        rgbPanel,
        0,
        true,
        nullptr,
        GFX_NOT_DEFINED,
        nullptr,
        GFX_NOT_DEFINED,
        0, // col_offset1
        0, // row_offset1
        0, // col_offset2
        0  // row_offset2
    );

    // Initialize the display and remember whether it succeeded
    bool ok = gfx->begin(ST7262_PANEL_CONFIG_TIMINGS_PCLK_HZ);
    _gfxInitialized = ok;
    Serial.printf("[DisplayMgr] gfx->begin returned %d\n", ok);
    Serial.println("[DisplayMgr] gfx created");

    if(ok) {
        _fb_pixels = SCREEN_WIDTH * SCREEN_HEIGHT;
        size_t bufferSize = _fb_pixels * sizeof(uint16_t);

        _fb_buf[0] = (uint16_t*)gfx->getFramebuffer();
        _fb_buf[1] = (uint16_t*)heap_caps_aligned_alloc(64, bufferSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

        if(_fb_buf[1] != nullptr) {
            memset(_fb_buf[0], 0, bufferSize);
            memset(_fb_buf[1], 0, bufferSize);
            Serial.println("[DisplayMgr] Double-buffering enabled");
        }

        this->BacklightOn();
    }

    xTaskCreate(
        DisplayMgr::Subscribe,
        "DisplayEventSubscriber",
        4096,
        this,
        4,
        &this->taskHandler
    );
}

void DisplayMgr::PlayGifTask(void* pvParameters)
{
    DisplayMgr* self = static_cast<DisplayMgr*>(pvParameters);
    SystemAPI* system = SystemAPI::getInstance();

    Serial.println("[DisplayMgr] Task: Waiting for PSRAM data...");

    // 데이터가 로드될 때까지 최대 5초 대기 (main의 LoadGifToPSRAM 완료 대기)
    int timeout = 50;
    while (self->_pendingGifData == nullptr && timeout-- > 0) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (self->_pendingGifData != nullptr) {
        Serial.println("[DisplayMgr] Task: PSRAM data found. Starting play.");
        self->Clear();

        // 재생 중 메모리 해제 방지를 위해 Lock
        if (system->LockGif()) {
            self->PlayGifFromMemory(self->_pendingGifData, self->_pendingGifSize, false);
            system->UnlockGif();
        } else {
            Serial.println("[DisplayMgr] Task: Failed to lock GIF memory");
        }

        // GIF 재생 종료 후 화면 복구 (Deadlock 방지를 위해 Unlock 후 호출)
        self->Redraw();
    } else {
        Serial.println("[DisplayMgr] Task: Timeout waiting for data.");
    }

    SystemAPI::getInstance()->storageSubscriber.SetEvent(STORAGE_CLEAR_LOADED_PSRAM);
    vTaskDelete(NULL);
}

void DisplayMgr::Subscribe(void* pvParameters)
{
    DisplayMgr* self = static_cast<DisplayMgr*>(pvParameters);
    SystemAPI* system = SystemAPI::getInstance();
    DisplayEventSubscriber& subscriber = system->displaySubscriber;
    StorageEventSubscriber& storage = system->storageSubscriber;

    DisplayEventData event;

    while(true)
    {
        // 큐에서 이벤트를 기다림 (Blocking)
        if(subscriber.ReceiveEvent(&event, portMAX_DELAY))
        {
            switch(event.type)
            {
                case DISPLAY_EVENT_TYPE::DIPLAY_EVENT_NONE:
                {
                    break;
                }
                case DISPLAY_EVENT_TYPE::DISPLAY_SHOW_SPLASH:
                {
                    Serial.println("[DisplayMgr] Subscribe : Show splash");
                    // 1. StorageMgr에 로드 요청
                    storage.SetEvent(STORAGE_LOAD_TO_PSRAM, event.data);

                    // 2. 로드 완료 대기 (Polling loop within task)
                    // 주의: 여기서 대기하는 동안 다른 디스플레이 이벤트는 처리되지 않음 (Splash 로딩 중이므로 허용)
                    int waitCount = 0;
                    while (!system->isGifLoaded && waitCount++ < 100) { // 최대 10초 대기
                        vTaskDelay(pdMS_TO_TICKS(100));
                    }

                    if(system->isGifLoaded)
                    {
                        Serial.println("[DisplayMgr] Subscribe : Gif loaded to PSRAM");

                        // 포인터 복사 시점에도 안전하게 Lock
                        if (system->LockGif()) {
                            GIFMemory* gifObj = system->GetPsramObjPtr();
                            if(gifObj->size > 0) {
                                self->_pendingGifData = gifObj->data;
                                self->_pendingGifSize = gifObj->size;
                            }
                            system->UnlockGif();
                        }

                        // Lock을 시도하여 재생 중인지 확인 (Non-blocking)
                        if (system->LockGif(0))
                        {
                            system->UnlockGif(); // 확인했으니 즉시 반환
                            Serial.println("[DisplayMgr] Subscribe : Gif is not playing. Create PlayGifTask");
                            xTaskCreatePinnedToCore(DisplayMgr::PlayGifTask, "GifPlayTask", 16384, self, 5, &self->taskHandler, 1);
                        }
                    }
                    break;
                }
                case DISPLAY_EVENT_TYPE::DISPLAY_UPDATE_OBD_STATUS:
                {
                    String status = "OBD Status: ";
                    status += event.data;
                    self->Println(status);
                    break;
                }
                case DISPLAY_EVENT_TYPE::DISPLAY_UPDATE_VOLTAGE:
                {
                    String status = "Voltage: ";
                    status += event.data;
                    self->Println(status);
                    break;
                }
                case DISPLAY_EVENT_TYPE::DISPLAY_UPDATE_COOLANT:
                {
                    String status = "Coolant: ";
                    status += event.data;
                    self->Println(status);
                    break;
                }
                case DISPLAY_EVENT_TYPE::DISPLAY_UPDATE_CPU_USAGE:
                {
                    String status = "CPU Usage: ";
                    status += event.data;
                    self->Println(status);
                    break;
                }
                case DISPLAY_EVENT_TYPE::DISPLAY_UPDATE_RAM_USAGE:
                {
                    String status = "RAM Usage: ";
                    status += event.data;
                    self->Println(status);
                    break;
                }
                default:
                    Serial.printf("[DisplayMgr] Subscribe: Wrong Event type : %d\n", event.type);
                    break;
            }
        }
    }
}

void DisplayMgr::BacklightOn()
{
    digitalWrite(GPIO_BCKL, HIGH);
}

void DisplayMgr::BacklightOff()
{
    digitalWrite(GPIO_BCKL, LOW);
}

void DisplayMgr::TestBacklight()
{
static uint32_t lastToggleTime = 0;
  if(millis() - lastToggleTime > 2000) { // Toggle backlight every 5 seconds
    static bool backlightOn = false;
    if(backlightOn) {
      Serial.println("[DisplayMgr] Turning backlight OFF");
      this->BacklightOff();
    } else {
      Serial.println("[DisplayMgr] Turning backlight ON");
      this->BacklightOn();
    }
    backlightOn = !backlightOn;
    lastToggleTime = millis();
  }
}

void DisplayMgr::Println(const String& text)
{
    Serial.println(text);
    PushLine(text);
    if (_gfxInitialized)
    {
        Redraw();
    }
}

void DisplayMgr::Printf(const String& text)
{
    Serial.print(text);
    AppendToLastLine(text);
    if (_gfxInitialized)
    {
        Redraw();
    }
}

void DisplayMgr::PushLine(const String& line)
{
    _lines.push_back(line);
    Serial.println("[DisplayMgr] Pushed line: " + line);
    if (_lines.size() > CONSOLE_ROWS)
    {
        _lines.erase(_lines.begin());
    }
}

void DisplayMgr::AppendToLastLine(const String& text)
{
    if (_lines.empty())
    {
        _lines.push_back(text);
    }
    else
    {
        _lines.back() += text;
    }
}

void DisplayMgr::Redraw()
{
    // GIF 재생 중이라면 Lock이 걸려있으므로, 풀릴 때까지 대기 (Blocking)
    if (SystemAPI::getInstance()->LockGif(portMAX_DELAY)) {
        SystemAPI::getInstance()->UnlockGif();
    }

    if (!_gfxInitialized) {
        Serial.println("[DisplayMgr] Redraw skipped: gfx not initialized");
        return;
    }

    // 1. 안전을 위해 현재 활성 버퍼를 0번으로 고정하고 LCD에 설정
    _fb_active = 0;
    gfx->setFrameBuffer(_fb_buf[_fb_active]);

    // 2. 두 버퍼 모두 깨끗하게 청소 (GIF 잔상 제거)
    size_t bufferSize = _fb_pixels * sizeof(uint16_t);
    if (_fb_buf[0]) memset(_fb_buf[0], 0, bufferSize);
    if (_fb_buf[1]) memset(_fb_buf[1], 0, bufferSize);

    // 3. 화면 명시적으로 검은색으로 채우기
    gfx->fillScreen(RGB565_BLACK);
    gfx->flush();

    // 4. 텍스트 설정
    gfx->setTextColor(RGB565_WHITE);
    gfx->setTextSize(2);

    // 5. 저장된 로그 라인 출력 전 디버깅 (로그 유실 확인용)
    if (_lines.empty()) {
        Serial.println("[DisplayMgr] Warning: No lines to redraw (Vector is empty)");
    }

    int y = 1;
    for (const String& line : _lines)
    {
        gfx->setCursor(1, y);
        gfx->print(line);
        y += 20;
    }

    // 6. 하드웨어에 즉시 반영
    gfx->flush();
    Serial.println("[DisplayMgr] Redraw completed and buffer flushed");
}

void DisplayMgr::Clear()
{
    _lines.clear();
    if (_gfxInitialized)
    {
        gfx->fillScreen(RGB565_BLACK);
    }
}

bool DisplayMgr::PlayGifFromSD(const char* path, bool loop)
{
    if (!_gfxInitialized || !gfx) return false;

    // 재생 시작 전 버퍼 동기화
    if (_fb_buf[0] && _fb_buf[1]) {
        memcpy(_fb_buf[_fb_active ^ 1], _fb_buf[_fb_active], _fb_pixels * sizeof(uint16_t));
    }

    _gif.begin(GIF_PALETTE_RGB565_LE);
    int initOk = _gif.open(path, StorageMgr::GIFOpen, StorageMgr::GIFClose, StorageMgr::GIFRead, StorageMgr::GIFSeek, DisplayMgr::GifDrawStatic);
    if (!initOk) return false;

    this->taskHandler = xTaskGetCurrentTaskHandle();

    do {
        int r = 1;
        while (r > 0) {
            int delayMs = 0;
            r = _gif.playFrame(true, &delayMs, this);

            if (r < 0) {
                Serial.println("[DisplayMgr] GIF decode error");
                break;
            }

            if (_fb_buf[0] && _fb_buf[1]) {
                _fb_active ^= 1;
                gfx->setFrameBuffer(_fb_buf[_fb_active]);
                gfx->flush();
                memcpy(_fb_buf[_fb_active ^ 1], _fb_buf[_fb_active], _fb_pixels * sizeof(uint16_t));
            }

            // StopGif()에서 보낸 알림 확인 (종료 요청)
            if (ulTaskNotifyTake(pdTRUE, 0)) {
                r = 0; // 루프 탈출 조건
                loop = false;
                break;
            }

            if (delayMs > 0) delay(delayMs);
            else delay(1);
        }

        if (loop && r != 0) _gif.reset();
        else break;
    } while (loop);

    _gif.close();
    this->taskHandler = nullptr;

    Serial.println("[DisplayMgr] SD GIF finished. Restoring log screen...");
    this->Redraw();

    return true;
}

void DisplayMgr::StopGif()
{
    if (this->taskHandler != nullptr) {
        xTaskNotifyGive(this->taskHandler);
    }
}

void DisplayMgr::GifDrawStatic(GIFDRAW *pDraw)
{
    DisplayMgr* self = static_cast<DisplayMgr*>(pDraw->pUser);
    if (!self || !self->gfx) return;

    uint8_t backIdx = self->_fb_active ^ 1;
    uint16_t* backBuf = self->_fb_buf[backIdx];
    if (!backBuf) return;

    uint16_t fbw = self->gfx->getFrameBufferWidth();
    uint16_t fbh = self->gfx->getFrameBufferHeight();

    int canvasW = pDraw->iCanvasWidth;
    int canvasH = self->_gif.getCanvasHeight();

    int offsetX = (fbw - canvasW) / 2;
    int offsetY = (fbh - canvasH) / 2;

    // 실제 그릴 좌표 (GIF 내 좌표 + 중앙 오프셋)
    int dstY = pDraw->iY + pDraw->y + offsetY;
    int dstX = pDraw->iX + offsetX;
    int w = pDraw->iWidth;

    // 화면 범위를 벗어나면 그리지 않음 (Clipping)
    if (dstY < 0 || dstY >= (int)fbh) return;

    int startX = 0;
    int endX = w;

    if (dstX < 0) {
        startX = -dstX;
        dstX = 0;
    }
    if (dstX + (endX - startX) > (int)fbw) {
        endX = startX + (fbw - dstX);
    }

    // 픽셀 복사 루프
    uint16_t* pDest = &backBuf[dstY * fbw + dstX];
    uint8_t* pSrc = pDraw->pPixels;
    uint16_t* pPalette = pDraw->pPalette;

    for (int i = startX; i < endX; i++) {
        uint8_t idx = pSrc[i];
        if (pDraw->ucHasTransparency && idx == pDraw->ucTransparent) {
            pDest++;
            continue;
        }
        *pDest++ = pPalette[idx];
    }
}

bool DisplayMgr::PlayGifFromMemory(uint8_t* pData, size_t iSize, bool loop)
{
    if (!_gfxInitialized || !pData || iSize == 0) return false;

    _gif.begin(GIF_PALETTE_RGB565_LE);
    if (!_gif.open(pData, (int)iSize, DisplayMgr::GifDrawStatic)) return false;

    this->taskHandler = xTaskGetCurrentTaskHandle();
    Serial.printf("[DisplayMgr] GIF playing started from Memory\n");

    // 재생 전 버퍼 초기화 (이전 텍스트 잔상 제거)
    size_t bufferSize = _fb_pixels * sizeof(uint16_t);
    memset(_fb_buf[0], 0, bufferSize);
    memset(_fb_buf[1], 0, bufferSize);

    do {
        int delayMs = 0;
        // playFrame이 0보다 큰 동안(프레임이 남은 동안) 루프
        while (_gif.playFrame(true, &delayMs, this) > 0) {

            if (_fb_buf[0] && _fb_buf[1]) {
                _fb_active ^= 1; // 버퍼 스왑
                gfx->setFrameBuffer(_fb_buf[_fb_active]);
                gfx->flush();

                // 프레임 연속성을 위해 백 버퍼에 현재 내용 복사
                memcpy(_fb_buf[_fb_active ^ 1], _fb_buf[_fb_active], bufferSize);
            }

            // StopGif()에서 보낸 알림 확인 (종료 요청)
            if (ulTaskNotifyTake(pdTRUE, 0)) {
                loop = false; // 외부 루프 탈출
                break;        // 내부 루프 탈출
            }

            // --- 속도 조절 로직 ---
            if (delayMs > 0) {
                // delayMs를 speed로 나누어 대기 시간을 조절합니다.
                // 예: speed가 2.0이면 delay는 절반이 되어 2배 빠르게 재생됩니다.
                int adjustedDelay = (int)(delayMs / 30);
                if (adjustedDelay > 0) {
                    delay(adjustedDelay);
                } else {
                    // 속도가 너무 빠르면 최소한의 CPU 양보만 수행
                    yield();
                }
            } else {
                delay(1);
            }
        }

        if (loop) {
            _gif.reset();
        } else {
            break;
        }
    } while (loop);

    _gif.close();
    this->taskHandler = nullptr;

    Serial.println("[DisplayMgr] Memory GIF finished. Restoring log screen...");

    return true;
}