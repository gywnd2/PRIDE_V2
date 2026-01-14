#include <StorageMgr.h>

StorageMgr::StorageMgr()
{
    Serial.println("====StorageMgr");
}

StorageMgr::~StorageMgr()
{
    Serial.println("~~~~StorageMgr");
}

void StorageMgr::Init()
{
    if(!SD.begin(TF_CS, SPI, 40000000))
    {
        Serial.println("[StorageMgr] SD Card Mount Failed");
    }
    else
    {
        Serial.println("[StorageMgr] SD Card Mounted Successfully");
    }
}

void StorageMgr::ScanDirectory(const char* path, uint8_t depth)
{
    File dir = SD.open(path);
    if(!dir || !dir.isDirectory()) return;

    File entry;
    while(true)
    {
        entry = dir.openNextFile();
        if(!entry) break;

        FileNode node;
        node.fileName = String(entry.name());
        node.isDirectory = entry.isDirectory();
        node.depth = depth;

        this->fileList.push_back(node);
        if (entry.isDirectory())
        {
            Serial.printf("[StorageMgr] Dir: %s Depth: %d\n", entry.name(), depth);

            // Skip Windows metadata folder, otherwise recurse into it
            if (String(entry.name()) != String("System Volume Information"))
            {
                // Build child path (path may not end with '/')
                String childPath = String(path);
                if (!childPath.endsWith("/")) childPath += "/";
                childPath += entry.name();

                this->ScanDirectory(childPath.c_str(), depth + 1);
            }
            else
            {
                Serial.println("[StorageMgr] Skipping \"System Volume Information\"");
            }
        }
        else
        {
            Serial.printf("[StorageMgr] File: %s Depth: %d Size: %u bytes\n", entry.name(), depth, entry.size());
        }
        entry.close();
    }
    dir.close();
}

// Public SD helpers
bool StorageMgr::SDExists(const char* path)
{
    return SD.exists(path);
}

File StorageMgr::SDOpen(const char* path, const char* mode)
{
    // mode: "r" or "w"
    if (mode && strcmp(mode, "w") == 0)
    {
        return SD.open(path, FILE_WRITE);
    }
    return SD.open(path);
}

size_t StorageMgr::SDReadAll(const char* path, uint8_t* buffer, size_t maxLen)
{
    File f = SD.open(path);
    if (!f) return 0;
    size_t read = 0;
    while (f.available() && read < maxLen)
    {
        int r = f.read(&buffer[read], (int)min((size_t)255, maxLen - read));
        if (r <= 0) break;
        read += r;
    }
    f.close();
    return read;
}

bool StorageMgr::SDRemove(const char* path)
{
    return SD.remove(path);
}

void* StorageMgr::GIFOpen(const char *szFilename, int32_t *pFileSize)
{
    if (!szFilename) return nullptr;

    // 1. 힙 메모리에 File 객체 공간을 먼저 만듭니다.
    File *pf = new File();

    // 2. 그 객체에 바로 SD.open 결과를 대입합니다.
    *pf = SD.open(szFilename, FILE_READ);

    // 3. 파일이 제대로 열렸는지 확인합니다.
    if (!*pf || pf->isDirectory()) {
        Serial.printf("[StorageMgr] Error: Cannot open %s\n", szFilename);
        if (pf) {
            pf->close();
            delete pf;
        }
        return nullptr;
    }

    *pFileSize = (int32_t)pf->size();
    return (void *)pf;
}

void StorageMgr::GIFClose(void *pHandle)
{
    if (!pHandle) return;
    File* pf = (File*)pHandle;
    // File::isOpen() is not available on all implementations; call close() unconditionally
    pf->close();
    delete pf;
    Serial.printf("[StorageMgr] GIFClose: handle=%p\n", pHandle);
}

int32_t StorageMgr::GIFRead(GIFFILE *pFile, uint8_t *pBuf, int32_t iLen)
{
    File *pf = (File *)pFile->fHandle;
    if (!pf) return 0;

    // 읽을 수 있는 잔여 바이트 계산
    int32_t iBytesRead = (int32_t)pf->read(pBuf, iLen);
    pFile->iPos = (int32_t)pf->position();

    return iBytesRead;
}
int32_t StorageMgr::GIFSeek(GIFFILE *pFile, int32_t iPosition)
{
    if (!pFile || !pFile->fHandle) return -1;
    File *pf = (File*)pFile->fHandle;

    if (pFile->iSize <= 0) return -1; // can't seek in empty file

    if (iPosition < 0) iPosition = 0;
    if (iPosition > pFile->iSize) iPosition = pFile->iSize; // allow seek to EOF

    bool ok = pf->seek((uint32_t)iPosition, SeekSet);
    if (!ok) {
        Serial.printf("[StorageMgr] GIFSeek: seek failed pos=%d\n", iPosition);
        return -1;
    }

    pFile->iPos = iPosition;
    return pFile->iPos;
}

GIFMemory StorageMgr::LoadGifToPSRAM(const char* path) {
    GIFMemory mem = {nullptr, 0};

    // 1. 파일 열기
    File f = SD.open(path, FILE_READ);
    if (!f || f.isDirectory()) {
        Serial.printf("[StorageMgr] Load PSRAM Error: Cannot open %s\n", path);
        return mem;
    }

    // 2. 파일 크기 확인 및 PSRAM 할당
    size_t fileSize = f.size();

    // PSRAM(SPIRAM)에 메모리 할당
    mem.data = (uint8_t*)heap_caps_malloc(fileSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (mem.data == nullptr) {
        Serial.printf("[StorageMgr] Load PSRAM Error: Failed to allocate %u bytes\n", fileSize);
        f.close();
        return mem;
    }

    // 3. 데이터를 메모리로 읽어오기
    size_t bytesRead = f.read(mem.data, fileSize);
    f.close();

    if (bytesRead != fileSize) {
        Serial.printf("[StorageMgr] Load PSRAM Error: Read mismatch (%u/%u)\n", bytesRead, fileSize);
        heap_caps_free(mem.data);
        mem.data = nullptr;
        return mem;
    }

    mem.size = fileSize;
    Serial.printf("[StorageMgr] Successfully loaded %s to PSRAM (%u bytes)\n", path, fileSize);

    return mem;
}

void StorageMgr::FreeGifFromPSRAM(GIFMemory& mem) {
    if (mem.data != nullptr) {
        heap_caps_free(mem.data);
        mem.data = nullptr;
        mem.size = 0;
        Serial.println("[StorageMgr] PSRAM memory freed");
    }
}