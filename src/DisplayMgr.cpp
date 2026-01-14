#include "DisplayMgr.h"

DisplayMgr::DisplayMgr()
{
    Serial.println("====DisplayMgr");
}

DisplayMgr::~DisplayMgr()
{
    Serial.println("~~~~DisplayMgr");
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
            Serial.println("[DisplayMgr] Double-buffering enabled & Buffers cleared");
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

    // 3. 텍스트 설정
    gfx->setTextColor(RGB565_WHITE);
    gfx->setTextSize(2);

    // 4. 저장된 로그 라인 출력 전 디버깅 (로그 유실 확인용)
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

    // 5. 하드웨어에 즉시 반영
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

    _gifPlaying = true;
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

            if (!_gifPlaying) break;
            if (delayMs > 0) delay(delayMs);
            else delay(1);
        }

        if (loop && _gifPlaying) _gif.reset();
        else break;
    } while (loop && _gifPlaying);

    _gif.close();
    _gifPlaying = false;

    // [핵심] 재생 종료 후 로그 복구
    Serial.println("[DisplayMgr] SD GIF finished. Restoring log screen...");
    this->Redraw();

    return true;
}

void DisplayMgr::StopGif()
{
    _gifPlaying = false;
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

    // --- [중앙 정렬 로직 수정] ---
    // 구조체에 iCanvasHeight가 없으므로 _gif 객체에서 직접 가져옵니다.
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

    _gifPlaying = true;
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

            if (!_gifPlaying) break;

            // --- 속도 조절 로직 ---
            if (delayMs > 0) {
                // delayMs를 speed로 나누어 대기 시간을 조절합니다.
                // 예: speed가 2.0이면 delay는 절반이 되어 2배 빠르게 재생됩니다.
                int adjustedDelay = (int)(delayMs / 6);
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

        if (loop && _gifPlaying) {
            _gif.reset();
        } else {
            break;
        }
    } while (loop && _gifPlaying);

    _gif.close();
    _gifPlaying = false;

    // 재생이 끝났으므로 로그 화면으로 복구
    Serial.println("[DisplayMgr] Memory GIF finished. Restoring log screen...");
    this->Redraw();

    return true;
}