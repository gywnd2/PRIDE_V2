#ifndef __DISPLAY__
#define __DISPLAY__

#include <Arduino.h>
#include <AnimatedGIF.h>
#include <Arduino_GFX_Library.h>
#include <lvgl.h>
#include <vector>

#define SCREEN_WIDTH 800
#define SCREEN_HEIGHT 480

#define CONSOLE_ROWS 30

class DisplayMgr
{
    private:
        Arduino_ESP32RGBPanel *rgbPanel = nullptr;
        Arduino_RGB_Display *gfx = nullptr;
        bool _gfxInitialized = false;
        bool _lvglInitialized = false;

        std::vector<String> _lines;
        AnimatedGIF _gif;

        // Double-buffering 및 LVGL 관련 멤버
        uint16_t* _fb_buf[2] = {nullptr, nullptr};
        uint8_t _fb_active = 0;
        size_t _fb_pixels = 0;

        uint8_t* _pendingGifData = nullptr;
        size_t _pendingGifSize = 0;

        // LVGL 드라이버 구조체
        lv_disp_draw_buf_t _draw_buf;
        lv_disp_drv_t _disp_drv;

        // 내부 콜백 및 태스크
        static void GifDrawStatic(GIFDRAW *pDraw);
        static void PlayGifTask(void* pvParameters);
        static void Subscribe(void* pvParameters);
        static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p);

        TaskHandle_t taskHandler = nullptr;

    public:
        DisplayMgr() { Serial.println("====DisplayMgr Instance Created"); }
        ~DisplayMgr() { Serial.println("~~~~DisplayMgr Instance Deleted"); }

        void Init();
        void StartLVGL(); // GIF 종료 후 LVGL 엔진 시동

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
        bool PlayGifFromMemory(uint8_t* pData, size_t iSize, bool loop);
        void StopGif();

        bool IsLvglInitialized() { return _lvglInitialized; }
};

#endif