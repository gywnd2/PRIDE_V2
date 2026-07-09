#include <StorageMgr.h>
#include <CommonApi.h>
#include <time.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>

#define TEST_LOG(fmt, ...) UartLogf("[StorageMgr] " fmt "\n", ##__VA_ARGS__)
#define TEST_LINE() UartLogf("[StorageMgr] %s\n", __func__)

static constexpr const char* STORAGE_LOG_DIR = "/log";

enum ServiceOdoCopyState
{
    SERVICE_ODO_COPY_MISSING = 0,
    SERVICE_ODO_COPY_VALID,
    SERVICE_ODO_COPY_CORRUPT,
    SERVICE_ODO_COPY_IO_ERROR
};

static const char* service_odo_copy_state_name(ServiceOdoCopyState state)
{
    switch (state) {
        case SERVICE_ODO_COPY_MISSING: return "missing";
        case SERVICE_ODO_COPY_VALID: return "valid";
        case SERVICE_ODO_COPY_CORRUPT: return "corrupt";
        case SERVICE_ODO_COPY_IO_ERROR: return "io_error";
        default: return "unknown";
    }
}

static ServiceOdoState make_default_service_odo_state()
{
    ServiceOdoState state = {};
    return state;
}

static bool parse_uint32_text(const char* text, uint32_t* outValue)
{
    if (!text || !outValue) return false;

    const unsigned char* p = (const unsigned char*)text;
    while (*p != '\0' && isspace(*p)) p++;
    if (!isdigit(*p)) return false;

    uint32_t value = 0;
    while (isdigit(*p)) {
        uint32_t digit = (uint32_t)(*p - '0');
        if (value > (UINT32_MAX - digit) / 10U) return false;
        value = (value * 10U) + digit;
        p++;
    }

    while (*p != '\0') {
        if (!isspace(*p)) return false;
        p++;
    }

    *outValue = value;
    return true;
}

static char* trim_in_place(char* text)
{
    if (!text) return text;

    while (*text != '\0' && isspace((unsigned char)*text)) text++;

    char* end = text + strlen(text);
    while (end > text && isspace((unsigned char)*(end - 1))) {
        end--;
    }
    *end = '\0';
    return text;
}

static bool parse_service_odo_state_text(char* text, ServiceOdoState* outState)
{
    if (!text || !outState) return false;

    ServiceOdoState state = make_default_service_odo_state();

    if (strchr(text, '=') == nullptr) {
        uint32_t legacyServiceKm = 0;
        if (!parse_uint32_text(text, &legacyServiceKm)) return false;
        state.last_service_km = legacyServiceKm;
        *outState = state;
        return true;
    }

    bool versionFound = false;
    bool baseValidFound = false;
    bool baseOdoFound = false;

    char* cursor = text;
    while (cursor && *cursor != '\0') {
        char* line = cursor;
        char* next = strpbrk(cursor, "\r\n");
        if (next) {
            *next = '\0';
            cursor = next + 1;
            while (*cursor == '\r' || *cursor == '\n') cursor++;
        } else {
            cursor = nullptr;
        }

        line = trim_in_place(line);
        if (line[0] == '\0' || line[0] == '#') continue;

        char* eq = strchr(line, '=');
        if (!eq) return false;
        *eq = '\0';

        char* key = trim_in_place(line);
        char* valueText = trim_in_place(eq + 1);
        uint32_t value = 0;
        if (!parse_uint32_text(valueText, &value)) return false;

        if (strcmp(key, "version") == 0) {
            if (value != 2U) return false;
            versionFound = true;
        } else if (strcmp(key, "revision") == 0) {
            state.revision = value;
        } else if (strcmp(key, "base_odo_valid") == 0) {
            if (value > 1U) return false;
            state.base_odo_valid = (value != 0U);
            baseValidFound = true;
        } else if (strcmp(key, "base_odo_km") == 0) {
            state.base_odo_km = value;
            baseOdoFound = true;
        } else if (strcmp(key, "last_seen_odo_km") == 0) {
            state.last_seen_odo_km = value;
        } else if (strcmp(key, "last_service_km") == 0) {
            state.last_service_km = value;
        }
    }

    if (!versionFound || !baseValidFound) return false;
    if (state.base_odo_valid && !baseOdoFound) return false;

    *outState = state;
    return true;
}

static bool service_odo_states_equal(const ServiceOdoState& a, const ServiceOdoState& b)
{
    return a.revision == b.revision &&
           a.base_odo_valid == b.base_odo_valid &&
           a.base_odo_km == b.base_odo_km &&
           a.last_seen_odo_km == b.last_seen_odo_km &&
           a.last_service_km == b.last_service_km;
}

