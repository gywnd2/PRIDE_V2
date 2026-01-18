#ifndef __STORAGE__
#define __STORAGE__

#include <Arduino.h>
#include <SD.h>
#include <vector>
#include <SPI.h>
#include <AnimatedGIF.h>

struct GIFMemory
{
    uint8_t* data;
    size_t size;
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

        static void* GIFOpen(const char* szFilename, int32_t *pFileSize);
        static void GIFClose(void *pHandle);
        static int32_t GIFRead(GIFFILE *pFile, uint8_t *pBuf, int32_t iLen);
        static int32_t GIFSeek(GIFFILE *pFile, int32_t iPosition);

        GIFMemory LoadGifToPSRAM(const char* path);
        void FreeGifFromPSRAM(GIFMemory& mem);
};

#endif