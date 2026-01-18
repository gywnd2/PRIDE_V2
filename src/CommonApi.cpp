#include <CommonApi.h>
#include <BluetoothMgr.h>
#include <ObdMgr.h>
#include <StorageMgr.h>

SystemAPI* SystemAPI::_instance = nullptr;

SystemAPI::SystemAPI()
{
    this->Init();
}

void SystemAPI::Init()
{
    this->gifObj.data = nullptr;
    this->gifObj.size = 0;
    this->isGifLoaded = false;

    this->displaySubscriber.ClearEvent();
    this->soundSubscriber.ClearEvent();
    this->btSubscriber.ClearEvent();
    this->storageSubscriber.ClearEvent();
}

// ----------------------------------------------------------------
// BtEventSubscriber 구현
// ----------------------------------------------------------------

void BtEventSubscriber::SetEvent(BT_EVENT_TYPE type, uint8_t address[6])
{
    this->eventType = type;
    this->eventPending = true;

    if (address != nullptr)
    {
        memcpy(this->reqAddress, address, 6);
    }
}

void BtEventSubscriber::ClearEvent()
{
    this->eventPending = false;
}

bool BtEventSubscriber::IsEventPending()
{
    return this->eventPending;
}

BT_EVENT_TYPE BtEventSubscriber::GetEventType()
{
    return this->eventType;
}

// ----------------------------------------------------------------
// SoundEventSubscriber 구현
// ----------------------------------------------------------------

void SoundEventSubscriber::SetEvent(SOUND_EVENT_TYPE type, int track)
{
    if (this->eventPending) {
        Serial.printf("[StorageEvent] Event already set to : %d\n", this->eventType);
        return;
    }

    this->eventType = type;
    this->trackNum = track;
    this->eventPending = true;
}

void SoundEventSubscriber::ClearEvent()
{
    this->eventPending = false;
}

bool SoundEventSubscriber::IsEventPending()
{
    return this->eventPending;
}

int SoundEventSubscriber::GetTrackNum()
{
    return this->trackNum;
}

// ----------------------------------------------------------------
// DisplayEventSubscriber 구현
// ----------------------------------------------------------------

void DisplayEventSubscriber::SetEvent(DISPLAY_EVENT_TYPE type, const String& data)
{
    this->eventType = type;
    this->eventData = data;
    this->eventPending = true;
}

void DisplayEventSubscriber::ClearEvent()
{
    this->eventPending = false;
}

bool DisplayEventSubscriber::IsEventPending()
{
    return this->eventPending;
}

String DisplayEventSubscriber::GetEventData()
{
    return this->eventData;
}

DISPLAY_EVENT_TYPE DisplayEventSubscriber::GetEventType()
{
    return this->eventType;
}

// ----------------------------------------------------------------
// StorageEventSubscriber 구현
// ----------------------------------------------------------------

void StorageEventSubscriber::SetEvent(STORAGE_EVENT_TYPE type, const String& path)
{
    if (this->eventPending)
    {
        Serial.printf("[StorageEvent] Event already set to : %d\n", this->eventType);
        return;
    }

    this->eventType = type;
    this->filePath = path;
    this->eventPending = true;
}

void StorageEventSubscriber::ClearEvent()
{
    this->eventPending = false;
}

bool StorageEventSubscriber::IsEventPending()
{
    return this->eventPending;
}

const String* StorageEventSubscriber::GetFilePath()
{
    return &(this->filePath);
}

STORAGE_EVENT_TYPE StorageEventSubscriber::GetEventType()
{
    return this->eventType;
}

// ----------------------------------------------------------------
// SystemAPI 구현
// ----------------------------------------------------------------

void SystemAPI::PlaySplash()
{
    if (displayMgr)
    {
        displaySubscriber.SetEvent(DISPLAY_SHOW_SPLASH, "/anim/splash.gif");
    }
}

void SystemAPI::ConnectOBD()
{
    if (obdMgr)
    {
        btSubscriber.SetEvent(BT_REQUEST_CONNECT_OBD);
    }
}

bool SystemAPI::GetOBDConnected()
{
    if (btMgr)
    {
        return btMgr->GetConnected();
    }
    return false;
}

Stream* SystemAPI::GetBtStream()
{
    if (btMgr)
    {
        // BluetoothMgr에서 정의한 NimBLEStream의 주소를 Stream 포인터로 반환
        return btMgr->GetBleStream();
    }
    return nullptr;
}

GIFMemory* SystemAPI::GetPsramObjPtr()
{
    return &this->gifObj;
}