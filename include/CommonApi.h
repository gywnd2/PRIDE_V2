#ifndef __PRIDE_COMMON__
#define __PRIDE_COMMON__

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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

typedef enum
{
    SOUND_EVENT_NONE = 0,
    SOUND_PLAY_TRACK
} SOUND_EVENT_TYPE;

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

typedef enum
{
    STORAGE_EVENT_NONE = 0,
    STORAGE_SCAN,
    STORAGE_READ,
    STORAGE_WRITE,
    STORAGE_LOAD_TO_PSRAM,
    STORAGE_CLEAR_LOADED_PSRAM
} STORAGE_EVENT_TYPE;

// ----------------------------------------------------------------
// Event Subscriber Classes
// ----------------------------------------------------------------

class BtEventSubscriber
{
private:
    BT_EVENT_TYPE eventType;
    bool eventPending = false;
    uint8_t reqAddress[6];

public:
    BtEventSubscriber()
    {
        Serial.println("====BtEventSubscriber");
    }
    ~BtEventSubscriber()
    {
        Serial.println("~~~~BtEventSubscriber");
    }

    void SetEvent(BT_EVENT_TYPE type, uint8_t address[6] = nullptr);
    void ClearEvent();
    bool IsEventPending();
    BT_EVENT_TYPE GetEventType();
};

class SoundEventSubscriber
{
private:
    SOUND_EVENT_TYPE eventType;
    bool eventPending = false;
    int trackNum = 0;

public:
    SoundEventSubscriber()
    {
        Serial.println("====SoundEventSubscriber");
    }
    ~SoundEventSubscriber()
    {
        Serial.println("~~~~SoundEventSubscriber");
    }
    void SetEvent(SOUND_EVENT_TYPE type, int track = 0);
    void ClearEvent();
    bool IsEventPending();
    int GetTrackNum();
};

class DisplayEventSubscriber
{
private:
    DISPLAY_EVENT_TYPE eventType;
    bool eventPending = false;
    String eventData;

public:
    DisplayEventSubscriber()
    {
        Serial.println("====DisplayEventSubscriber");
    }
    ~DisplayEventSubscriber()
    {
        Serial.println("~~~~DisplayEventSubscriber");
    }
    void SetEvent(DISPLAY_EVENT_TYPE type, const String& data = "");
    void ClearEvent();
    bool IsEventPending();
    String GetEventData();
    DISPLAY_EVENT_TYPE GetEventType();
};

class StorageEventSubscriber
{
private:
    STORAGE_EVENT_TYPE eventType;
    bool eventPending = false;
    String filePath;
    bool gifLoadToPSRAM = false;

public:
    StorageEventSubscriber()
    {
        Serial.println("====StorageEventSubscriber");
    }
    ~StorageEventSubscriber()
    {
        Serial.println("~~~~StorageEventSubscriber");
    }
    void SetEvent(STORAGE_EVENT_TYPE type, const String& path = "");
    void ClearEvent();
    bool IsEventPending();
    const String* GetFilePath();
    STORAGE_EVENT_TYPE GetEventType();
};

// ----------------------------------------------------------------
// SystemAPI Class (Singleton)
// ----------------------------------------------------------------

class SystemAPI
{
private:
    SystemAPI();
    static SystemAPI* _instance;

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
};

#endif