static size_t format_service_odo_state_text(const ServiceOdoState& state, char* out, size_t outLen)
{
    if (!out || outLen == 0) return 0;

    int written = snprintf(
        out,
        outLen,
        "version=2\n"
        "revision=%lu\n"
        "base_odo_valid=%u\n"
        "base_odo_km=%lu\n"
        "last_seen_odo_km=%lu\n"
        "last_service_km=%lu\n",
        (unsigned long)state.revision,
        state.base_odo_valid ? 1U : 0U,
        (unsigned long)state.base_odo_km,
        (unsigned long)state.last_seen_odo_km,
        (unsigned long)state.last_service_km
    );
    if (written <= 0 || (size_t)written >= outLen) return 0;
    return (size_t)written;
}

static ServiceOdoCopyState read_service_odo_copy(const char* path, ServiceOdoState* outState)
{
    if (outState) *outState = make_default_service_odo_state();
    if (!path || !outState) return SERVICE_ODO_COPY_IO_ERROR;
    if (!SD.exists(path)) return SERVICE_ODO_COPY_MISSING;

    File f = SD.open(path, FILE_READ);
    if (!f) return SERVICE_ODO_COPY_IO_ERROR;
    if (f.isDirectory()) {
        f.close();
        return SERVICE_ODO_COPY_IO_ERROR;
    }

    char buf[192] = {0};
    size_t len = f.readBytes(buf, sizeof(buf) - 1);
    bool truncated = f.available() > 0;
    f.close();
    buf[len] = '\0';

    if (truncated) return SERVICE_ODO_COPY_CORRUPT;
    return parse_service_odo_state_text(buf, outState) ? SERVICE_ODO_COPY_VALID : SERVICE_ODO_COPY_CORRUPT;
}

static bool write_service_odo_copy(const char* path, const ServiceOdoState& state)
{
    if (!path) return false;
    if (SD.exists(path) && !SD.remove(path)) return false;

    char text[192] = {0};
    size_t len = format_service_odo_state_text(state, text, sizeof(text));
    if (len == 0) return false;

    File writeFile = SD.open(path, FILE_WRITE);
    if (!writeFile) return false;
    size_t written = writeFile.print(text);
    writeFile.flush();
    writeFile.close();
    return written == len;
}

static bool write_service_odo_mirror_locked(const ServiceOdoState& state)
{
    if (!write_service_odo_copy(SERVICE_ODO_BACKUP_FILE_PATH, state)) return false;
    if (!write_service_odo_copy(SERVICE_ODO_FILE_PATH, state)) return false;
    return true;
}

static bool read_service_odo_mirror_locked(ServiceOdoState* outState, bool* repairNeeded)
{
    if (!outState) return false;
    *outState = make_default_service_odo_state();
    if (repairNeeded) *repairNeeded = false;

    ServiceOdoState primary = make_default_service_odo_state();
    ServiceOdoState backup = make_default_service_odo_state();
    ServiceOdoCopyState primaryState = read_service_odo_copy(SERVICE_ODO_FILE_PATH, &primary);
    ServiceOdoCopyState backupState = read_service_odo_copy(SERVICE_ODO_BACKUP_FILE_PATH, &backup);

    if (primaryState == SERVICE_ODO_COPY_VALID && backupState == SERVICE_ODO_COPY_VALID) {
        *outState = (backup.revision > primary.revision) ? backup : primary;
        if (repairNeeded) *repairNeeded = !service_odo_states_equal(primary, backup);
        if (!service_odo_states_equal(primary, backup)) {
            TEST_LOG("service odo mirror mismatch primary_rev=%u backup_rev=%u chosen_rev=%u",
                     (unsigned int)primary.revision,
                     (unsigned int)backup.revision,
                     (unsigned int)outState->revision);
        }
        return true;
    }

    if (primaryState == SERVICE_ODO_COPY_VALID) {
        *outState = primary;
        if (repairNeeded) *repairNeeded = true;
        TEST_LOG("service odo primary recovered rev=%u backup_state=%s",
                 (unsigned int)primary.revision,
                 service_odo_copy_state_name(backupState));
        return true;
    }

    if (backupState == SERVICE_ODO_COPY_VALID) {
        *outState = backup;
        if (repairNeeded) *repairNeeded = true;
        TEST_LOG("service odo backup recovered rev=%u primary_state=%s",
                 (unsigned int)backup.revision,
                 service_odo_copy_state_name(primaryState));
        return true;
    }

    if (primaryState == SERVICE_ODO_COPY_MISSING && backupState == SERVICE_ODO_COPY_MISSING) {
        if (repairNeeded) *repairNeeded = true;
        return true;
    }

    if (primaryState == SERVICE_ODO_COPY_IO_ERROR || backupState == SERVICE_ODO_COPY_IO_ERROR) {
        TEST_LOG("service odo mirror read failed primary_state=%s backup_state=%s",
                 service_odo_copy_state_name(primaryState),
                 service_odo_copy_state_name(backupState));
        return false;
    }

    if (repairNeeded) *repairNeeded = true;
    TEST_LOG("service odo mirror corrupt primary_state=%s backup_state=%s",
             service_odo_copy_state_name(primaryState),
             service_odo_copy_state_name(backupState));
    return false;
}

