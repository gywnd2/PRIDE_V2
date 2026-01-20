#ifndef __PRIDE_COMMON__
#define __PRIDE_COMMON__

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "StorageMgr.h"

// Forward Declarations
class DisplayMgr;
class ObdMgr;
class Mp3Mgr;
class BluetoothMgr;
class StorageMgr;

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
    STORAGE_CLEAR_LOADED_PSRAM
} STORAGE_EVENT_TYPE;

typedef struct {
    STORAGE_EVENT_TYPE type;
    char filePath[256];
} StorageEventData;

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
    SemaphoreHandle_t _gifMutex = nullptr;

    DisplayMgr* displayMgr = nullptr;
    StorageMgr* storageMgr = nullptr;
    ObdMgr* obdMgr = nullptr;
    Mp3Mgr* mp3Mgr = nullptr;
    BluetoothMgr* btMgr = nullptr;

    GIFMemory gifObj;

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

    DisplayEventSubscriber displaySubscriber;
    SoundEventSubscriber   soundSubscriber;
    BtEventSubscriber      btSubscriber;
    StorageEventSubscriber storageSubscriber;

    bool isGifLoaded = false;

    void registerDisplay(DisplayMgr* mgr) { displayMgr = mgr; }
    void registerStorage(StorageMgr* mgr) { storageMgr = mgr; }
    void registerMp3(Mp3Mgr* mgr) { mp3Mgr = mgr; }
    void registerBt(BluetoothMgr* mgr) { btMgr = mgr; }
    void registerObd(ObdMgr* mgr) { obdMgr = mgr; }

    void PlaySplash();
    void ConnectOBD();
    bool GetOBDConnected();

    Stream* GetBtStream();
    GIFMemory* GetPsramObjPtr();
    
    bool LockGif(TickType_t waitTime = portMAX_DELAY);
    void UnlockGif();
};

#endif