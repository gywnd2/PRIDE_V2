#ifndef __STORAGE__
#define __STORAGE__

#include <Arduino.h>
#include <SD.h>
#include <vector>
#include <SPI.h>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static constexpr const char* SERVICE_ODO_DIR = "/db";
static constexpr const char* SERVICE_ODO_FILE_PATH = "/db/odo.txt";
static constexpr const char* SERVICE_ODO_BACKUP_FILE_PATH = "/db/odo.bak";
static constexpr const char* SERVICE_OIL_CYCLE_FILE_PATH = "/db/oil_cycle.txt";
static constexpr uint32_t SERVICE_OIL_CYCLE_DEFAULT_KM = 7000U;
static constexpr uint32_t SERVICE_ODO_THRESHOLD_KM = SERVICE_OIL_CYCLE_DEFAULT_KM;

struct ServiceOdoState
{
    uint32_t revision = 0;
    // Retained for v2 file compatibility; service life now uses last_service_km.
    bool base_odo_valid = false;
    uint32_t base_odo_km = 0;
    // Last raw source counter and accumulated distance since oil replacement.
    uint32_t last_seen_odo_km = 0;
    uint32_t last_service_km = 0;
};

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
        File _activeLogFile;
        String _activeLogPath;
        uint32_t _activeLogIndex = 0;
        uint32_t _runtimeLogLineNumber = 0;
        bool _activeLogUsesRtcName = false;
        bool _runtimeLogSessionClosed = false;

        bool EnsureLogDir();
        bool EnsureDbDir();
        bool PrepareNextRuntimeLogFileLocked();

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
        bool ReadServiceOdoState(ServiceOdoState* outState);
        bool WriteServiceOdoState(const ServiceOdoState& state);
        bool ReadServiceOdoKm(uint32_t* outKm);
        bool ReadServiceOilCycleKm(uint32_t* outKm);
        bool WriteServiceOilCycleKm(uint32_t cycleKm, uint32_t* writtenOut = nullptr);
        bool ResetServiceOilCycleKm(uint32_t* writtenOut = nullptr);
        bool AppendDisplayRuntimeLogLine(const char* line);
        void FinishRuntimeLogSession();
        String GetActiveLogPath() const { return _activeLogPath; }

        GIFMemory LoadGifToPSRAM(const char* path);
        void FreeGifFromPSRAM(GIFMemory& mem);
};

#endif