#ifndef FILE_APPEND
#define FILE_APPEND "a"
#endif

static bool get_synced_local_time(struct tm* out)
{
    if (!out) return false;
    time_t now = time(nullptr);
    if (now <= 0) return false;

    struct tm tm_local = {};
    if (localtime_r(&now, &tm_local) == nullptr) return false;

    // Consider clock valid only after NTP sync (year >= 2024).
    int year = tm_local.tm_year + 1900;
    if (year < 2024) return false;

    *out = tm_local;
    return true;
}

static bool format_runtime_time_header(char* out, size_t out_len)
{
    if (!out || out_len == 0) return false;

    struct tm tm_local = {};
    if (!get_synced_local_time(&tm_local)) return false;

    snprintf(out, out_len, "[%02d-%02d-%02d]",
             tm_local.tm_hour,
             tm_local.tm_min,
             tm_local.tm_sec);
    return true;
}

static bool format_runtime_date_path(char* out, size_t out_len)
{
    if (!out || out_len == 0) return false;

    struct tm tm_local = {};
    if (!get_synced_local_time(&tm_local)) return false;

    int year = (tm_local.tm_year + 1900) % 100;
    snprintf(out, out_len, "%s/runtime_%02d%02d%02d.log",
             STORAGE_LOG_DIR,
             year,
             tm_local.tm_mon + 1,
             tm_local.tm_mday);
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
        uint32_t oilCycleKm = SERVICE_OIL_CYCLE_DEFAULT_KM;
        this->ReadServiceOilCycleKm(&oilCycleKm);
        if (this->ReadServiceOdoKm(&serviceOdoKm)) {
            system->PublishServiceOdoKm(serviceOdoKm);
            TEST_LOG("service odo=%u km cycle=%u due=%d",
                     (unsigned int)serviceOdoKm,
                     (unsigned int)oilCycleKm,
                     serviceOdoKm >= oilCycleKm ? 1 : 0);
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

bool StorageMgr::ReadServiceOdoState(ServiceOdoState* outState)
{
    if (!outState) return false;
    *outState = make_default_service_odo_state();
    if (_logMutex == nullptr) return false;
    if (xSemaphoreTake(_logMutex, pdMS_TO_TICKS(120)) != pdTRUE) return false;

    bool ok = false;
    do {
        if (!EnsureDbDir()) break;

        bool repairNeeded = false;
        if (!read_service_odo_mirror_locked(outState, &repairNeeded)) break;
        if (repairNeeded) {
            if (write_service_odo_mirror_locked(*outState)) {
                TEST_LOG("service odo mirror repaired rev=%u", (unsigned int)outState->revision);
            } else {
                TEST_LOG("service odo mirror repair failed rev=%u", (unsigned int)outState->revision);
            }
        }
        ok = true;
    } while (false);

    xSemaphoreGive(_logMutex);
    return ok;
}

bool StorageMgr::WriteServiceOdoState(const ServiceOdoState& state)
{
    if (_logMutex == nullptr) return false;
    if (xSemaphoreTake(_logMutex, pdMS_TO_TICKS(180)) != pdTRUE) return false;

    bool ok = false;
    do {
        if (!EnsureDbDir()) break;
        if (!write_service_odo_mirror_locked(state)) break;
        TEST_LOG("service odo state updated rev=%u acc_valid=%d source_base=%u last_seen=%u service=%u",
                 (unsigned int)state.revision,
                 state.base_odo_valid ? 1 : 0,
                 (unsigned int)state.base_odo_km,
                 (unsigned int)state.last_seen_odo_km,
                 (unsigned int)state.last_service_km);
        ok = true;
    } while (false);

    xSemaphoreGive(_logMutex);
    return ok;
}

bool StorageMgr::ReadServiceOdoKm(uint32_t* outKm)
{
    if (!outKm) return false;
    *outKm = 0;

    ServiceOdoState state = {};
    if (!ReadServiceOdoState(&state)) return false;
    *outKm = state.last_service_km;
    return true;
}

bool StorageMgr::ReadServiceOilCycleKm(uint32_t* outKm)
{
    if (!outKm) return false;
    *outKm = SERVICE_OIL_CYCLE_DEFAULT_KM;
    if (_logMutex == nullptr) return false;
    if (xSemaphoreTake(_logMutex, pdMS_TO_TICKS(120)) != pdTRUE) return false;

    bool ok = false;
    do {
        if (!EnsureDbDir()) break;
        if (!SD.exists(SERVICE_OIL_CYCLE_FILE_PATH)) {
            ok = true;
            break;
        }

        File f = SD.open(SERVICE_OIL_CYCLE_FILE_PATH, FILE_READ);
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

        *outKm = (value == 0UL) ? SERVICE_OIL_CYCLE_DEFAULT_KM : (uint32_t)value;
        ok = true;
    } while (false);

    xSemaphoreGive(_logMutex);
    return ok;
}

bool StorageMgr::WriteServiceOilCycleKm(uint32_t cycleKm, uint32_t* writtenOut)
{
    if (writtenOut) *writtenOut = 0;
    if (cycleKm == 0U) cycleKm = SERVICE_OIL_CYCLE_DEFAULT_KM;
    if (_logMutex == nullptr) return false;
    if (xSemaphoreTake(_logMutex, pdMS_TO_TICKS(180)) != pdTRUE) return false;

    bool ok = false;
    do {
        if (!EnsureDbDir()) break;

        if (SD.exists(SERVICE_OIL_CYCLE_FILE_PATH) && !SD.remove(SERVICE_OIL_CYCLE_FILE_PATH)) {
            break;
        }

        File writeFile = SD.open(SERVICE_OIL_CYCLE_FILE_PATH, FILE_WRITE);
        if (!writeFile) break;
        writeFile.print(cycleKm);
        writeFile.close();

        if (writtenOut) *writtenOut = cycleKm;
        TEST_LOG("service oil cycle updated value=%u", (unsigned int)cycleKm);
        ok = true;
    } while (false);

    xSemaphoreGive(_logMutex);
    return ok;
}

bool StorageMgr::ResetServiceOilCycleKm(uint32_t* writtenOut)
{
    return WriteServiceOilCycleKm(SERVICE_OIL_CYCLE_DEFAULT_KM, writtenOut);
}

bool StorageMgr::PrepareNextRuntimeLogFileLocked()
{
    if (_runtimeLogSessionClosed) return false;
    char rtcPath[48] = {0};
    bool rtcPathValid = format_runtime_date_path(rtcPath, sizeof(rtcPath));

    if (_activeLogPath.length() > 0 && _activeLogFile) {
        if (!_activeLogUsesRtcName && rtcPathValid) {
            _activeLogFile.flush();
            _activeLogFile.close();
            _activeLogPath = String(rtcPath);
            _activeLogUsesRtcName = true;
            _activeLogFile = SD.open(_activeLogPath.c_str(), FILE_APPEND);
            if (!_activeLogFile) {
                TEST_LOG("failed to switch display runtime log file: %s", _activeLogPath.c_str());
                _activeLogPath = "";
                _activeLogUsesRtcName = false;
                return false;
            }
            TEST_LOG("display runtime log file switched=%s", _activeLogPath.c_str());
        }
        return true;
    }
    if (_activeLogPath.length() > 0 && !_activeLogFile) _activeLogPath = "";
    if (!EnsureLogDir()) return false;

    _runtimeLogLineNumber = 0;
    if (rtcPathValid) {
        _activeLogPath = String(rtcPath);
        _activeLogUsesRtcName = true;
    } else {
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
        _activeLogPath = String(STORAGE_LOG_DIR) + "/runtime_" + String(_activeLogIndex) + ".log";
        _activeLogUsesRtcName = false;
    }

    _activeLogFile = SD.open(_activeLogPath.c_str(), FILE_APPEND);
    if (!_activeLogFile) {
        TEST_LOG("failed to open display runtime log file: %s", _activeLogPath.c_str());
        _activeLogPath = "";
        _activeLogUsesRtcName = false;
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
    _activeLogUsesRtcName = false;

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
