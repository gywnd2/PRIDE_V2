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

    if (_gifMutex == NULL) {
        _gifMutex = xSemaphoreCreateMutex();
    }

    // 큐는 생성자에서 초기화되므로 별도 Clear 불필요
}

// ----------------------------------------------------------------
// BtEventSubscriber 구현
// ----------------------------------------------------------------

BtEventSubscriber::BtEventSubscriber() {
    Serial.println("====BtEventSubscriber");
    _queue = xQueueCreate(10, sizeof(BtEventData)); // 큐 크기 10
}

BtEventSubscriber::~BtEventSubscriber() {
    Serial.println("~~~~BtEventSubscriber");
    if (_queue) vQueueDelete(_queue);
}

void BtEventSubscriber::SetEvent(BT_EVENT_TYPE type, uint8_t address[6])
{
    BtEventData data;
    data.type = type;
    if (address != nullptr) {
        memcpy(data.address, address, 6);
    } else {
        memset(data.address, 0, 6);
    }

    xQueueSend(_queue, &data, 0);
}

bool BtEventSubscriber::ReceiveEvent(BtEventData* event, TickType_t waitTime) {
    return xQueueReceive(_queue, event, waitTime) == pdTRUE;
}

// ----------------------------------------------------------------
// SoundEventSubscriber 구현
// ----------------------------------------------------------------

SoundEventSubscriber::SoundEventSubscriber() {
    Serial.println("====SoundEventSubscriber");
    _queue = xQueueCreate(10, sizeof(SoundEventData));
}

SoundEventSubscriber::~SoundEventSubscriber() {
    Serial.println("~~~~SoundEventSubscriber");
    if (_queue) vQueueDelete(_queue);
}

void SoundEventSubscriber::SetEvent(SOUND_EVENT_TYPE type, int track)
{
    SoundEventData data;
    data.type = type;
    data.track = track;
    xQueueSend(_queue, &data, 0);
}

bool SoundEventSubscriber::ReceiveEvent(SoundEventData* event, TickType_t waitTime) {
    return xQueueReceive(_queue, event, waitTime) == pdTRUE;
}

// ----------------------------------------------------------------
// DisplayEventSubscriber 구현
// ----------------------------------------------------------------

DisplayEventSubscriber::DisplayEventSubscriber() {
    Serial.println("====DisplayEventSubscriber");
    _queue = xQueueCreate(10, sizeof(DisplayEventData));
}

DisplayEventSubscriber::~DisplayEventSubscriber() {
    Serial.println("~~~~DisplayEventSubscriber");
    if (_queue) vQueueDelete(_queue);
}

void DisplayEventSubscriber::SetEvent(DISPLAY_EVENT_TYPE type, const String& data)
{
    DisplayEventData evt;
    evt.type = type;
    // String 데이터를 고정 길이 char 배열로 복사 (Overflow 방지)
    strncpy(evt.data, data.c_str(), sizeof(evt.data) - 1);
    evt.data[sizeof(evt.data) - 1] = '\0';

    xQueueSend(_queue, &evt, 0);
}

bool DisplayEventSubscriber::ReceiveEvent(DisplayEventData* event, TickType_t waitTime) {
    return xQueueReceive(_queue, event, waitTime) == pdTRUE;
}

// ----------------------------------------------------------------
// StorageEventSubscriber 구현
// ----------------------------------------------------------------

StorageEventSubscriber::StorageEventSubscriber() {
    Serial.println("====StorageEventSubscriber");
    _queue = xQueueCreate(10, sizeof(StorageEventData));
}

StorageEventSubscriber::~StorageEventSubscriber() {
    Serial.println("~~~~StorageEventSubscriber");
    if (_queue) vQueueDelete(_queue);
}

void StorageEventSubscriber::SetEvent(STORAGE_EVENT_TYPE type, const String& path)
{
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

bool SystemAPI::LockGif(TickType_t waitTime)
{
    if (_gifMutex) {
        return xSemaphoreTake(_gifMutex, waitTime) == pdTRUE;
    }
    return false;
}

void SystemAPI::UnlockGif()
{
    if (_gifMutex) {
        xSemaphoreGive(_gifMutex);
    }
}