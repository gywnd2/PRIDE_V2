#include <CommonApi.h>
#include <BluetoothMgr.h>
#include <ObdMgr.h>
#include <StorageMgr.h>
#include <DisplayMgr.h>
#include <string.h>
#include <stdarg.h>
#include <ui.h>

#define TEST_LOG(fmt, ...) UartLogf("[SystemAPI] " fmt "\n", ##__VA_ARGS__)
#define TEST_LINE() UartLogf("[SystemAPI] %s\n", __func__)

static void trim_trailing_newline(char* text)
{
    if (!text) return;
    size_t len = strlen(text);
    while (len > 0) {
        char c = text[len - 1];
        if (c != '\n' && c != '\r') break;
        text[len - 1] = '\0';
        len--;
    }
}

static uint32_t normalize_service_oil_cycle(uint32_t cycleKm)
{
    return (cycleKm == 0U) ? SERVICE_OIL_CYCLE_DEFAULT_KM : cycleKm;
}

static uint8_t service_odo_to_oil_percent(uint32_t totalKm, uint32_t cycleKm)
{
    cycleKm = normalize_service_oil_cycle(cycleKm);
    if (totalKm >= cycleKm) return 0;

    uint32_t remainingKm = cycleKm - totalKm;
    return (uint8_t)(((remainingKm * 100U) + (cycleKm / 2U)) / cycleKm);
}

static uint32_t bump_service_odo_revision(uint32_t revision)
{
    return (revision == UINT32_MAX) ? UINT32_MAX : (revision + 1U);
}

static uint32_t saturating_add_u32(uint32_t value, uint32_t delta)
{
    if (UINT32_MAX - value < delta) return UINT32_MAX;
    return value + delta;
}

#ifndef UART_LOG_MAX_PER_SEC
#define UART_LOG_MAX_PER_SEC 48U
#endif

#ifndef UART_LOG_ONLY_WHEN_DEBUG_VISIBLE
#define UART_LOG_ONLY_WHEN_DEBUG_VISIBLE 1
#endif

static portMUX_TYPE s_uartLogBudgetMux = portMUX_INITIALIZER_UNLOCKED;

static bool uart_log_budget_acquire()
{
    static uint32_t windowStartMs = 0;
    static uint16_t windowCount = 0;

    bool allow = false;
    uint32_t now = millis();
    portENTER_CRITICAL(&s_uartLogBudgetMux);
    if (windowStartMs == 0 || (now - windowStartMs) >= 1000U) {
        windowStartMs = now;
        windowCount = 0;
    }

    if (windowCount < UART_LOG_MAX_PER_SEC) {
        windowCount++;
        allow = true;
    }
    portEXIT_CRITICAL(&s_uartLogBudgetMux);

    return allow;
}

void UartLogf(const char* fmt, ...)
{
    if (!fmt) return;
    bool debugVisible = ui_debug_log_capture_enabled();
#if UART_LOG_ONLY_WHEN_DEBUG_VISIBLE
    if (!debugVisible) return;
#endif
    if (!uart_log_budget_acquire()) return;

    char uart_line[384];
    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(uart_line, sizeof(uart_line), fmt, args);
    va_end(args);
    if (written < 0) return;

    size_t uart_len = strlen(uart_line);
    if (uart_len > 0) {
        int avail = Serial.availableForWrite();
        if (avail >= (int)uart_len) {
            Serial.write((const uint8_t*)uart_line, uart_len);
        }
    }

    trim_trailing_newline(uart_line);
    if (uart_line[0] == '\0') return;
    if (!debugVisible) return;

    char ui_line[420];
    snprintf(ui_line, sizeof(ui_line), "[UART] %s", uart_line);
    ui_debug_log_enqueue(ui_line);
}

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
    uiState.oilPercent = 100;
    uiState.serviceOilCycleKm = SERVICE_OIL_CYCLE_DEFAULT_KM;
    serviceOdoState = {};
    serviceOdoStateLoaded = false;

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

    if (_serviceOdoMutex == NULL) {
        _serviceOdoMutex = xSemaphoreCreateMutex();
    }
    TEST_LOG("service odo mutex=%p", _serviceOdoMutex);
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
    _queue = xQueueCreate(48, sizeof(StorageEventData));
}

