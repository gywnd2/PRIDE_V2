#ifndef __PRIDE_COMMON__
#define __PRIDE_COMMON__

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include "StorageMgr.h"

// Forward Declarations
class DisplayMgr;
class ObdMgr;
class Mp3Mgr;
class BluetoothMgr;
class StorageMgr;
class WifiMgr;

// ----------------------------------------------------------------
// Event Types & Data Structures
// ----------------------------------------------------------------

typedef enum
{
    BT_EVENT_NONE = 0,
    BT_REQUEST_CONNECT_OBD,
    BT_REQUEST_CONNECT,
    BT_REQUEST_DISCONNECT,
    BT_REQUEST_RESET_CONNECTION
} BT_EVENT_TYPE;

typedef struct {
    BT_EVENT_TYPE type;
    uint8_t address[6];
} BtEventData;

typedef enum
{
    SOUND_EVENT_NONE = 0,
    SOUND_PLAY_TRACK
} SOUND_EVENT_TYPE;

typedef struct {
    SOUND_EVENT_TYPE type;
    int track;
} SoundEventData;

typedef enum
{
    DIPLAY_EVENT_NONE = 0,
    DISPLAY_SHOW_SPLASH,
    DISPLAY_SHOW_GOODBYE,
    DISPLAY_SHOW_GAUGE_REBOOT,
    DISPLAY_UPDATE_OBD_STATUS,
    DISPLAY_UPDATE_VOLTAGE,
    DISPLAY_UPDATE_COOLANT,
    DISPLAY_UPDATE_CPU_USAGE,
    DISPLAY_UPDATE_RAM_USAGE
} DISPLAY_EVENT_TYPE;

typedef struct {
    DISPLAY_EVENT_TYPE type;
    char data[256];
} DisplayEventData;

typedef enum
{
    STORAGE_EVENT_NONE = 0,
    STORAGE_SCAN,
    STORAGE_READ,
    STORAGE_WRITE,
    STORAGE_LOAD_TO_PSRAM,
    STORAGE_CLEAR_LOADED_PSRAM,
    STORAGE_APPEND_LOG
} STORAGE_EVENT_TYPE;

typedef struct {
    STORAGE_EVENT_TYPE type;
    char filePath[256];
} StorageEventData;

typedef struct {
    bool wifiConnected;
    int32_t wifiRssi;
    bool clockValid;
    char clockText[6];   // "HH:MM"
    bool weatherValid;
    char cityText[64];
    char weatherText[32];

    bool btConnected;
    int obdStatus;

    bool coolantValid;
    uint16_t coolant;

    bool batteryValid;
    uint16_t batteryVoltage;

    bool outsideTempValid;
    int16_t outsideTempC;
} UiSharedState;

// ----------------------------------------------------------------
// Event Subscriber Classes
// ----------------------------------------------------------------

class BtEventSubscriber
{
private:
    QueueHandle_t _queue;
public:
    BtEventSubscriber();
    ~BtEventSubscriber();
    void SetEvent(BT_EVENT_TYPE type, uint8_t address[6] = nullptr);
    bool ReceiveEvent(BtEventData* event, TickType_t waitTime);
};

class SoundEventSubscriber
{
private:
    QueueHandle_t _queue;
public:
    SoundEventSubscriber();
    ~SoundEventSubscriber();
    void SetEvent(SOUND_EVENT_TYPE type, int track = 0);
    bool ReceiveEvent(SoundEventData* event, TickType_t waitTime);
};

class DisplayEventSubscriber
{
private:
    QueueHandle_t _queue;
public:
    DisplayEventSubscriber();
    ~DisplayEventSubscriber();
    void SetEvent(DISPLAY_EVENT_TYPE type, const String& data = "");
    bool ReceiveEvent(DisplayEventData* event, TickType_t waitTime);
};

class StorageEventSubscriber
{
private:
    QueueHandle_t _queue;
public:
    StorageEventSubscriber();
    ~StorageEventSubscriber();
    void SetEvent(STORAGE_EVENT_TYPE type, const String& path = "");
    bool ReceiveEvent(StorageEventData* event, TickType_t waitTime);
};

