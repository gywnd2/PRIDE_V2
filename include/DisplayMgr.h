#ifndef __DISPLAY__
#define __DISPLAY__

#include <Arduino.h>
#include <ui.h>
#include <AnimatedGIF.h>
#include <Arduino_GFX_Library.h>

#define SCREEN_WIDTH 800
#define SCREEN_HEIGHT 480
#define SCREENBUFFER_SIZE_PIXELS 4096

class DisplayMgr
{
    private:
        Arduino_ESP32RGBPanel *rgbPanel = nullptr;
        Arduino_RGB_Display *display = nullptr;

    public:
        DisplayMgr();
        ~DisplayMgr();
        void Init();
        void DrawTestScreen();
        void BacklightOn();
        void BacklightOff();
        void TestBacklight();
};

#endif