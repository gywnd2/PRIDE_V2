#include <StorageMgr.h>
#include <CommonApi.h>
#include <time.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>

#define TEST_LOG(fmt, ...) UartLogf("[StorageMgr] " fmt "\n", ##__VA_ARGS__)
#define TEST_LINE() UartLogf("[StorageMgr] %s\n", __func__)

static constexpr const char* STORAGE_LOG_DIR = "/log";

#ifndef FILE_APPEND
#define FILE_APPEND "a"
#endif

static bool format_runtime_time_header(char* out, size_t out_len)
{
    if (!out || out_len == 0) return false;
    time_t now = time(nullptr);
    if (now <= 0) return false;

    struct tm tm_local = {};
    if (localtime_r(&now, &tm_local) == nullptr) return false;

    // Consider clock valid only after NTP sync (year >= 2024).
    int year = tm_local.tm_year + 1900;
    if (year < 2024) return false;

    snprintf(out, out_len, "[%02d-%02d-%02d]",
             tm_local.tm_hour,
             tm_local.tm_min,
             tm_local.tm_sec);
    return true;
}

static void copy_trimmed_log_line(char* out, size_t out_len, const char* line)
{
    if (!out || out_len == 0) return;
    out[0] = '\0';
    if (!line) return;

    strncpy(out, line, out_len - 1);
    out[out_len - 1] = '\0';

    size_t len = strlen(out);
    while (len > 0) {
        char c = out[len - 1];
        if (c != '\n' && c != '\r') break;
        out[len - 1] = '\0';
        len--;
    }
}

