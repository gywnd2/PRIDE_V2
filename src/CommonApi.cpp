#include <CommonApi.h>
#include <BluetoothMgr.h>
#include <ObdMgr.h>
#include <StorageMgr.h>
#include <DisplayMgr.h>
#include <string.h>

#define TEST_LOG(fmt, ...) Serial.printf("[SystemAPI] " fmt "\n", ##__VA_ARGS__)
#define TEST_LINE() Serial.printf("[SystemAPI] %s\n", __func__)

SystemAPI* SystemAPI::_instance = nullptr;

SystemAPI::SystemAPI()
{
    // Mutex initialization is done in Init()
}

void SystemAPI::Init()
{
    TEST_LINE();
    gifObj.data = nullptr;
    gifObj.size = 0;
    isGifLoaded = false;
    memset(&uiState, 0, sizeof(uiState));
    uiState.wifiRssi = -100;

    if (_gifMutex == NULL) {
        _gifMutex = xSemaphoreCreateMutex();
    }
    TEST_LOG("gif mutex=%p", _gifMutex);

    if (_lvglMutex == NULL) {
        _lvglMutex = xSemaphoreCreateMutex();
    }
    TEST_LOG("lvgl mutex=%p", _lvglMutex);

    if (_uiStateMutex == NULL) {
        _uiStateMutex = xSemaphoreCreateMutex();
    }
    TEST_LOG("ui state mutex=%p", _uiStateMutex);
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
    BaseType_t ok = xQueueSend(_queue, &data, 0);
    if (ok != pdTRUE) {
        TEST_LOG("bt queue full, drop type=%d", (int)type);
    }
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
    BaseType_t ok = xQueueSend(_queue, &data, 0);
    if (ok != pdTRUE) {
        TEST_LOG("sound queue full, drop type=%d track=%d", (int)type, track);
    }
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
    BaseType_t ok = xQueueSend(_queue, &evt, 0);
    if (ok != pdTRUE) {
        TEST_LOG("display queue full, drop type=%d", (int)type);
    }
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
    BaseType_t ok = xQueueSend(_queue, &evt, 0);
    if (ok != pdTRUE) {
        TEST_LOG("storage queue full, drop type=%d path=%s", (int)type, evt.filePath);
    }
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

bool SystemAPI::IsObdCommConnected() {
    return (obdMgr != nullptr) && (obdMgr->GetOBDStatus() == OBD_CONNECTED);
}

bool SystemAPI::IsObdDisconnected() {
    return (obdMgr != nullptr) && (obdMgr->GetOBDStatus() == OBD_DISCONNECTED);
}

bool SystemAPI::IsDisplayReady() {
    return (displayMgr != nullptr) &&
           displayMgr->IsLvglInitialized() &&
           displayMgr->IsSplashFinished();
}

Stream* SystemAPI::GetBtStream() {
    if (btMgr) return btMgr->GetBleStream();
    return nullptr;
}

GIFMemory* SystemAPI::GetPsramObjPtr() {
    return &gifObj;
}

void SystemAPI::AppendStorageLog(const char* line) {
    if (!storageMgr || !line || line[0] == '\0') return;
    storageSubscriber.SetEvent(STORAGE_APPEND_LOG, String(line));
}

void SystemAPI::PublishWifiState(bool connected, int32_t rssi) {
    if (LockUiState(pdMS_TO_TICKS(20))) {
        uiState.wifiConnected = connected;
        uiState.wifiRssi = rssi;
        UnlockUiState();
    }
}

void SystemAPI::PublishClockText(const char* hhmm) {
    if (!hhmm || hhmm[0] == '\0') return;
    if (LockUiState(pdMS_TO_TICKS(20))) {
        strncpy(uiState.clockText, hhmm, sizeof(uiState.clockText) - 1);
        uiState.clockText[sizeof(uiState.clockText) - 1] = '\0';
        uiState.clockValid = true;
        UnlockUiState();
    }
}

void SystemAPI::PublishWeatherText(const char* city, const char* weather) {
    if (!city || !weather || city[0] == '\0' || weather[0] == '\0') return;
    if (LockUiState(pdMS_TO_TICKS(20))) {
        strncpy(uiState.cityText, city, sizeof(uiState.cityText) - 1);
        uiState.cityText[sizeof(uiState.cityText) - 1] = '\0';
        strncpy(uiState.weatherText, weather, sizeof(uiState.weatherText) - 1);
        uiState.weatherText[sizeof(uiState.weatherText) - 1] = '\0';
        uiState.weatherValid = true;
        UnlockUiState();
    }
}

void SystemAPI::PublishBtConnected(bool connected) {
    if (LockUiState(pdMS_TO_TICKS(20))) {
        uiState.btConnected = connected;
        UnlockUiState();
    }
}

void SystemAPI::PublishObdStatus(int status) {
    if (LockUiState(pdMS_TO_TICKS(20))) {
        uiState.obdStatus = status;
        UnlockUiState();
    }
}

void SystemAPI::PublishObdCoolant(uint16_t coolant) {
    if (LockUiState(pdMS_TO_TICKS(20))) {
        uiState.coolant = coolant;
        uiState.coolantValid = true;
        UnlockUiState();
    }
}

void SystemAPI::PublishObdBatteryVoltage(uint16_t voltage) {
    if (LockUiState(pdMS_TO_TICKS(20))) {
        uiState.batteryVoltage = voltage;
        uiState.batteryValid = true;
        UnlockUiState();
    }
}

void SystemAPI::PublishObdOutsideTemp(int16_t tempC, bool valid) {
    if (LockUiState(pdMS_TO_TICKS(20))) {
        uiState.outsideTempC = tempC;
        uiState.outsideTempValid = valid;
        UnlockUiState();
    }
}

bool SystemAPI::GetUiSharedSnapshot(UiSharedState* out, TickType_t waitTime) {
    if (!out) return false;
    if (!LockUiState(waitTime)) return false;
    *out = uiState;
    UnlockUiState();
    return true;
}

bool SystemAPI::GetUiWifiState(bool* connected, int32_t* rssi, TickType_t waitTime) {
    if (!connected) return false;
    if (!LockUiState(waitTime)) return false;
    *connected = uiState.wifiConnected;
    if (rssi) *rssi = uiState.wifiRssi;
    UnlockUiState();
    return true;
}

bool SystemAPI::GetUiClockText(char* out, size_t outLen, TickType_t waitTime) {
    if (!out || outLen == 0) return false;
    if (!LockUiState(waitTime)) return false;
    strncpy(out, uiState.clockText, outLen - 1);
    out[outLen - 1] = '\0';
    bool valid = uiState.clockValid;
    UnlockUiState();
    return valid;
}

bool SystemAPI::GetUiWeather(char* outCity, size_t cityLen, char* outWeather, size_t weatherLen, bool* valid, TickType_t waitTime) {
    if (!outCity || cityLen == 0 || !outWeather || weatherLen == 0) return false;
    if (!LockUiState(waitTime)) return false;
    strncpy(outCity, uiState.cityText, cityLen - 1);
    outCity[cityLen - 1] = '\0';
    strncpy(outWeather, uiState.weatherText, weatherLen - 1);
    outWeather[weatherLen - 1] = '\0';
    if (valid) *valid = uiState.weatherValid;
    bool hasData = uiState.weatherValid;
    UnlockUiState();
    return hasData;
}

bool SystemAPI::GetUiBtConnected(bool* connected, TickType_t waitTime) {
    if (!connected) return false;
    if (!LockUiState(waitTime)) return false;
    *connected = uiState.btConnected;
    UnlockUiState();
    return true;
}

bool SystemAPI::GetUiObdStatus(int* status, TickType_t waitTime) {
    if (!status) return false;
    if (!LockUiState(waitTime)) return false;
    *status = uiState.obdStatus;
    UnlockUiState();
    return true;
}

bool SystemAPI::GetUiCoolant(uint16_t* coolant, bool* valid, TickType_t waitTime) {
    if (!coolant) return false;
    if (!LockUiState(waitTime)) return false;
    *coolant = uiState.coolant;
    if (valid) *valid = uiState.coolantValid;
    UnlockUiState();
    return true;
}

bool SystemAPI::GetUiBatteryVoltage(uint16_t* voltage, bool* valid, TickType_t waitTime) {
    if (!voltage) return false;
    if (!LockUiState(waitTime)) return false;
    *voltage = uiState.batteryVoltage;
    if (valid) *valid = uiState.batteryValid;
    UnlockUiState();
    return true;
}

bool SystemAPI::GetUiOutsideTemp(int16_t* tempC, bool* valid, TickType_t waitTime) {
    if (!tempC) return false;
    if (!LockUiState(waitTime)) return false;
    *tempC = uiState.outsideTempC;
    if (valid) *valid = uiState.outsideTempValid;
    UnlockUiState();
    return true;
}

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

bool SystemAPI::LockUiState(TickType_t waitTime) {
    return (_uiStateMutex) ? (xSemaphoreTake(_uiStateMutex, waitTime) == pdTRUE) : false;
}

void SystemAPI::UnlockUiState() {
    if (_uiStateMutex) xSemaphoreGive(_uiStateMutex);
}
