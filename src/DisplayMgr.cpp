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
    if (not _gfxInitialized) {
        Serial.println("[DisplayMgr] Redraw skipped: gfx not initialized");
        return;
    }

    gfx->fillScreen(RGB565_BLACK);
    gfx->setTextColor(RGB565_WHITE);
    gfx->setTextSize(2);

    int y = 1;
    for (const String& line : _lines)
    {
        gfx->setCursor(1, y);
        gfx->print(line);
        gfx->print("\n");
        y += 20;
    }
}

void DisplayMgr::Clear()
{
    _lines.clear();
    if (_gfxInitialized)
    {
        gfx->fillScreen(RGB565_BLACK);
    }
}