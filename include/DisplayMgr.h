#ifndef __DISPLAY__
#define __DISPLAY__

#include <Arduino.h>
#include <ui.h>
#include <AnimatedGIF.h>
#include <Arduino_GFX_Library.h>
#include <vector>
#include <StorageMgr.h>

#define SCREEN_WIDTH 800
#define SCREEN_HEIGHT 480
#define SCREENBUFFER_SIZE_PIXELS 4096

#define CONSOLE_COLS 80
#define CONSOLE_ROWS 30

class DisplayMgr
{
    private:
        Arduino_ESP32RGBPanel *rgbPanel = nullptr;
        Arduino_RGB_Display *gfx = nullptr;
        bool _gfxInitialized = false;
        std::vector<String> _lines;
        int _cursor;

        // Animated GIF instance (reused)
        AnimatedGIF _gif;
        bool _gifPlaying = false;

        // Double-buffering members
        uint16_t* _fb_buf[2] = {nullptr, nullptr};
        uint8_t _fb_active = 0; // index of buffer currently used by panel
        size_t _fb_pixels = 0;

        // Internal draw callback used by AnimatedGIF
        static void GifDrawStatic(GIFDRAW *pDraw);
        static void PlaySplash(void* pvParameters);

        TaskHandle_t _splashTaskHandle = nullptr;

    public:
        DisplayMgr();
        ~DisplayMgr();
        void Init();
        void BacklightOn();
        void BacklightOff();
        void TestBacklight();
        void Println(const String& text);
        void Printf(const String& text);
        void PushLine(const String& line);
        void AppendToLastLine(const String& text);
        void Redraw();
        void Clear();

        bool PlayGifFromSD(const char* path, bool loop = true);
        void StopGif();

        bool PlayGifFromMemory(uint8_t* pData, size_t iSize, bool loop);
};

#endif