// ----------------------------------------------------------------
// SystemAPI Class (Singleton)
// ----------------------------------------------------------------

class SystemAPI
{
private:
    SystemAPI();
    static SystemAPI* _instance;

    // Mutex Handles
    SemaphoreHandle_t _gifMutex = nullptr;
    SemaphoreHandle_t _lvglMutex = nullptr;
    SemaphoreHandle_t _uiStateMutex = nullptr;

    DisplayMgr* displayMgr = nullptr;
    StorageMgr* storageMgr = nullptr;
    ObdMgr* obdMgr = nullptr;
    Mp3Mgr* mp3Mgr = nullptr;
    BluetoothMgr* btMgr = nullptr;
    WifiMgr* wifiMgr = nullptr;
    GIFMemory gifObj;
    UiSharedState uiState;

public:
    static SystemAPI* getInstance()
    {
        if (_instance == nullptr)
        {
            _instance = new SystemAPI();
        }
        return _instance;
    }

    void Init();

    BtEventSubscriber      btSubscriber;
    SoundEventSubscriber   soundSubscriber;
    DisplayEventSubscriber displaySubscriber;
    StorageEventSubscriber storageSubscriber;
    bool isGifLoaded = false;

    // Manager Registration
    void registerDisplay(DisplayMgr* mgr) { displayMgr = mgr; }
    void registerStorage(StorageMgr* mgr) { storageMgr = mgr; }
    void registerMp3(Mp3Mgr* mgr) { mp3Mgr = mgr; }
    void registerBt(BluetoothMgr* mgr) { btMgr = mgr; }
    void registerObd(ObdMgr* mgr) { obdMgr = mgr; }
    void registerWifi(WifiMgr* mgr) { wifiMgr = mgr; }

    // API Methods
    void PlaySplash();
    void ConnectOBD();
    bool GetOBDConnected();
    // Logic-state getters (source: manager runtime state)
    bool IsObdCommConnected();
    bool IsObdDisconnected();
    bool IsDisplayReady();
    Stream* GetBtStream();
    GIFMemory* GetPsramObjPtr();

    // Shared UI state publish/snapshot
    void PublishWifiState(bool connected, int32_t rssi);
    void PublishClockText(const char* hhmm);
    void PublishWeatherText(const char* city, const char* weather);
    void PublishBtConnected(bool connected);
    void PublishObdStatus(int status);
    void PublishObdCoolant(uint16_t coolant);
    void PublishObdBatteryVoltage(uint16_t voltage);
    void PublishObdOutsideTemp(int16_t tempC, bool valid = true);
    void AppendStorageLog(const char* line);
    bool GetUiSharedSnapshot(UiSharedState* out, TickType_t waitTime = 0);
    bool GetUiWifiState(bool* connected, int32_t* rssi = nullptr, TickType_t waitTime = 0);
    bool GetUiClockText(char* out, size_t outLen, TickType_t waitTime = 0);
    bool GetUiWeather(char* outCity, size_t cityLen, char* outWeather, size_t weatherLen, bool* valid = nullptr, TickType_t waitTime = 0);
    bool GetUiBtConnected(bool* connected, TickType_t waitTime = 0);
    bool GetUiObdStatus(int* status, TickType_t waitTime = 0);
    bool GetUiCoolant(uint16_t* coolant, bool* valid = nullptr, TickType_t waitTime = 0);
    bool GetUiBatteryVoltage(uint16_t* voltage, bool* valid = nullptr, TickType_t waitTime = 0);
    bool GetUiOutsideTemp(int16_t* tempC, bool* valid = nullptr, TickType_t waitTime = 0);

    // Resource Locking (Thread Safety)
    bool LockGif(TickType_t waitTime = portMAX_DELAY);
    void UnlockGif();
    bool LockLvgl(TickType_t waitTime = portMAX_DELAY);
    void UnlockLvgl();
    bool LockUiState(TickType_t waitTime = portMAX_DELAY);
    void UnlockUiState();
};

#endif
