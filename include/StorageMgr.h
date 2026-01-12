#ifndef __STORAGE__
#define __STORAGE__

#include <Arduino.h>
#include <SD.h>

#define SD_CS_PIN 5

class StorageMgr
{
    private:


    public:
        StorageMgr();
        ~StorageMgr();

        void Init();
        void Exists(const char* path);
        File OpenFile(const char* path, const char* mode);
        size_t ReadAll(const char* path, uint8_t* buffer, size_t maxLen);

};

#endif