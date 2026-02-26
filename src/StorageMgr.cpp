#include <StorageMgr.h>
#include <CommonApi.h>

#define TEST_LOG(fmt, ...) Serial.printf("[StorageMgr] " fmt "\n", ##__VA_ARGS__)
#define TEST_LINE() Serial.printf("[StorageMgr] %s\n", __func__)

static constexpr const char* STORAGE_LOG_DIR = "/log";
static constexpr uint32_t STORAGE_LOG_MAX_INDEX = 9999;

void StorageMgr::Init()
{
    TEST_LINE();
    if(!SD.begin(TF_CS, SPI, 40000000))
    {
        Serial.println("[StorageMgr] SD Card Mount Failed");
        TEST_LOG("SD.begin failed");
    }
    else
    {
        Serial.println("[StorageMgr] SD Card Mounted Successfully");
        TEST_LOG("SD.begin success");
        if (_logMutex == nullptr) {
            _logMutex = xSemaphoreCreateMutex();
        }
        TEST_LOG("log mutex=%p", _logMutex);
        if (_logMutex && xSemaphoreTake(_logMutex, pdMS_TO_TICKS(300)) == pdTRUE) {
            PrepareNextLogFileLocked();
            xSemaphoreGive(_logMutex);
        }

        this->ScanDirectory("/", 0);
        TEST_LOG("ScanDirectory finished, entries=%u", (unsigned int)this->fileList.size());

        SystemAPI* system = SystemAPI::getInstance();
        TEST_LOG("SystemAPI ptr=%p", system);
        if (system->LockGif(pdMS_TO_TICKS(500))) {
            TEST_LOG("LockGif acquired");
            GIFMemory* objPtr = system->GetPsramObjPtr();
            if (objPtr) {
                if (objPtr->data != nullptr) {
                    heap_caps_free(objPtr->data);
                    objPtr->data = nullptr;
                    objPtr->size = 0;
                }
                *objPtr = this->LoadGifToPSRAM("/anim/splash.gif");
                system->isGifLoaded = (objPtr->data != nullptr);
                TEST_LOG("Splash GIF preload: %s (%u bytes)",
                         system->isGifLoaded ? "OK" : "FAIL",
                         (unsigned int)objPtr->size);
                TEST_LOG("preload result loaded=%d size=%u",
                         system->isGifLoaded ? 1 : 0, (unsigned int)objPtr->size);
            }
            system->UnlockGif();
        } else {
            Serial.println("[StorageMgr] Splash GIF preload skipped (gif mutex timeout)");
            TEST_LOG("LockGif timeout");
        }

        if (taskHandler != NULL) {
            vTaskDelete(taskHandler);
            taskHandler = NULL;
        }

        BaseType_t ret = xTaskCreatePinnedToCore(
            StorageMgr::Subscribe,
            "StorageEventSubscriber",
            8192,
            this,
            1,
            &taskHandler,
            0
        );
        if (ret != pdPASS) {
            Serial.println("[StorageMgr] Critical: StorageEventSubscriber task create failed");
        } else {
            TEST_LOG("StorageEventSubscriber task created");
        }
    }
}

void StorageMgr::Subscribe(void* pvParameters)
{
    TEST_LINE();
    StorageMgr* self = static_cast<StorageMgr*>(pvParameters);
    SystemAPI* system = SystemAPI::getInstance();
    StorageEventSubscriber& subscriber = system->storageSubscriber;

    StorageEventData event;

    while (true)
    {
        if(subscriber.ReceiveEvent(&event, portMAX_DELAY))
        {
            TEST_LOG("Subscribe: %d", event.type);
            switch(event.type)
            {
                case STORAGE_EVENT_TYPE::STORAGE_EVENT_NONE:
                {
                    break;
                }
                case STORAGE_EVENT_TYPE:: STORAGE_SCAN:
                {
                    self->ScanDirectory("/", 0);
                    break;
                }
                case STORAGE_EVENT_TYPE::STORAGE_READ:
                {
                    break;
                }
                case STORAGE_EVENT_TYPE::STORAGE_WRITE:
                {
                    break;
                }
                case STORAGE_EVENT_TYPE::STORAGE_LOAD_TO_PSRAM:
                {
                    String filePath = String(event.filePath);
                    GIFMemory* objPtr = system->GetPsramObjPtr();
                    if(objPtr == nullptr) {
                        Serial.println("[StorageMgr] Critical Error: gifObj pointer is NULL");
                        break;
                    }

                    if (system->LockGif()) {
                        if(objPtr->data != nullptr) {
                            heap_caps_free(objPtr->data);
                            objPtr->data = nullptr;
                            objPtr->size = 0;
                        }

                        *objPtr = self->LoadGifToPSRAM(filePath.c_str());
                        system->isGifLoaded = (objPtr->data != nullptr);
                        system->UnlockGif();
                        Serial.println("[StorageMgr] Successfully loaded gif to PSRAM");
                    } else {
                        Serial.println("[StorageMgr] Failed to acquire GIF lock for loading");
                    }
                    break;
                }
                case STORAGE_EVENT_TYPE::STORAGE_CLEAR_LOADED_PSRAM:
                {
                    GIFMemory* objPtr = system->GetPsramObjPtr();
                    if (system->LockGif()) {
                        if(objPtr && objPtr->data != nullptr) {
                            heap_caps_free(objPtr->data);
                            objPtr->data = nullptr;
                            objPtr->size = 0;
                        }
                        system->isGifLoaded = false;
                        system->UnlockGif();
                        Serial.println("[StorageMgr] Cleared gif in PSRAM");
                    } else {
                        Serial.println("[StorageMgr] Failed to acquire GIF lock for clearing");
                    }
                    break;
                }
                case STORAGE_EVENT_TYPE::STORAGE_APPEND_LOG:
                {
                    if (!self->AppendRuntimeLogLine(event.filePath)) {
                        TEST_LOG("log append failed");
                    }
                    break;
                }
                default:
                    TEST_LOG("Subscribe: Wrong Event type : %d", event.type);
                    break;
            }
        }
    }
}


