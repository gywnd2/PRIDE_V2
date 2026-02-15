#ifndef __DISPLAY__
#define __DISPLAY__

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <lvgl.h>
#include <vector>

#define CONSOLE_ROWS 30
struct GIFMemory;

class DisplayMgr
{
    private:
        Arduino_ESP32RGBPanel *rgbPanel = nullptr;
        Arduino_RGB_Display *gfx = nullptr;
        bool _gfxInitialized = false;
        bool _lvglInitialized = false;
        bool _splashFinished = false;

        std::vector<String> _lines;

        // Double-buffering and LVGL memory
        uint16_t* _fb_buf[2] = {nullptr, nullptr};
        size_t _fb_pixels = 0;
        uint8_t _frontFbIndex = 0;
        uint8_t _backFbIndex = 1;

        String _pendingGifPath;
        lv_obj_t* _splashGif = nullptr;
        lv_img_dsc_t _splashGifDsc = {};

        lv_disp_draw_buf_t _draw_buf;
        lv_disp_drv_t _disp_drv;

        static void HandleLvglTask(void *pvParameters);
        static void PlayGifTask(void* pvParameters);
        static void Subscribe(void* pvParameters);
        static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p);

        TaskHandle_t _gifTaskHandler = nullptr;
        TaskHandle_t _eventTaskHandler = nullptr;
        TaskHandle_t _lvglTaskHandler = nullptr;

    public:
        DisplayMgr() { Serial.println("====DisplayMgr Instance Created"); }
        ~DisplayMgr() { Serial.println("~~~~DisplayMgr Instance Deleted"); }

        void Init();
        void StartLVGL();

        void BacklightOn();
        void BacklightOff();
        void TestBacklight();

        void Println(const String& text);
        void Printf(const String& text);
        void PushLine(const String& line);
        void AppendToLastLine(const String& text);
        void Redraw();
        void Clear();

        bool PlayGifFromSD(const char* path);
        bool PlayGifFromMemory(const GIFMemory& gifMem);
        void StopGif();

        bool IsLvglInitialized() { return _lvglInitialized; }
        bool IsSplashFinished() { return _splashFinished; }

        void CreateMainBackground();
};

#endif
