#include <CommonApi.h>
#include <BluetoothMgr.h>
#include <ObdMgr.h>
#include <StorageMgr.h>
#include <DisplayMgr.h>

SystemAPI* SystemAPI::_instance = nullptr;

SystemAPI::SystemAPI()
{
    // Mutex 초기화는 Init에서 수행
}

void SystemAPI::Init()
{
    this->gifObj.data = nullptr;
    this->gifObj.size = 0;
    this->isGifLoaded = false;

    // GIF 메모리 보호용 뮤텍스
    if (_gifMutex == NULL) {
        _gifMutex = xSemaphoreCreateMutex();
    }

    // LVGL 스레드 동기화용 뮤텍스
    if (_lvglMutex == NULL) {
        _lvglMutex = xSemaphoreCreateMutex();
    }
}

// ----------------------------------------------------------------
// BtEventSubscriber Implementation
// ----------------------------------------------------------------

BtEventSubscriber::BtEventSubscriber() {
    _queue = xQueueCreate(10, sizeof(BtEventData));
}

BtEventSubscriber::~BtEventSubscriber() {
    if (_queue) vQueueDelete(_queue);
}

void BtEventSubscriber::SetEvent(BT_EVENT_TYPE type, uint8_t address[6]) {
    BtEventData data;
    data.type = type;
    if (address != nullptr) memcpy(data.address, address, 6);
    else memset(data.address, 0, 6);
    xQueueSend(_queue, &data, 0);
}

bool BtEventSubscriber::ReceiveEvent(BtEventData* event, TickType_t waitTime) {
    return xQueueReceive(_queue, event, waitTime) == pdTRUE;
}

// ----------------------------------------------------------------
// SoundEventSubscriber Implementation
// ----------------------------------------------------------------

SoundEventSubscriber::SoundEventSubscriber() {
    _queue = xQueueCreate(10, sizeof(SoundEventData));
}

SoundEventSubscriber::~SoundEventSubscriber() {
    if (_queue) vQueueDelete(_queue);
}

void SoundEventSubscriber::SetEvent(SOUND_EVENT_TYPE type, int track) {
    SoundEventData data;
    data.type = type;
    data.track = track;
    xQueueSend(_queue, &data, 0);
}

bool SoundEventSubscriber::ReceiveEvent(SoundEventData* event, TickType_t waitTime) {
    return xQueueReceive(_queue, event, waitTime) == pdTRUE;
}

// ----------------------------------------------------------------
// DisplayEventSubscriber Implementation
// ----------------------------------------------------------------

DisplayEventSubscriber::DisplayEventSubscriber() {
    _queue = xQueueCreate(10, sizeof(DisplayEventData));
}

DisplayEventSubscriber::~DisplayEventSubscriber() {
    if (_queue) vQueueDelete(_queue);
}

void DisplayEventSubscriber::SetEvent(DISPLAY_EVENT_TYPE type, const String& data) {
    DisplayEventData evt;
    evt.type = type;
    strncpy(evt.data, data.c_str(), sizeof(evt.data) - 1);
    evt.data[sizeof(evt.data) - 1] = '\0';
    xQueueSend(_queue, &evt, 0);
}

bool DisplayEventSubscriber::ReceiveEvent(DisplayEventData* event, TickType_t waitTime) {
    return xQueueReceive(_queue, event, waitTime) == pdTRUE;
}

// ----------------------------------------------------------------
// StorageEventSubscriber Implementation
// ----------------------------------------------------------------

StorageEventSubscriber::StorageEventSubscriber() {
    _queue = xQueueCreate(10, sizeof(StorageEventData));
}

StorageEventSubscriber::~StorageEventSubscriber() {
    if (_queue) vQueueDelete(_queue);
}

void StorageEventSubscriber::SetEvent(STORAGE_EVENT_TYPE type, const String& path) {
    StorageEventData evt;
    evt.type = type;
    strncpy(evt.filePath, path.c_str(), sizeof(evt.filePath) - 1);
    evt.filePath[sizeof(evt.filePath) - 1] = '\0';
    xQueueSend(_queue, &evt, 0);
}

bool StorageEventSubscriber::ReceiveEvent(StorageEventData* event, TickType_t waitTime) {
    return xQueueReceive(_queue, event, waitTime) == pdTRUE;
}

// ----------------------------------------------------------------
// SystemAPI Implementation
// ----------------------------------------------------------------

void SystemAPI::PlaySplash() {
    if (displayMgr) {
        displaySubscriber.SetEvent(DISPLAY_SHOW_SPLASH, "/anim/splash.gif");
    }
}

void SystemAPI::ConnectOBD() {
    if (obdMgr) {
        btSubscriber.SetEvent(BT_REQUEST_CONNECT_OBD);
    }
}

bool SystemAPI::GetOBDConnected() {
    if (btMgr) return btMgr->GetConnected();
    return false;
}

Stream* SystemAPI::GetBtStream() {
    if (btMgr) return btMgr->GetBleStream();
    return nullptr;
}

GIFMemory* SystemAPI::GetPsramObjPtr() {
    return &this->gifObj;
}

// Mutex Methods
bool SystemAPI::LockGif(TickType_t waitTime) {
    return (_gifMutex) ? (xSemaphoreTake(_gifMutex, waitTime) == pdTRUE) : false;
}

void SystemAPI::UnlockGif() {
    if (_gifMutex) xSemaphoreGive(_gifMutex);
}

bool SystemAPI::LockLvgl(TickType_t waitTime) {
    return (_lvglMutex) ? (xSemaphoreTake(_lvglMutex, waitTime) == pdTRUE) : false;
}

void SystemAPI::UnlockLvgl() {
    if (_lvglMutex) xSemaphoreGive(_lvglMutex);
}