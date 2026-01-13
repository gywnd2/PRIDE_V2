#ifndef __STORAGE__
#define __STORAGE__

#include <Arduino.h>
#include <SD.h>
#include <vector>

#define SD_CS_PIN 5

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

    public:
        StorageMgr();
        ~StorageMgr();

        void Init();
        void ScanDirectory(const char* path, uint8_t depth);
        std::vector<FileNode> GetFileList() const { return fileList; };
};

#endif