static bool parse_runtime_log_index(const char* name, uint32_t* outIndex)
{
    if (!name || !outIndex) return false;

    String base = String(name);
    int slash = base.lastIndexOf('/');
    if (slash >= 0) {
        base = base.substring(slash + 1);
    }

    static constexpr const char* PREFIX = "runtime_";
    static constexpr const char* SUFFIX = ".log";
    if (!base.startsWith(PREFIX) || !base.endsWith(SUFFIX)) return false;

    String numberText = base.substring(strlen(PREFIX), base.length() - strlen(SUFFIX));
    if (numberText.length() == 0) return false;
    for (size_t i = 0; i < numberText.length(); ++i) {
        if (!isdigit((unsigned char)numberText[i])) return false;
    }

    *outIndex = (uint32_t)strtoul(numberText.c_str(), nullptr, 10);
    return *outIndex > 0;
}

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

        this->ScanDirectory("/", 0);
        TEST_LOG("ScanDirectory finished, entries=%u", (unsigned int)this->fileList.size());

        SystemAPI* system = SystemAPI::getInstance();
        TEST_LOG("SystemAPI ptr=%p", system);
        uint32_t serviceOdoKm = 0;
        if (this->ReadServiceOdoKm(&serviceOdoKm)) {
            system->PublishServiceOdoKm(serviceOdoKm);
            TEST_LOG("service odo=%u km due=%d",
                     (unsigned int)serviceOdoKm,
                     serviceOdoKm >= SERVICE_ODO_THRESHOLD_KM ? 1 : 0);
        }
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
                case STORAGE_EVENT_TYPE::STORAGE_APPEND_DISPLAY_LOG:
                {
                    if (!self->AppendDisplayRuntimeLogLine(event.filePath)) {
                        TEST_LOG("display log append failed");
                    }
                    break;
                }
                case STORAGE_EVENT_TYPE::STORAGE_FINISH_DISPLAY_LOG_SESSION:
                {
                    self->FinishRuntimeLogSession();
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

bool StorageMgr::EnsureDbDir()
{
    if (SD.exists(SERVICE_ODO_DIR)) return true;
    bool ok = SD.mkdir(SERVICE_ODO_DIR);
    TEST_LOG("db dir create %s (ok=%d)", SERVICE_ODO_DIR, ok ? 1 : 0);
    return ok;
}

bool StorageMgr::ReadServiceOdoKm(uint32_t* outKm)
{
    if (!outKm) return false;
    *outKm = 0;
    if (_logMutex == nullptr) return false;
    if (xSemaphoreTake(_logMutex, pdMS_TO_TICKS(120)) != pdTRUE) return false;

    bool ok = false;
    do {
        if (!EnsureDbDir()) break;
        if (!SD.exists(SERVICE_ODO_FILE_PATH)) {
            ok = true;
            break;
        }

        File f = SD.open(SERVICE_ODO_FILE_PATH, FILE_READ);
        if (!f) break;
        if (f.isDirectory()) {
            f.close();
            break;
        }

        char buf[32] = {0};
        size_t len = f.readBytes(buf, sizeof(buf) - 1);
        f.close();
        buf[len] = '\0';

        char* end = nullptr;
        unsigned long value = strtoul(buf, &end, 10);
        if (end == buf) break;

        *outKm = (uint32_t)value;
        ok = true;
    } while (false);

    xSemaphoreGive(_logMutex);
    return ok;
}

bool StorageMgr::AddServiceOdoKm(uint32_t deltaKm, uint32_t* totalOut)
{
    if (totalOut) *totalOut = 0;
    if (_logMutex == nullptr) return false;
    if (xSemaphoreTake(_logMutex, pdMS_TO_TICKS(180)) != pdTRUE) return false;

    bool ok = false;
    uint32_t total = 0;
    do {
        if (!EnsureDbDir()) break;

        if (SD.exists(SERVICE_ODO_FILE_PATH)) {
            File readFile = SD.open(SERVICE_ODO_FILE_PATH, FILE_READ);
            if (!readFile) break;
            if (readFile.isDirectory()) {
                readFile.close();
                break;
            }

            char buf[32] = {0};
            size_t len = readFile.readBytes(buf, sizeof(buf) - 1);
            readFile.close();
            buf[len] = '\0';

            char* end = nullptr;
            unsigned long value = strtoul(buf, &end, 10);
            if (end == buf) break;
            total = (uint32_t)value;
        }

        if (UINT32_MAX - total < deltaKm) total = UINT32_MAX;
        else total += deltaKm;

        if (SD.exists(SERVICE_ODO_FILE_PATH) && !SD.remove(SERVICE_ODO_FILE_PATH)) {
            break;
        }

        File writeFile = SD.open(SERVICE_ODO_FILE_PATH, FILE_WRITE);
        if (!writeFile) break;
        writeFile.print(total);
        writeFile.close();

        if (totalOut) *totalOut = total;
        TEST_LOG("service odo updated delta=%u total=%u",
                 (unsigned int)deltaKm,
                 (unsigned int)total);
        ok = true;
    } while (false);

    xSemaphoreGive(_logMutex);
    return ok;
}

bool StorageMgr::PrepareNextRuntimeLogFileLocked()
{
    if (_runtimeLogSessionClosed) return false;
    if (_activeLogPath.length() > 0 && _activeLogFile) return true;
    if (_activeLogPath.length() > 0 && !_activeLogFile) _activeLogPath = "";
    if (!EnsureLogDir()) return false;

    uint32_t maxIndex = 0;
    File dir = SD.open(STORAGE_LOG_DIR);
    if (dir && dir.isDirectory()) {
        while (true) {
            File entry = dir.openNextFile();
            if (!entry) break;

            uint32_t index = 0;
            if (!entry.isDirectory() && parse_runtime_log_index(entry.name(), &index)) {
                if (index > maxIndex) maxIndex = index;
            }
            entry.close();
        }
        dir.close();
    }

    _activeLogIndex = maxIndex + 1U;
    _runtimeLogLineNumber = 0;
    _activeLogPath = String(STORAGE_LOG_DIR) + "/runtime_" + String(_activeLogIndex) + ".log";

    _activeLogFile = SD.open(_activeLogPath.c_str(), FILE_APPEND);
    if (!_activeLogFile) {
        TEST_LOG("failed to open display runtime log file: %s", _activeLogPath.c_str());
        _activeLogPath = "";
        return false;
    }

    TEST_LOG("display runtime log file=%s", _activeLogPath.c_str());
    return true;
}

bool StorageMgr::AppendDisplayRuntimeLogLine(const char* line)
{
    if (!line || line[0] == '\0') return false;
    if (_logMutex == nullptr) return false;
    if (xSemaphoreTake(_logMutex, pdMS_TO_TICKS(120)) != pdTRUE) return false;

    bool ok = false;
    do {
        if (_runtimeLogSessionClosed) {
            ok = true;
            break;
        }
        if (!PrepareNextRuntimeLogFileLocked()) break;

        char trimmedLine[480] = {0};
        copy_trimmed_log_line(trimmedLine, sizeof(trimmedLine), line);
        if (trimmedLine[0] == '\0') {
            ok = true;
            break;
        }

        char header[24] = {0};
        if (!format_runtime_time_header(header, sizeof(header))) {
            _runtimeLogLineNumber++;
            snprintf(header, sizeof(header), "[%u]", (unsigned int)_runtimeLogLineNumber);
        } else {
            _runtimeLogLineNumber++;
        }

        _activeLogFile.printf("%s %s\n", header, trimmedLine);
        _activeLogFile.flush();
        ok = true;
    } while (false);

    xSemaphoreGive(_logMutex);
    return ok;
}

void StorageMgr::FinishRuntimeLogSession()
{
    if (_logMutex == nullptr) return;
    if (xSemaphoreTake(_logMutex, pdMS_TO_TICKS(120)) != pdTRUE) return;

    if (_activeLogFile) {
        _activeLogFile.flush();
        _activeLogFile.close();
    }
    _runtimeLogSessionClosed = true;
    _activeLogPath = "";
    _activeLogIndex = 0;
    _runtimeLogLineNumber = 0;

    xSemaphoreGive(_logMutex);
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