StorageEventSubscriber::~StorageEventSubscriber() {
    if (_queue) vQueueDelete(_queue);
}

void StorageEventSubscriber::SetEvent(STORAGE_EVENT_TYPE type, const String& path) {
    StorageEventData evt;
    evt.type = type;
    strncpy(evt.filePath, path.c_str(), sizeof(evt.filePath) - 1);
    evt.filePath[sizeof(evt.filePath) - 1] = '\0';
    TickType_t wait = (type == STORAGE_APPEND_DISPLAY_LOG ||
                       type == STORAGE_FINISH_DISPLAY_LOG_SESSION) ? pdMS_TO_TICKS(40) : 0;
    BaseType_t ok = xQueueSend(_queue, &evt, wait);
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

void SystemAPI::AppendDisplayLog(const char* line) {
    if (!storageMgr || !line || line[0] == '\0') return;
    storageSubscriber.SetEvent(STORAGE_APPEND_DISPLAY_LOG, String(line));
}

void SystemAPI::FinishDisplayLogSession() {
    if (!storageMgr) return;
    storageSubscriber.SetEvent(STORAGE_FINISH_DISPLAY_LOG_SESSION);
}

bool SystemAPI::LoadServiceOdoState(ServiceOdoState* outState) {
    if (!outState || !storageMgr || !_serviceOdoMutex) return false;

    if (xSemaphoreTake(_serviceOdoMutex, pdMS_TO_TICKS(120)) != pdTRUE) {
        return false;
    }

    bool ok = true;
    if (!serviceOdoStateLoaded) {
        ServiceOdoState loaded = {};
        ok = storageMgr->ReadServiceOdoState(&loaded);
        if (ok) {
            serviceOdoState = loaded;
            serviceOdoStateLoaded = true;
        }
    }

    if (ok) *outState = serviceOdoState;
    xSemaphoreGive(_serviceOdoMutex);
    return ok;
}

bool SystemAPI::StoreServiceOdoState(const ServiceOdoState& state) {
    if (!storageMgr || !_serviceOdoMutex) return false;
    if (!storageMgr->WriteServiceOdoState(state)) return false;

    if (xSemaphoreTake(_serviceOdoMutex, pdMS_TO_TICKS(120)) != pdTRUE) {
        return false;
    }
    serviceOdoState = state;
    serviceOdoStateLoaded = true;
    xSemaphoreGive(_serviceOdoMutex);
    return true;
}

bool SystemAPI::GetCurrentOrLastOdoKm(uint32_t* outOdoKm) {
    if (!outOdoKm) return false;
    *outOdoKm = 0;

    if (obdMgr) {
        uint32_t currentOdoKm = obdMgr->GetOdometerKm();
        if (currentOdoKm > 0 || obdMgr->GetOBDStatus() == OBD_CONNECTED) {
            *outOdoKm = currentOdoKm;
            return true;
        }
    }

    ServiceOdoState state = {};
    if (LoadServiceOdoState(&state) && state.last_seen_odo_km > 0) {
        *outOdoKm = state.last_seen_odo_km;
        return true;
    }
    return false;
}

bool SystemAPI::RefreshServiceDueFromStorage() {
    if (!storageMgr) {
        PublishServiceDue(false);
        return false;
    }

    uint32_t cycleKm = SERVICE_OIL_CYCLE_DEFAULT_KM;
    bool cycleOk = storageMgr->ReadServiceOilCycleKm(&cycleKm);
    cycleKm = normalize_service_oil_cycle(cycleKm);
    if (LockUiState(pdMS_TO_TICKS(20))) {
        uiState.serviceOilCycleKm = cycleKm;
        UnlockUiState();
    }

    ServiceOdoState state = {};
    bool ok = LoadServiceOdoState(&state);
    if (ok) PublishServiceOdoKm(state.last_service_km);
    else PublishServiceOdoKm(0);
    return ok && cycleOk;
}

bool SystemAPI::UpdateServiceOdoFromCurrentOdo(uint32_t currentOdoKm, uint32_t* serviceOut, bool persistSnapshot) {
    if (serviceOut) *serviceOut = 0;
    if (!storageMgr) return false;

    ServiceOdoState state = {};
    if (!LoadServiceOdoState(&state)) return false;

    bool shouldPersist = persistSnapshot;
    uint32_t serviceKm = state.last_service_km;
    uint32_t deltaKm = 0;

    if (!state.base_odo_valid) {
        state.base_odo_valid = true;
        state.base_odo_km = 0;
        state.last_seen_odo_km = currentOdoKm;
        shouldPersist = true;
        TEST_LOG("service odo accumulator initialized source=%u service=%u",
                 (unsigned int)currentOdoKm,
                 (unsigned int)serviceKm);
    } else if (state.last_seen_odo_km == 0U) {
        state.last_seen_odo_km = currentOdoKm;
        shouldPersist = true;
        TEST_LOG("service odo source baseline set source=%u service=%u",
                 (unsigned int)currentOdoKm,
                 (unsigned int)serviceKm);
    } else if (currentOdoKm >= state.last_seen_odo_km) {
        deltaKm = currentOdoKm - state.last_seen_odo_km;
        if (deltaKm > 0U) {
            serviceKm = saturating_add_u32(serviceKm, deltaKm);
        }
    } else {
        TEST_LOG("service odo source reset last_seen=%u current=%u service=%u",
                 (unsigned int)state.last_seen_odo_km,
                 (unsigned int)currentOdoKm,
                 (unsigned int)serviceKm);
        if (persistSnapshot) {
            shouldPersist = true;
        }
    }

    state.last_seen_odo_km = currentOdoKm;
    state.last_service_km = serviceKm;

    bool ok = true;
    if (shouldPersist) {
        state.revision = bump_service_odo_revision(state.revision);
        ok = StoreServiceOdoState(state);
    } else if (_serviceOdoMutex && xSemaphoreTake(_serviceOdoMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        serviceOdoState = state;
        serviceOdoStateLoaded = true;
        xSemaphoreGive(_serviceOdoMutex);
    }

    if (ok) {
        PublishServiceOdoKm(serviceKm);
        if (serviceOut) *serviceOut = serviceKm;
    }
    return ok;
}

bool SystemAPI::PersistServiceOdoSnapshot(uint32_t currentOdoKm, uint32_t* serviceOut) {
    return UpdateServiceOdoFromCurrentOdo(currentOdoKm, serviceOut, true);
}

bool SystemAPI::ResetServiceOdo() {
    uint32_t currentOdoKm = 0;
    if (!GetCurrentOrLastOdoKm(&currentOdoKm)) return false;

    ServiceOdoState state = {};
    LoadServiceOdoState(&state);
    state.revision = bump_service_odo_revision(state.revision);
    state.base_odo_valid = true;
    state.base_odo_km = 0;
    state.last_seen_odo_km = currentOdoKm;
    state.last_service_km = 0;

    bool ok = StoreServiceOdoState(state);
    if (ok) {
        if (obdMgr) {
            obdMgr->ResetServiceOdoSessionBase();
        }
        PublishServiceOdoKm(0);
    }
    return ok;
}

bool SystemAPI::GetServiceOdoKm(uint32_t* outKm) {
    if (outKm) *outKm = 0;
    if (!outKm) return false;

    ServiceOdoState state = {};
    if (!LoadServiceOdoState(&state)) return false;
    *outKm = state.last_service_km;
    return true;
}

bool SystemAPI::SetServiceOdoKm(uint32_t serviceKm) {
    if (!storageMgr) return false;

    uint32_t currentOdoKm = 0;
    GetCurrentOrLastOdoKm(&currentOdoKm);

    ServiceOdoState state = {};
    LoadServiceOdoState(&state);
    state.revision = bump_service_odo_revision(state.revision);
    state.base_odo_valid = true;
    state.base_odo_km = 0;
    state.last_seen_odo_km = currentOdoKm;
    state.last_service_km = serviceKm;

    bool ok = StoreServiceOdoState(state);
    if (ok) PublishServiceOdoKm(state.last_service_km);
    return ok;
}

bool SystemAPI::GetServiceOdoBaseKm(uint32_t* outKm, bool* valid) {
    if (outKm) *outKm = 0;
    if (valid) *valid = false;
    if (!outKm) return false;

    ServiceOdoState state = {};
    if (!LoadServiceOdoState(&state)) return false;
    *outKm = state.last_service_km;
    if (valid) *valid = true;
    return true;
}

bool SystemAPI::SetServiceOdoBaseKm(uint32_t baseOdoKm) {
    return SetServiceOdoKm(baseOdoKm);
}

bool SystemAPI::GetServiceOilCycleKm(uint32_t* outKm) {
    if (!outKm) return false;
    *outKm = SERVICE_OIL_CYCLE_DEFAULT_KM;
    if (!storageMgr) return false;

    uint32_t cycleKm = SERVICE_OIL_CYCLE_DEFAULT_KM;
    bool ok = storageMgr->ReadServiceOilCycleKm(&cycleKm);
    *outKm = normalize_service_oil_cycle(cycleKm);
    return ok;
}

bool SystemAPI::SetServiceOilCycleKm(uint32_t cycleKm) {
    if (!storageMgr) return false;

    uint32_t writtenKm = SERVICE_OIL_CYCLE_DEFAULT_KM;
    bool ok = storageMgr->WriteServiceOilCycleKm(normalize_service_oil_cycle(cycleKm), &writtenKm);
    if (ok) {
        RefreshServiceDueFromStorage();
    }
    return ok;
}

bool SystemAPI::ResetServiceOilCycleKm() {
    return SetServiceOilCycleKm(SERVICE_OIL_CYCLE_DEFAULT_KM);
}

extern "C" bool ui_reset_service_odo(void)
{
    SystemAPI* system = SystemAPI::getInstance();
    if (!system) return false;
    return system->ResetServiceOdo();
}

extern "C" bool ui_get_service_odo_km(uint32_t* outKm)
{
    SystemAPI* system = SystemAPI::getInstance();
    if (!system) return false;
    return system->GetServiceOdoKm(outKm);
}

extern "C" bool ui_set_service_odo_km(uint32_t serviceKm)
{
    SystemAPI* system = SystemAPI::getInstance();
    if (!system) return false;
    return system->SetServiceOdoKm(serviceKm);
}

extern "C" bool ui_get_service_odo_base_km(uint32_t* outKm, bool* valid)
{
    SystemAPI* system = SystemAPI::getInstance();
    if (!system) return false;
    return system->GetServiceOdoBaseKm(outKm, valid);
}

extern "C" bool ui_set_service_odo_base_km(uint32_t baseOdoKm)
{
    SystemAPI* system = SystemAPI::getInstance();
    if (!system) return false;
    return system->SetServiceOdoBaseKm(baseOdoKm);
}

extern "C" bool ui_get_service_oil_cycle_km(uint32_t* outKm)
{
    SystemAPI* system = SystemAPI::getInstance();
    if (!system) return false;
    return system->GetServiceOilCycleKm(outKm);
}

extern "C" bool ui_set_service_oil_cycle_km(uint32_t cycleKm)
{
    SystemAPI* system = SystemAPI::getInstance();
    if (!system) return false;
    return system->SetServiceOilCycleKm(cycleKm);
}

extern "C" bool ui_reset_service_oil_cycle_km(void)
{
    SystemAPI* system = SystemAPI::getInstance();
    if (!system) return false;
    return system->ResetServiceOilCycleKm();
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

void SystemAPI::PublishServiceDue(bool due) {
    if (LockUiState(pdMS_TO_TICKS(20))) {
        uiState.serviceDue = due;
        UnlockUiState();
    }
}

void SystemAPI::PublishServiceOdoKm(uint32_t totalKm) {
    if (LockUiState(pdMS_TO_TICKS(20))) {
        uint32_t cycleKm = normalize_service_oil_cycle(uiState.serviceOilCycleKm);
        uiState.serviceOdoKm = totalKm;
        uiState.serviceOilCycleKm = cycleKm;
        uiState.serviceDue = totalKm >= cycleKm;
        uiState.oilPercent = service_odo_to_oil_percent(totalKm, cycleKm);
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

bool SystemAPI::GetObdDataSnapshot(ObdData* out, TickType_t waitTime) {
    (void)waitTime;
    if (!out || !obdMgr) return false;
    *out = obdMgr->GetObdData();
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
