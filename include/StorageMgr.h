#ifndef __STORAGE__
#define __STORAGE__

#include <Arduino.h>
#include <SD.h>
#include <vector>
#include <SPI.h>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

struct GIFMemory
{
    uint8_t* data = nullptr;
    size_t size = 0;
};

struct FileNode
{
    String fileName;
    bool isDirectory;
    uint8_t depth;
};

class StorageMgr
{
    private:
        std::vector<FileNode> fileList;
        size_t ReadAll(const char* path, uint8_t* buffer, size_t maxLen);
        void Exists(const char* path);
        File OpenFile(const char* path, const char* mode);
        TaskHandle_t taskHandler = nullptr;
        SemaphoreHandle_t _logMutex = nullptr;
        String _activeLogPath;
        uint32_t _activeLogIndex = 0;

        bool EnsureLogDir();
        bool PrepareNextLogFileLocked();

        static void Subscribe(void* pvParameters);

    public:
        StorageMgr()
        {
            Serial.println("====StorageMgr");
        }
        ~StorageMgr()
        {
            Serial.println("~~~~StorageMgr");
        }

        void Init();
        void ScanDirectory(const char* path, uint8_t depth);
        std::vector<FileNode> GetFileList() const { return fileList; };

        bool SDExists(const char* path);
        File SDOpen(const char* path, const char* mode = "r");
        size_t SDReadAll(const char* path, uint8_t* buffer, size_t maxLen);
        bool SDRemove(const char* path);
        bool AppendRuntimeLogLine(const char* line);
        String GetActiveLogPath() const { return _activeLogPath; }

        GIFMemory LoadGifToPSRAM(const char* path);
        void FreeGifFromPSRAM(GIFMemory& mem);
};

#endif
