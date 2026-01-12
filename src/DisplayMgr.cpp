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


    display = new Arduino_RGB_Display(
        SCREEN_WIDTH,
        SCREEN_HEIGHT,
        rgbPanel,
        0,
        true,
        nullptr,
        GFX_NOT_DEFINED,
        nullptr,
        GFX_NOT_DEFINED
    );
    Serial.println("[DisplayMgr] display created");
}

void DisplayMgr::DrawTestScreen()
{
    if (display->begin())
    {
        display->fillScreen(RGB565_WHITE);
        display->setTextColor(RGB565_BLACK);
        display->setTextSize(4);
        display->setCursor(50, 50);
        display->print("Display Initialized!");
        Serial.println("[DisplayMgr] Display initialized and test screen drawn");
    }
    else
    {
        Serial.println("[DisplayMgr] Failed to begin display");
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