void StorageMgr::ScanDirectory(const char* path, uint8_t depth)
{
    if (depth == 0) {
        fileList.clear();
    }

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
            TEST_LOG("Dir: %s Depth: %d", entry.name(), depth);

            if (String(entry.name()) != String("System Volume Information"))
            {
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
            TEST_LOG("File: %s Depth: %d Size: %u bytes", entry.name(), depth, entry.size());
        }
        entry.close();
    }
    dir.close();
}

bool StorageMgr::SDExists(const char* path)
{
    return SD.exists(path);
}

File StorageMgr::SDOpen(const char* path, const char* mode)
{
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

bool StorageMgr::EnsureLogDir()
{
    if (SD.exists(STORAGE_LOG_DIR)) return true;
    bool ok = SD.mkdir(STORAGE_LOG_DIR);
    TEST_LOG("log dir create %s (ok=%d)", STORAGE_LOG_DIR, ok ? 1 : 0);
    return ok;
}

bool StorageMgr::PrepareNextLogFileLocked()
{
    if (_activeLogPath.length() > 0) return true;
    if (!EnsureLogDir()) return false;

    char candidate[32] = {0};
    for (uint32_t idx = 1; idx <= STORAGE_LOG_MAX_INDEX; ++idx) {
        snprintf(candidate, sizeof(candidate), "%s/log_%lu.txt",
                 STORAGE_LOG_DIR, (unsigned long)idx);
        if (!SD.exists(candidate)) {
            File f = SD.open(candidate, FILE_WRITE);
            if (!f) {
                TEST_LOG("failed to create runtime log file: %s", candidate);
                return false;
            }
            f.printf("[BOOT] runtime log start ms=%lu\n", (unsigned long)millis());
            f.close();

            _activeLogPath = String(candidate);
            _activeLogIndex = idx;
            TEST_LOG("runtime log file=%s", _activeLogPath.c_str());
            return true;
        }
    }

    TEST_LOG("runtime log file index exhausted (max=%lu)",
             (unsigned long)STORAGE_LOG_MAX_INDEX);
    return false;
}

bool StorageMgr::AppendRuntimeLogLine(const char* line)
{
    if (!line || line[0] == '\0') return false;
    if (_logMutex == nullptr) return false;
    if (xSemaphoreTake(_logMutex, pdMS_TO_TICKS(120)) != pdTRUE) return false;

    bool ok = false;
    do {
        if (!PrepareNextLogFileLocked()) break;

        File f = SD.open(_activeLogPath.c_str(), FILE_WRITE);
        if (!f) {
            TEST_LOG("failed to open runtime log file: %s", _activeLogPath.c_str());
            break;
        }
        f.printf("[%lu] %s\n", (unsigned long)millis(), line);
        f.close();
        ok = true;
    } while (false);

    xSemaphoreGive(_logMutex);
    return ok;
}

GIFMemory StorageMgr::LoadGifToPSRAM(const char* path) {
    TEST_LOG("LoadGifToPSRAM path=%s", path ? path : "(null)");
    GIFMemory mem;

    File f = SD.open(path, FILE_READ);
    if (!f || f.isDirectory()) {
        TEST_LOG("Load PSRAM Error: Cannot open %s", path);
        return mem;
    }

    size_t fileSize = f.size();
    mem.data = (uint8_t*)heap_caps_malloc(fileSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (mem.data == nullptr) {
        TEST_LOG("Load PSRAM Error: Failed to allocate %u bytes", (unsigned int)fileSize);
        f.close();
        return mem;
    }

    size_t bytesRead = f.read(mem.data, fileSize);
    f.close();

    if (bytesRead != fileSize) {
        TEST_LOG("Load PSRAM Error: Read mismatch (%u/%u)",
                 (unsigned int)bytesRead, (unsigned int)fileSize);
        heap_caps_free(mem.data);
        mem.data = nullptr;
        return mem;
    }

    mem.size = fileSize;
    TEST_LOG("LoadGifToPSRAM success size=%u", (unsigned int)mem.size);
    return mem;
}

void StorageMgr::FreeGifFromPSRAM(GIFMemory& mem) {
    TEST_LOG("FreeGifFromPSRAM data=%p size=%u", mem.data, (unsigned int)mem.size);
    if (mem.data != nullptr) {
        heap_caps_free(mem.data);
        mem.data = nullptr;
        mem.size = 0;
    }
}
