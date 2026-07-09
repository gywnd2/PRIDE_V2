#include <ObdMgr.h>
#include <CommonApi.h>
#include <string.h>

#define TEST_LOG(fmt, ...) UartLogf("[ObdMgr] " fmt "\n", ##__VA_ARGS__)
#define TEST_LINE() UartLogf("[ObdMgr] %s\n", __func__)

String ObdStatusStr[8] =
{
    "BT Init Failed",
    "BT Init Success",
    "BT Connecting",
    "BT Connect Failed",
    "OBD Init Failed",
    "OBD Init Success",
    "OBD Connected",
    "OBD Disconnected"
};

static portMUX_TYPE s_obdBusyMux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_lastOdoLogMs = 0;
static uint32_t s_lastOdoSourceLogMs = 0;
static uint32_t s_lastOdoSourceLogValue = UINT32_MAX;
static int8_t s_lastOdoLogState = ELM_GENERAL_ERROR;
static constexpr uint16_t ELM_INIT_TIMEOUT_MS = 600;
static constexpr uint16_t ELM_PID_TIMEOUT_MS = 1500;
static constexpr uint16_t ELM_QUERY_WAIT_INTERVAL_MS = 30;
static constexpr char ELM_INIT_PROTOCOL = ISO_15765_11_BIT_500_KBAUD;
static constexpr uint32_t ODO_QUERY_INTERVAL_MS = 1000;
static constexpr uint8_t ODO_LINK_LOST_RETRY_MAX = 2;
static constexpr uint32_t OBD_DIAG_LOG_INTERVAL_MS = 30000;
static constexpr const char* ODO_CLUSTER_HEADER = "7C6";
static constexpr const char* ODO_DEFAULT_HEADER = "7DF";
static constexpr const char* ODO_CLUSTER_QUERY = "22B002";
static constexpr const char* ODO_CLUSTER_RESPONSE = "62B002";

namespace {
const char* ElmStateToString(int8_t state)
{
    switch (state) {
        case ELM_SUCCESS: return "SUCCESS";
        case ELM_NO_RESPONSE: return "NO_RESPONSE";
        case ELM_BUFFER_OVERFLOW: return "BUFFER_OVERFLOW";
        case ELM_GARBAGE: return "GARBAGE";
        case ELM_UNABLE_TO_CONNECT: return "UNABLE_TO_CONNECT";
        case ELM_NO_DATA: return "NO_DATA";
        case ELM_STOPPED: return "STOPPED";
        case ELM_TIMEOUT: return "TIMEOUT";
        case ELM_GETTING_MSG: return "GETTING_MSG";
        case ELM_MSG_RXD: return "MSG_RXD";
        case ELM_GENERAL_ERROR: return "GENERAL_ERROR";
        default: return "UNKNOWN";
    }
}

bool ShouldLogOdometer(int8_t state)
{
    const uint32_t now = millis();
    const uint32_t minIntervalMs = (state == ELM_SUCCESS) ? 5000U : 1500U;
    if (state != s_lastOdoLogState || (now - s_lastOdoLogMs) >= minIntervalMs) {
        s_lastOdoLogState = state;
        s_lastOdoLogMs = now;
        return true;
    }
    return false;
}

bool IsObdLinkLostState(int8_t state)
{
    return state == ELM_GENERAL_ERROR ||
           state == ELM_NO_RESPONSE ||
           state == ELM_NO_DATA ||
           state == ELM_TIMEOUT;
}

bool HexNibble(char c, uint8_t* out)
{
    if (!out) return false;
    if (c >= '0' && c <= '9') {
        *out = (uint8_t)(c - '0');
        return true;
    }
    if (c >= 'A' && c <= 'F') {
        *out = (uint8_t)(c - 'A' + 10);
        return true;
    }
    if (c >= 'a' && c <= 'f') {
        *out = (uint8_t)(c - 'a' + 10);
        return true;
    }
    return false;
}

bool ParseHexUInt24AfterMarker(const char* payload, const char* marker, uint32_t* outValue)
{
    if (!payload || !marker || !outValue) return false;

    const char* p = strstr(payload, marker);
    if (!p) return false;
    p += strlen(marker);

    uint32_t value = 0;
    for (uint8_t i = 0; i < 6; ++i) {
        uint8_t nibble = 0;
        if (!HexNibble(p[i], &nibble)) return false;
        value = (value << 4) | nibble;
    }

    *outValue = value;
    return true;
}

void FormatLittleEndian24(uint32_t value, char* out, size_t outLen)
{
    if (!out || outLen < 7) return;
    snprintf(out,
             outLen,
             "%02lX%02lX%02lX",
             (unsigned long)(value & 0xFFU),
             (unsigned long)((value >> 8) & 0xFFU),
             (unsigned long)((value >> 16) & 0xFFU));
}

void FormatLittleEndian32(uint32_t value, char* out, size_t outLen)
{
    if (!out || outLen < 9) return;
    snprintf(out,
             outLen,
             "%02lX%02lX%02lX%02lX",
             (unsigned long)(value & 0xFFU),
             (unsigned long)((value >> 8) & 0xFFU),
             (unsigned long)((value >> 16) & 0xFFU),
             (unsigned long)((value >> 24) & 0xFFU));
}

String NormalizeHexLine(const String& line)
{
    String out;
    out.reserve(line.length());
    for (size_t i = 0; i < line.length(); ++i) {
        char c = line.charAt(i);
        if (c >= '0' && c <= '9') {
            out += c;
        } else if (c >= 'a' && c <= 'f') {
            out += (char)(c - 'a' + 'A');
        } else if (c >= 'A' && c <= 'F') {
            out += c;
        }
    }
    return out;
}

bool ShouldLogOdoSource(uint32_t value)
{
    const uint32_t now = millis();
    if (value != s_lastOdoSourceLogValue || (now - s_lastOdoSourceLogMs) >= 5000U) {
        s_lastOdoSourceLogValue = value;
        s_lastOdoSourceLogMs = now;
        return true;
    }
    return false;
}

void LogPidError(const char* pid, int8_t state, const char* detail = nullptr)
{
    if (detail && detail[0] != '\0') {
        TEST_LOG("pid %s error state=%d (%s) %s",
                 pid,
                 (int)state,
                 ElmStateToString(state),
                 detail);
        return;
    }
    TEST_LOG("pid %s error state=%d (%s)",
             pid,
             (int)state,
             ElmStateToString(state));
}
} // namespace

bool ObdMgr::LockData(TickType_t waitTime)
{
    return (_dataMutex != nullptr) && (xSemaphoreTake(_dataMutex, waitTime) == pdTRUE);
}

void ObdMgr::UnlockData()
{
    if (_dataMutex) xSemaphoreGive(_dataMutex);
}

bool ObdMgr::TryLockObdQuery()
{
    bool locked = false;
    portENTER_CRITICAL(&s_obdBusyMux);
    if (!obd_busy) {
        obd_busy = true;
        locked = true;
    }
    portEXIT_CRITICAL(&s_obdBusyMux);
    return locked;
}

void ObdMgr::UnlockObdQuery()
{
    portENTER_CRITICAL(&s_obdBusyMux);
    obd_busy = false;
    portEXIT_CRITICAL(&s_obdBusyMux);
}

void ObdMgr::PostEvent(ObdMgrEventType type)
{
    if (!_eventQueue) return;
    ObdMgrEventData evt = {};
    evt.type = type;
    if (xQueueSend(_eventQueue, &evt, 0) != pdTRUE) {
        TEST_LOG("event queue full, drop type=%d", (int)type);
    }
}

bool ObdMgr::StartConnectTask()
{
    if (_connectTaskRunning) return true;
    BaseType_t ret = xTaskCreatePinnedToCore(
        ConnectBTTask,
        "ConnectBTTask",
        8192,
        this,
        2,
        &_connectTask,
        0
    );
    if (ret != pdPASS) {
        _connectTask = NULL;
        Serial.println("[ObdMgr] Critical: ConnectBTTask create failed");
        return false;
    }
    _connectTaskRunning = true;
    TEST_LOG("ConnectBTTask created");
    return true;
}

bool ObdMgr::StartQueryTask()
{
    if (_queryTaskRunning) return true;
    BaseType_t ret = xTaskCreatePinnedToCore(
        QueryOBDData,
        "QueryOBDData",
        4096,
        this,
        3,
        &query_obd_data_task,
        0
    );
    if (ret != pdPASS) {
        query_obd_data_task = NULL;
        Serial.println("[ObdMgr] Critical: QueryOBDData task create failed");
        return false;
    }
    _queryTaskRunning = true;
    TEST_LOG("QueryOBDData task created");
    return true;
}

void ObdMgr::Init(void)
{
    TEST_LINE();
    if (_dataMutex == nullptr) {
        _dataMutex = xSemaphoreCreateMutex();
    }
    TEST_LOG("data mutex=%p", _dataMutex);
    if (_eventQueue == nullptr) {
        _eventQueue = xQueueCreate(16, sizeof(ObdMgrEventData));
    }
    TEST_LOG("event queue=%p", _eventQueue);

    if (LockData(pdMS_TO_TICKS(20))) {
        memset(&obd_data, 0, sizeof(obd_data));
        obd_status = BT_INIT_FAILED;
        UnlockData();
    }
    _connectTask = NULL;
    query_obd_data_task = NULL;
    _connectTaskRunning = false;
    _queryTaskRunning = false;
    _hadPidSuccess = false;
    _awaitingOdometerRecovery = false;
    _goodbyeScreenActive = false;
    _bootMs = millis();
    _odometerStartKm = 0;
    _odometerLastKm = 0;
    _odometerStartValid = false;

    Serial.println("[ObdMgr] S3 BLE OBD task started");
    SetOBDStatus(BT_INIT_SUCCESS);
    vTaskDelay(pdMS_TO_TICKS(100));

    if (_eventTask != NULL) {
        vTaskDelete(_eventTask);
        _eventTask = NULL;
    }

    BaseType_t ret = xTaskCreatePinnedToCore(EventTask, "ObdEventTask", 4096, this, 2, &_eventTask, 0);
    if (ret != pdPASS) {
        Serial.println("[ObdMgr] Critical: ObdEventTask create failed");
    } else {
        TEST_LOG("ObdEventTask created");
        PostEvent(OBD_MGR_EVENT_START_CONNECT);
    }
}

void ObdMgr::FinalizeServiceOdoSession(SystemAPI* system)
{
    if (!system || _odometerLastKm == 0) return;

    uint32_t serviceKm = 0;
    if (system->PersistServiceOdoSnapshot(_odometerLastKm, &serviceKm)) {
        TEST_LOG("service odo snapshot persisted start=%u end=%u service=%u",
                 (unsigned int)_odometerStartKm,
                 (unsigned int)_odometerLastKm,
                 (unsigned int)serviceKm);
        _odometerStartKm = _odometerLastKm;
    } else {
        TEST_LOG("service odo snapshot persist failed start=%u end=%u",
                 (unsigned int)_odometerStartKm,
                 (unsigned int)_odometerLastKm);
    }
}

void ObdMgr::HandleQueryLinkLoss(SystemAPI* system, const char* reason)
{
    _queryTaskRunning = false;
    query_obd_data_task = NULL;

    bool confirmedConnected = _hadPidSuccess && (GetOBDStatus() == OBD_CONNECTED);
    TEST_LOG("query link loss: confirmed=%d had_pid=%d status=%d reason=%s",
             confirmedConnected ? 1 : 0,
             _hadPidSuccess ? 1 : 0,
             GetOBDStatus(),
             reason ? reason : "(null)");

    if (confirmedConnected) {
        PostEvent(OBD_MGR_EVENT_LINK_LOST);
        return;
    }

    SetOBDStatus(OBD_DISCONNECTED);
    if (system) {
        system->btSubscriber.SetEvent(BT_REQUEST_DISCONNECT);
    }
    vTaskDelay(pdMS_TO_TICKS(OBD_RECONNECT_INTERVAL_MS));
    PostEvent(OBD_MGR_EVENT_START_CONNECT);
}

void ObdMgr::EventTask(void *param)
{
    TEST_LINE();
    ObdMgr* self = static_cast<ObdMgr*>(param);
    SystemAPI* system = SystemAPI::getInstance();
    if (self == NULL || system == NULL || self->_eventQueue == NULL) {
        vTaskDelete(NULL);
        return;
    }

    ObdMgrEventData event = {};
    while (true) {
        if (xQueueReceive(self->_eventQueue, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        switch (event.type) {
            case OBD_MGR_EVENT_START_CONNECT:
            {
                if (self->GetOBDStatus() == OBD_CONNECTED || self->_connectTaskRunning) {
                    break;
                }
                if (!self->StartConnectTask()) {
                    vTaskDelay(pdMS_TO_TICKS(OBD_RECONNECT_INTERVAL_MS));
                    self->PostEvent(OBD_MGR_EVENT_START_CONNECT);
                }
                break;
            }

            case OBD_MGR_EVENT_LINK_LOST:
            {
                self->SetOBDStatus(OBD_DISCONNECTED);
                self->_awaitingOdometerRecovery = true;
                system->btSubscriber.SetEvent(BT_REQUEST_DISCONNECT);

                if (!self->_goodbyeScreenActive) {
                    self->FinalizeServiceOdoSession(system);
                    system->displaySubscriber.SetEvent(DISPLAY_SHOW_GOODBYE);
                    system->soundSubscriber.SetEvent(SOUND_PLAY_TRACK, 2);
                    self->_goodbyeScreenActive = true;
                }

                vTaskDelay(pdMS_TO_TICKS(OBD_RECONNECT_INTERVAL_MS));
                self->PostEvent(OBD_MGR_EVENT_START_CONNECT);
                break;
            }

            case OBD_MGR_EVENT_ODOMETER_SUCCESS:
            {
                self->_hadPidSuccess = true;
                if (system->GetOBDConnected()) {
                    self->SetOBDStatus(OBD_CONNECTED);
                }
                if (self->_awaitingOdometerRecovery) {
                    self->_awaitingOdometerRecovery = false;
                    if (self->_goodbyeScreenActive) {
                        system->displaySubscriber.SetEvent(DISPLAY_SHOW_GAUGE_REBOOT);
                        self->_goodbyeScreenActive = false;
                    }
                }
                break;
            }

            default:
                break;
        }
    }
}

void ObdMgr::ConnectBTTask(void *param)
{
    TEST_LINE();
    ObdMgr* self = static_cast<ObdMgr*>(param);
    SystemAPI* system = SystemAPI::getInstance();

    if (self == NULL || system == NULL)
    {
        if (self) {
            self->_connectTaskRunning = false;
            self->_connectTask = NULL;
        }
        vTaskDelete(NULL);
        return;
    }

    // Defer BLE scan/connect during splash/GIF startup to reduce shared bus contention.
    TickType_t waitStart = xTaskGetTickCount();
    const TickType_t waitTimeout = pdMS_TO_TICKS(15000);
    while (!system->IsDisplayReady() &&
           (xTaskGetTickCount() - waitStart) < waitTimeout) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (!system->IsDisplayReady()) {
        TEST_LOG("Display not ready after defer timeout, continue BLE connect");
    } else {
        TEST_LOG("Display ready, start BLE connect");
    }

    Serial.println("[ObdMgr] Starting BLE Scan/Connect...");
    self->SetOBDStatus(BT_CONNECTING);

#ifndef OBD_SIMUL_MODE
    system->ConnectOBD();

    int timeout = 0;
    while (!system->GetOBDConnected() && timeout < 20)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
        timeout++;
        Serial.print(".");
    }

    if (!system->GetOBDConnected())
    {
        Serial.println("\n[ObdMgr] BLE Connect Failed");
        self->SetOBDStatus(BT_CONNECT_FAILED);
        self->_connectTaskRunning = false;
        self->_connectTask = NULL;
        vTaskDelay(pdMS_TO_TICKS(OBD_RECONNECT_INTERVAL_MS));
        self->PostEvent(OBD_MGR_EVENT_START_CONNECT);
        vTaskDelete(NULL);
        return;
    }

    Serial.println("\n[ObdMgr] BLE Connected. Initializing ELM327...");

    Stream* bleStream = system->GetBtStream();
    bool elmReady = false;
    if (bleStream != nullptr) {
        // Fail-fast init: avoid long auto-protocol search that can starve IDLE task.
        elmReady = self->myELM327.begin(*bleStream,
                                        false,
                                        ELM_INIT_TIMEOUT_MS,
                                        ELM_INIT_PROTOCOL);
        TEST_LOG("ELM init result=%d connected=%d protocol=%c timeout=%u",
                 elmReady ? 1 : 0,
                 self->myELM327.connected ? 1 : 0,
                 (int)ELM_INIT_PROTOCOL,
                 (unsigned int)ELM_INIT_TIMEOUT_MS);
    } else {
        TEST_LOG("ELM init skipped: BLE stream null");
    }

    if (!elmReady || !self->myELM327.connected) {
        self->SetOBDStatus(OBD_INIT_FAILED);
        system->btSubscriber.SetEvent(BT_REQUEST_DISCONNECT);
        self->_connectTaskRunning = false;
        self->_connectTask = NULL;
        vTaskDelay(pdMS_TO_TICKS(OBD_RECONNECT_INTERVAL_MS));
        self->PostEvent(OBD_MGR_EVENT_START_CONNECT);
        vTaskDelete(NULL);
        return;
    }
#endif

    self->SetOBDStatus(OBD_CONNECTED);
    if (!self->StartQueryTask()) {
        self->SetOBDStatus(OBD_DISCONNECTED);
        system->btSubscriber.SetEvent(BT_REQUEST_DISCONNECT);
        self->_connectTaskRunning = false;
        self->_connectTask = NULL;
        vTaskDelay(pdMS_TO_TICKS(OBD_RECONNECT_INTERVAL_MS));
        self->PostEvent(OBD_MGR_EVENT_START_CONNECT);
        vTaskDelete(NULL);
        return;
    }
    self->_connectTaskRunning = false;
    self->_connectTask = NULL;
    vTaskDelete(NULL);
}

void ObdMgr::QueryCoolant(uint16_t &coolant_temp)
{
#ifdef OBD_SIMUL_MODE
    vTaskDelay(pdMS_TO_TICKS(OBD_SIMUL_QUERY_TIME));
    coolant_temp = rand() % (150 - 0 + 1) + 0;
    TEST_LOG("pid 0105 ok value=%u C [sim]", (unsigned int)coolant_temp);
#else
    int8_t finalState = ELM_GENERAL_ERROR;
    while (true)
    {
        float coolant = myELM327.engineCoolantTemp();
        finalState = myELM327.nb_rx_state;
        if (finalState == ELM_SUCCESS)
        {
            coolant_temp = (uint16_t)coolant;
            break;
        }
        if (finalState != ELM_GETTING_MSG) {
            coolant_temp = OBD_QUERY_INVALID_RESPONSE;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(ELM_QUERY_WAIT_INTERVAL_MS));
    }
    if (finalState == ELM_SUCCESS) {
        TEST_LOG("pid 0105 ok value=%u C", (unsigned int)coolant_temp);
    } else {
        LogPidError("0105", finalState);
    }
#endif
}

void ObdMgr::QueryVoltage(uint16_t &voltage_level)
{
#ifdef OBD_SIMUL_MODE
    vTaskDelay(pdMS_TO_TICKS(OBD_SIMUL_QUERY_TIME));
    voltage_level = rand() % (18 - 6 + 1) + 6;
    TEST_LOG("pid 0142 ok value=%u V [sim]", (unsigned int)voltage_level);
#else
    int8_t finalState = ELM_GENERAL_ERROR;
    float finalVoltage = 0.0f;
    while (true)
    {
        float voltage = myELM327.batteryVoltage();
        finalState = myELM327.nb_rx_state;
        if (finalState == ELM_SUCCESS)
        {
            finalVoltage = voltage;
            voltage_level = (uint16_t)voltage;
            break;
        }
        if (finalState != ELM_GETTING_MSG) {
            voltage_level = OBD_QUERY_INVALID_RESPONSE;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(ELM_QUERY_WAIT_INTERVAL_MS));
    }
    if (finalState == ELM_SUCCESS) {
        TEST_LOG("pid 0142 ok value=%.1f V", (double)finalVoltage);
    } else {
        LogPidError("0142", finalState);
    }
#endif
}

void ObdMgr::QueryOdometer(uint32_t &odometer_km)
{
#ifdef OBD_SIMUL_MODE
    static uint32_t simOdometer = 123456;
    vTaskDelay(pdMS_TO_TICKS(OBD_SIMUL_QUERY_TIME));
    simOdometer += 1;
    odometer_km = simOdometer;
    myELM327.nb_rx_state = ELM_SUCCESS;
    TEST_LOG("pid 0131 ok value=%u km [sim]", (unsigned int)odometer_km);
#else
    int8_t finalState = ELM_GENERAL_ERROR;
    float odometerRaw = 0.0f;
    uint32_t startMs = millis();
    while (true) {
        odometerRaw = myELM327.distSinceCodesCleared();
        finalState = myELM327.nb_rx_state;
        if (finalState == ELM_SUCCESS) {
            odometer_km = (uint32_t)odometerRaw;
            break;
        }
        if (finalState != ELM_GETTING_MSG) {
            odometer_km = OBD_QUERY_INVALID_RESPONSE;
            break;
        }
        if ((millis() - startMs) >= ELM_PID_TIMEOUT_MS) {
            finalState = ELM_TIMEOUT;
            myELM327.nb_rx_state = ELM_TIMEOUT;
            odometer_km = OBD_QUERY_INVALID_RESPONSE;
            break;
        }
        // Yield CPU while waiting ELM response to reduce bus/RTOS contention.
        vTaskDelay(pdMS_TO_TICKS(ELM_QUERY_WAIT_INTERVAL_MS));
    }
    if (finalState == ELM_SUCCESS) {
        if (ShouldLogOdoSource(odometer_km)) {
            Serial.printf("[ObdMgr] odo source 0131 value=%lu km\n",
                          (unsigned long)odometer_km);
        }
        TEST_LOG("pid 0131 ok value=%u km", (unsigned int)odometer_km);
    } else {
        LogPidError("0131", finalState);
    }
#endif
}

bool ObdMgr::QueryClusterOdometer(uint32_t &odometer_km, String* payloadOut)
{
    odometer_km = OBD_QUERY_INVALID_RESPONSE;
    if (payloadOut) *payloadOut = "";

#ifdef OBD_SIMUL_MODE
    return false;
#else
    static uint8_t failCount = 0;
    static uint32_t nextAttemptMs = 0;
    uint32_t now = millis();
    if (failCount >= 3U && (int32_t)(now - nextAttemptMs) < 0) {
        return false;
    }

    char setHeaderCmd[16] = {0};
    snprintf(setHeaderCmd, sizeof(setHeaderCmd), "AT SH %s", ODO_CLUSTER_HEADER);

    int8_t headerState = myELM327.sendCommand_Blocking(setHeaderCmd);
    if (headerState != ELM_SUCCESS) {
        myELM327.sendCommand_Blocking("AT SH 7DF");
        return false;
    }

    myELM327.flushInputBuff();
    int8_t queryState = myELM327.sendCommand_Blocking(ODO_CLUSTER_QUERY);
    String payload = "";
    if (myELM327.payload != nullptr) {
        payload = String(myELM327.payload);
    }

    char restoreHeaderCmd[16] = {0};
    snprintf(restoreHeaderCmd, sizeof(restoreHeaderCmd), "AT SH %s", ODO_DEFAULT_HEADER);
    myELM327.sendCommand_Blocking(restoreHeaderCmd);

    if (payloadOut) *payloadOut = payload;
    myELM327.nb_rx_state = queryState;

    uint32_t parsedKm = 0;
    if (queryState == ELM_SUCCESS &&
        ParseHexUInt24AfterMarker(payload.c_str(), ODO_CLUSTER_RESPONSE, &parsedKm) &&
        parsedKm > 0U) {
        odometer_km = parsedKm;
        failCount = 0;
        nextAttemptMs = 0;
        return true;
    }

    if (failCount < 255U) failCount++;
    if (failCount >= 3U) {
        nextAttemptMs = millis() + 60000U;
    }
    return false;
#endif
}

void ObdMgr::QueryRPM(uint16_t &rpm_value)
{
    int rpm_retry_count = 0;
#ifdef OBD_SIMUL_MODE
    vTaskDelay(pdMS_TO_TICKS(OBD_SIMUL_QUERY_TIME));
    rpm_value = rand() % (6000 - 1000 + 1) + 1000;
    if (ShouldLogOdometer(ELM_SUCCESS)) {
        TEST_LOG("pid 010C ok value=%u rpm [sim]", (unsigned int)rpm_value);
    }
#else
    int8_t finalState = ELM_GENERAL_ERROR;
    while (true)
    {
        float rpm = myELM327.rpm();
        finalState = myELM327.nb_rx_state;
        if (finalState == ELM_SUCCESS)
        {
            rpm_value = (uint16_t)rpm;
            break;
        }
        if (finalState == ELM_NO_DATA)
        {
            if (rpm_retry_count >= RPM_REQ_RETRY_MAX)
            {
                rpm_value = OBD_QUERY_INVALID_RESPONSE;
                break;
            }
            rpm_retry_count++;
        }
        else if (finalState != ELM_GETTING_MSG)
        {
            rpm_value = OBD_QUERY_INVALID_RESPONSE;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(ELM_QUERY_WAIT_INTERVAL_MS));
    }
    if (ShouldLogOdometer(finalState)) {
        if (finalState == ELM_SUCCESS) {
            TEST_LOG("pid 010C ok value=%u rpm", (unsigned int)rpm_value);
        } else {
            LogPidError("010C", finalState);
        }
    }
#endif
}

bool ObdMgr::QueryOutsideTemp(float& outsideTempC)
{
#ifdef OBD_SIMUL_MODE
    outsideTempC = (float)(rand() % 40) - 10.0f;
    TEST_LOG("pid 0146 ok value=%.1f C [sim]", (double)outsideTempC);
    return true;
#else
    outsideTempC = 0.0f;
    if (GetOBDStatus() != OBD_CONNECTED) return false;
    if (!TryLockObdQuery()) return false;

    bool success = false;
    int8_t finalState = ELM_GENERAL_ERROR;
    uint32_t startMs = millis();
    while (true) {
        outsideTempC = myELM327.ambientAirTemp();
        finalState = myELM327.nb_rx_state;
        if (finalState == ELM_SUCCESS) {
            success = true;
            break;
        }
        if (finalState != ELM_GETTING_MSG) {
            break;
        }
        if ((millis() - startMs) >= ELM_PID_TIMEOUT_MS) {
            finalState = ELM_TIMEOUT;
            myELM327.nb_rx_state = ELM_TIMEOUT;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(ELM_QUERY_WAIT_INTERVAL_MS));
    }

    UnlockObdQuery();
    if (success) {
        TEST_LOG("pid 0146 ok value=%.1f C", (double)outsideTempC);
    } else {
        LogPidError("0146", finalState);
    }
    return success;
#endif
}

bool ObdMgr::QueryPidRaw(const String& pidCommand, String& payloadOut, int8_t& stateOut)
{
    payloadOut = "";
    stateOut = ELM_GENERAL_ERROR;

#ifdef OBD_SIMUL_MODE
    payloadOut = "SIM";
    stateOut = ELM_SUCCESS;
    TEST_LOG("PID query cmd=%s: OK payload=%s [sim]",
             pidCommand.c_str(),
             payloadOut.c_str());
    return true;
#else
    if (pidCommand.length() < 4) {
        return false;
    }
    if (GetOBDStatus() != OBD_CONNECTED) {
        stateOut = ELM_UNABLE_TO_CONNECT;
        return false;
    }

    const TickType_t lockTimeout = pdMS_TO_TICKS(1200);
    TickType_t lockStart = xTaskGetTickCount();
    while (!TryLockObdQuery()) {
        if ((xTaskGetTickCount() - lockStart) >= lockTimeout) {
            stateOut = ELM_GETTING_MSG;
            TEST_LOG("PID query cmd=%s: FAIL state=%d (%s) reason=busy",
                     pidCommand.c_str(),
                     (int)stateOut,
                     ElmStateToString(stateOut));
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    myELM327.flushInputBuff();
    stateOut = myELM327.sendCommand_Blocking(pidCommand.c_str());
    if (myELM327.payload != nullptr) {
        payloadOut = String(myELM327.payload);
    }
    UnlockObdQuery();

    if (stateOut == ELM_SUCCESS) {
        TEST_LOG("PID query cmd=%s: OK payload=%s",
                 pidCommand.c_str(),
                 payloadOut.c_str());
        return true;
    }

    TEST_LOG("PID query cmd=%s: FAIL state=%d (%s) payload=%s",
             pidCommand.c_str(),
             (int)stateOut,
             ElmStateToString(stateOut),
             payloadOut.c_str());
    return false;
#endif
}

bool ObdMgr::QueryPidRawWithHeader(const String& header, const String& pidCommand, String& payloadOut, int8_t& stateOut)
{
    payloadOut = "";
    stateOut = ELM_GENERAL_ERROR;

#ifdef OBD_SIMUL_MODE
    payloadOut = "SIM";
    stateOut = ELM_SUCCESS;
    return true;
#else
    if (header.length() == 0 || pidCommand.length() < 4) {
        return false;
    }
    if (GetOBDStatus() != OBD_CONNECTED) {
        stateOut = ELM_UNABLE_TO_CONNECT;
        return false;
    }

    const TickType_t lockTimeout = pdMS_TO_TICKS(1600);
    TickType_t lockStart = xTaskGetTickCount();
    while (!TryLockObdQuery()) {
        if ((xTaskGetTickCount() - lockStart) >= lockTimeout) {
            stateOut = ELM_GETTING_MSG;
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    char setHeaderCmd[20] = {0};
    snprintf(setHeaderCmd, sizeof(setHeaderCmd), "AT SH %s", header.c_str());
    int8_t headerState = myELM327.sendCommand_Blocking(setHeaderCmd);
    if (headerState != ELM_SUCCESS) {
        stateOut = headerState;
        if (myELM327.payload != nullptr) payloadOut = String(myELM327.payload);
        myELM327.sendCommand_Blocking("AT SH 7DF");
        UnlockObdQuery();
        return false;
    }

    myELM327.flushInputBuff();
    stateOut = myELM327.sendCommand_Blocking(pidCommand.c_str());
    if (myELM327.payload != nullptr) {
        payloadOut = String(myELM327.payload);
    }
    myELM327.sendCommand_Blocking("AT SH 7DF");
    UnlockObdQuery();
    return stateOut == ELM_SUCCESS;
#endif
}

bool ObdMgr::QueryPidRawWithProtocolHeader(const char* protocol,
                                           const String& header,
                                           const String& pidCommand,
                                           String& payloadOut,
                                           int8_t& stateOut)
{
    payloadOut = "";
    stateOut = ELM_GENERAL_ERROR;

#ifdef OBD_SIMUL_MODE
    payloadOut = "SIM";
    stateOut = ELM_SUCCESS;
    return true;
#else
    if (!protocol || protocol[0] == '\0' || header.length() == 0 || pidCommand.length() < 4) {
        return false;
    }
    if (GetOBDStatus() != OBD_CONNECTED) {
        stateOut = ELM_UNABLE_TO_CONNECT;
        return false;
    }

    const TickType_t lockTimeout = pdMS_TO_TICKS(1600);
    TickType_t lockStart = xTaskGetTickCount();
    while (!TryLockObdQuery()) {
        if ((xTaskGetTickCount() - lockStart) >= lockTimeout) {
            stateOut = ELM_GETTING_MSG;
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    char setProtocolCmd[12] = {0};
    snprintf(setProtocolCmd, sizeof(setProtocolCmd), "AT SP %s", protocol);
    int8_t protocolState = myELM327.sendCommand_Blocking(setProtocolCmd);
    if (protocolState != ELM_SUCCESS) {
        stateOut = protocolState;
        if (myELM327.payload != nullptr) payloadOut = String(myELM327.payload);
        myELM327.sendCommand_Blocking("AT SP 6");
        myELM327.sendCommand_Blocking("AT SH 7DF");
        UnlockObdQuery();
        return false;
    }

    char setHeaderCmd[20] = {0};
    snprintf(setHeaderCmd, sizeof(setHeaderCmd), "AT SH %s", header.c_str());
    int8_t headerState = myELM327.sendCommand_Blocking(setHeaderCmd);
    if (headerState != ELM_SUCCESS) {
        stateOut = headerState;
        if (myELM327.payload != nullptr) payloadOut = String(myELM327.payload);
        myELM327.sendCommand_Blocking("AT SP 6");
        myELM327.sendCommand_Blocking("AT SH 7DF");
        UnlockObdQuery();
        return false;
    }

    myELM327.flushInputBuff();
    stateOut = myELM327.sendCommand_Blocking(pidCommand.c_str());
    if (myELM327.payload != nullptr) {
        payloadOut = String(myELM327.payload);
    }

    myELM327.sendCommand_Blocking("AT SP 6");
    myELM327.sendCommand_Blocking("AT SH 7DF");
    UnlockObdQuery();
    return stateOut == ELM_SUCCESS;
#endif
}

bool ObdMgr::QueryPidValueWithHeader(const String& header,
                                     uint8_t service,
                                     uint16_t pid,
                                     uint8_t numExpectedBytes,
                                     double scaleFactor,
                                     double& valueOut,
                                     String& payloadOut,
                                     int8_t& stateOut)
{
    valueOut = 0.0;
    payloadOut = "";
    stateOut = ELM_GENERAL_ERROR;

#ifdef OBD_SIMUL_MODE
    stateOut = ELM_NO_DATA;
    return false;
#else
    if (GetOBDStatus() != OBD_CONNECTED) {
        stateOut = ELM_UNABLE_TO_CONNECT;
        return false;
    }

    const TickType_t lockTimeout = pdMS_TO_TICKS(1600);
    TickType_t lockStart = xTaskGetTickCount();
    while (!TryLockObdQuery()) {
        if ((xTaskGetTickCount() - lockStart) >= lockTimeout) {
            stateOut = ELM_GETTING_MSG;
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    char setHeaderCmd[20] = {0};
    snprintf(setHeaderCmd, sizeof(setHeaderCmd), "AT SH %s", header.c_str());
    int8_t headerState = myELM327.sendCommand_Blocking(setHeaderCmd);
    if (headerState != ELM_SUCCESS) {
        stateOut = headerState;
        if (myELM327.payload != nullptr) payloadOut = String(myELM327.payload);
        myELM327.sendCommand_Blocking("AT SH 7DF");
        UnlockObdQuery();
        return false;
    }

    bool oldSpecifyResponses = myELM327.specifyNumResponses;
    myELM327.specifyNumResponses = false;
    uint32_t startMs = millis();
    while (true) {
        valueOut = myELM327.processPID(service, pid, 1, numExpectedBytes, scaleFactor);
        stateOut = myELM327.nb_rx_state;
        if (stateOut == ELM_SUCCESS) {
            break;
        }
        if (stateOut != ELM_GETTING_MSG) {
            break;
        }
        if ((millis() - startMs) >= ELM_PID_TIMEOUT_MS) {
            stateOut = ELM_TIMEOUT;
            myELM327.nb_rx_state = ELM_TIMEOUT;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(ELM_QUERY_WAIT_INTERVAL_MS));
    }
    myELM327.specifyNumResponses = oldSpecifyResponses;
    if (myELM327.payload != nullptr) {
        payloadOut = String(myELM327.payload);
    }
    myELM327.sendCommand_Blocking("AT SH 7DF");
    UnlockObdQuery();
    return stateOut == ELM_SUCCESS;
#endif
}

void ObdMgr::PrintOdometerProbe(void)
{
#ifdef OBD_SIMUL_MODE
    Serial.println("[odo-probe] simulated mode");
#else
    struct RawProbe {
        const char* header;
        const char* command;
    };

    const RawProbe rawProbes[] = {
        {"7C6", "22B002"},
        {"7C0", "22B002"},
        {"7C4", "22B002"},
        {"7D1", "22B002"},
        {"7B3", "22B002"},
        {"7E0", "22B002"},
        {"7E1", "22B002"},
        {"7E0", "22295A"},
        {"7E1", "22295A"},
        {"7C6", "22295A"},
        {"7E0", "22F4A6"},
        {"7E1", "22F4A6"},
        {"7C6", "22F4A6"}
    };

    Serial.println("[odo-probe] raw begin");
    for (size_t i = 0; i < sizeof(rawProbes) / sizeof(rawProbes[0]); ++i) {
        String payload;
        int8_t state = ELM_GENERAL_ERROR;
        bool ok = QueryPidRawWithHeader(rawProbes[i].header, rawProbes[i].command, payload, state);
        Serial.printf("[odo-probe][raw] header=%s cmd=%s ok=%d state=%d payload=%s\n",
                      rawProbes[i].header,
                      rawProbes[i].command,
                      ok ? 1 : 0,
                      (int)state,
                      payload.c_str());
        vTaskDelay(pdMS_TO_TICKS(80));
    }

    struct ProtocolProbe {
        const char* protocol;
        const char* header;
        const char* command;
    };

    const ProtocolProbe protocolProbes[] = {
        {"7", "17FC0076", "22295A"},
        {"7", "17FC0076", "22B002"},
        {"7", "17FC007B", "22F40D"}
    };

    Serial.println("[odo-probe] protocol raw begin");
    for (size_t i = 0; i < sizeof(protocolProbes) / sizeof(protocolProbes[0]); ++i) {
        String payload;
        int8_t state = ELM_GENERAL_ERROR;
        bool ok = QueryPidRawWithProtocolHeader(protocolProbes[i].protocol,
                                                protocolProbes[i].header,
                                                protocolProbes[i].command,
                                                payload,
                                                state);
        Serial.printf("[odo-probe][proto] protocol=%s header=%s cmd=%s ok=%d state=%d payload=%s\n",
                      protocolProbes[i].protocol,
                      protocolProbes[i].header,
                      protocolProbes[i].command,
                      ok ? 1 : 0,
                      (int)state,
                      payload.c_str());
        vTaskDelay(pdMS_TO_TICKS(80));
    }

    struct ValueProbe {
        const char* header;
        uint8_t service;
        uint16_t pid;
        uint8_t bytes;
        double scale;
        const char* name;
    };

    const ValueProbe valueProbes[] = {
        {"7C6", 0x22, 0xB002, 3, 1.0, "22B002/u24"},
        {"7E0", 0x22, 0x295A, 3, 1.0, "22295A/u24"},
        {"7E1", 0x22, 0x295A, 3, 1.0, "22295A/u24"},
        {"7E0", 0x22, 0xF4A6, 4, 0.1, "22F4A6/u32x0.1"},
        {"7DF", 0x01, 0x00A6, 4, 0.1, "01A6/u32x0.1"}
    };

    Serial.println("[odo-probe] processPID begin");
    for (size_t i = 0; i < sizeof(valueProbes) / sizeof(valueProbes[0]); ++i) {
        double value = 0.0;
        String payload;
        int8_t state = ELM_GENERAL_ERROR;
        bool ok = QueryPidValueWithHeader(valueProbes[i].header,
                                          valueProbes[i].service,
                                          valueProbes[i].pid,
                                          valueProbes[i].bytes,
                                          valueProbes[i].scale,
                                          value,
                                          payload,
                                          state);
        Serial.printf("[odo-probe][pidv] header=%s name=%s ok=%d state=%d value=%.1f payload=%s\n",
                      valueProbes[i].header,
                      valueProbes[i].name,
                      ok ? 1 : 0,
                      (int)state,
                      value,
                      payload.c_str());
        vTaskDelay(pdMS_TO_TICKS(80));
    }
    Serial.println("[odo-probe] done");
#endif
}

void ObdMgr::PrintCanOdometerProbe(uint32_t targetKm, uint32_t durationMs)
{
#ifdef OBD_SIMUL_MODE
    Serial.println("[can-probe] simulated mode");
#else
    if (targetKm == 0U) {
        Serial.println("[can-probe] target must be > 0");
        return;
    }
    if (GetOBDStatus() != OBD_CONNECTED) {
        Serial.println("[can-probe] OBD not connected");
        return;
    }

    const TickType_t lockTimeout = pdMS_TO_TICKS(1600);
    TickType_t lockStart = xTaskGetTickCount();
    while (!TryLockObdQuery()) {
        if ((xTaskGetTickCount() - lockStart) >= lockTimeout) {
            Serial.println("[can-probe] OBD busy");
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    uint32_t targetX10 = targetKm * 10U;
    char raw24[7] = {0};
    char raw32[9] = {0};
    char raw24Le[7] = {0};
    char raw32Le[9] = {0};
    char x10_24[7] = {0};
    char x10_32[9] = {0};
    char x10_24Le[7] = {0};
    char x10_32Le[9] = {0};
    snprintf(raw24, sizeof(raw24), "%06lX", (unsigned long)(targetKm & 0xFFFFFFU));
    snprintf(raw32, sizeof(raw32), "%08lX", (unsigned long)targetKm);
    FormatLittleEndian24(targetKm, raw24Le, sizeof(raw24Le));
    FormatLittleEndian32(targetKm, raw32Le, sizeof(raw32Le));
    snprintf(x10_24, sizeof(x10_24), "%06lX", (unsigned long)(targetX10 & 0xFFFFFFU));
    snprintf(x10_32, sizeof(x10_32), "%08lX", (unsigned long)targetX10);
    FormatLittleEndian24(targetX10, x10_24Le, sizeof(x10_24Le));
    FormatLittleEndian32(targetX10, x10_32Le, sizeof(x10_32Le));

    Serial.printf("[can-probe] begin target=%lu patterns=%s,%s,%s,%s,%s,%s,%s,%s duration_ms=%lu\n",
                  (unsigned long)targetKm,
                  raw24,
                  raw32,
                  raw24Le,
                  raw32Le,
                  x10_24,
                  x10_32,
                  x10_24Le,
                  x10_32Le,
                  (unsigned long)durationMs);

    myELM327.sendCommand_Blocking("AT H1");
    myELM327.sendCommand_Blocking("AT S0");
    myELM327.sendCommand_Blocking("AT CAF0");
    myELM327.flushInputBuff();

    Stream* port = myELM327.elm_port;
    if (!port) {
        Serial.println("[can-probe] ELM stream unavailable");
        myELM327.sendCommand_Blocking("AT CAF1");
        myELM327.sendCommand_Blocking("AT H0");
        myELM327.sendCommand_Blocking("AT SH 7DF");
        UnlockObdQuery();
        return;
    }

    port->print("AT MA\r");
    uint32_t startMs = millis();
    uint32_t lineCount = 0;
    uint32_t matchCount = 0;
    uint32_t sampleCount = 0;
    String line;
    line.reserve(96);

    while ((millis() - startMs) < durationMs) {
        while (port->available() > 0) {
            char c = (char)port->read();
            if (c == '\r' || c == '\n' || c == '>') {
                if (line.length() > 0) {
                    lineCount++;
                    String normalized = NormalizeHexLine(line);
                    bool match =
                        normalized.indexOf(raw24) >= 0 ||
                        normalized.indexOf(raw32) >= 0 ||
                        normalized.indexOf(raw24Le) >= 0 ||
                        normalized.indexOf(raw32Le) >= 0 ||
                        normalized.indexOf(x10_24) >= 0 ||
                        normalized.indexOf(x10_32) >= 0 ||
                        normalized.indexOf(x10_24Le) >= 0 ||
                        normalized.indexOf(x10_32Le) >= 0;
                    if (match) {
                        matchCount++;
                        Serial.printf("[can-probe][match] raw=%s norm=%s\n",
                                      line.c_str(),
                                      normalized.c_str());
                    } else if (sampleCount < 20U) {
                        sampleCount++;
                        Serial.printf("[can-probe][sample] raw=%s norm=%s\n",
                                      line.c_str(),
                                      normalized.c_str());
                    }
                    line = "";
                }
            } else if (line.length() < 95U) {
                line += c;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    port->print('\r');
    uint32_t stopStart = millis();
    while ((millis() - stopStart) < 800U) {
        while (port->available() > 0) {
            (void)port->read();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    myELM327.sendCommand_Blocking("AT CAF1");
    myELM327.sendCommand_Blocking("AT H0");
    myELM327.sendCommand_Blocking("AT SH 7DF");
    Serial.printf("[can-probe] done lines=%lu matches=%lu\n",
                  (unsigned long)lineCount,
                  (unsigned long)matchCount);
    UnlockObdQuery();
#endif
}

void ObdMgr::QueryOBDData(void *param)
{
    TEST_LINE();
    ObdMgr* self = static_cast<ObdMgr*>(param);
    SystemAPI* system = SystemAPI::getInstance();
    if (self == NULL || system == NULL)
    {
        vTaskDelete(NULL);
        return;
    }

    ObdData data = {};
    unsigned long now = millis();
    unsigned long last_odo_time = now;
    // Run non-odometer query once immediately after OBD task starts.
    unsigned long last_query_time = now - 60000;
    unsigned long last_diag_log_time = now;
    bool sessionOdoSuccess = false;

    while (true)
    {
        unsigned long current_time = millis();

        if (current_time - last_odo_time >= ODO_QUERY_INTERVAL_MS)
        {
            bool odoRetryTriggered = false;
            if (self->TryLockObdQuery())
            {
                self->QueryOdometer(data.odometer_km);
                int8_t odoState = self->myELM327.nb_rx_state;

                if (IsObdLinkLostState(odoState)) {
                    odoRetryTriggered = true;
                    for (uint8_t retry = 0; retry < ODO_LINK_LOST_RETRY_MAX; ++retry) {
                        TEST_LOG("odo retry %u/%u after state=%d (%s)",
                                 (unsigned int)(retry + 1),
                                 (unsigned int)ODO_LINK_LOST_RETRY_MAX,
                                 (int)odoState,
                                 ElmStateToString(odoState));
                        self->QueryOdometer(data.odometer_km);
                        odoState = self->myELM327.nb_rx_state;
                        if (!IsObdLinkLostState(odoState)) {
                            break;
                        }
                    }
                }
                {
                    uint32_t elapsedSec = (millis() - self->_bootMs) / 1000U;
                    self->SetDriveTimeSec(elapsedSec);
                }

                if (odoState == ELM_SUCCESS) {
                    self->SetOdometerKm(data.odometer_km);
                    if (!self->_odometerStartValid || data.odometer_km < self->_odometerStartKm) {
                        self->_odometerStartValid = true;
                        self->_odometerStartKm = data.odometer_km;
                    }
                    self->_odometerLastKm = data.odometer_km;
                    if (self->_odometerStartValid) {
                        uint32_t trip = (self->_odometerLastKm >= self->_odometerStartKm)
                                            ? (self->_odometerLastKm - self->_odometerStartKm)
                                            : 0;
                        self->SetTripDistanceKm(trip);
                    }
                    system->UpdateServiceOdoFromCurrentOdo(data.odometer_km);

                    if (!sessionOdoSuccess ||
                        self->_awaitingOdometerRecovery ||
                        self->GetOBDStatus() != OBD_CONNECTED) {
                        sessionOdoSuccess = true;
                        self->PostEvent(OBD_MGR_EVENT_ODOMETER_SUCCESS);
                    }
                } else if (IsObdLinkLostState(odoState)) {
                    LogPidError("0131", odoState, "link-lost");
                    self->UnlockObdQuery();
                    self->HandleQueryLinkLoss(system, "odometer");
                    vTaskDelete(NULL);
                    return;
                }
                self->UnlockObdQuery();
            }
            last_odo_time = current_time;
            if (odoRetryTriggered) {
                // When in ODO retry mode, skip all non-ODO PID queries in this cycle.
                vTaskDelay(pdMS_TO_TICKS(ELM_QUERY_WAIT_INTERVAL_MS));
                continue;
            }
        }

        if ((current_time - last_diag_log_time) >= OBD_DIAG_LOG_INTERVAL_MS) {
            UBaseType_t hwmWords = uxTaskGetStackHighWaterMark(NULL);
            TEST_LOG("OBD diag: stack_hwm=%u words, odo_state=%d, query_interval=%lu ms",
                     (unsigned int)hwmWords,
                     (int)self->myELM327.nb_rx_state,
                     (unsigned long)ODO_QUERY_INTERVAL_MS);
            last_diag_log_time = current_time;
        }

        if (current_time - last_query_time >= 5150)
        {
            if (self->TryLockObdQuery())
            {
                self->QueryVoltage(data.voltage);
                int8_t voltageState = self->myELM327.nb_rx_state;
                if (voltageState == ELM_SUCCESS) {
                    self->SetVoltageLevel(data.voltage);
                }

                self->QueryCoolant(data.coolant);
                int8_t coolantState = self->myELM327.nb_rx_state;
                if (coolantState == ELM_SUCCESS) {
                    self->SetCoolantTemp(data.coolant);
                }
                self->UnlockObdQuery();
            }

            float outsideTemp = 0.0f;
            if (self->QueryOutsideTemp(outsideTemp)) {
                self->SetOutsideTemp((int16_t)outsideTemp, true);
            }
            last_query_time = current_time;
        }

        if (!system->GetOBDConnected())
        {
            Serial.println("[ObdMgr] BLE link lost. Terminating OBD query task.");
            self->HandleQueryLinkLoss(system, "ble-disconnect");
            vTaskDelete(NULL);
            return;
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void ObdMgr::SetOBDStatus(int status)
{
    if (LockData(pdMS_TO_TICKS(20))) {
        this->obd_status = status;
        UnlockData();
    } else {
        this->obd_status = status;
    }

    SystemAPI* system = SystemAPI::getInstance();
    if (system) {
        system->PublishObdStatus(status);
        if (status != OBD_CONNECTED) {
            system->PublishObdOutsideTemp(0, false);
        }
    }
}

int ObdMgr::GetOBDStatus()
{
    int status = BT_INIT_FAILED;
    if (LockData(pdMS_TO_TICKS(20))) {
        status = this->obd_status;
        UnlockData();
    } else {
        status = this->obd_status;
    }
    return status;
}

void ObdMgr::ResetServiceOdoSessionBase(void)
{
    if (_odometerLastKm > 0 || _odometerStartValid) {
        _odometerStartKm = _odometerLastKm;
        _odometerStartValid = true;
    } else {
        _odometerStartKm = 0;
        _odometerStartValid = false;
    }
}

void ObdMgr::SetRPM(unsigned short rpmValue)
{
    if (LockData(pdMS_TO_TICKS(20))) {
        this->obd_data.rpm = rpmValue;
        UnlockData();
    } else {
        this->obd_data.rpm = rpmValue;
    }
}

void ObdMgr::SetOdometerKm(uint32_t odometerKm)
{
    if (LockData(pdMS_TO_TICKS(20))) {
        this->obd_data.odometer_km = odometerKm;
        UnlockData();
    } else {
        this->obd_data.odometer_km = odometerKm;
    }
}

void ObdMgr::SetTripDistanceKm(uint32_t tripKm)
{
    if (LockData(pdMS_TO_TICKS(20))) {
        this->obd_data.trip_distance_km = tripKm;
        UnlockData();
    } else {
        this->obd_data.trip_distance_km = tripKm;
    }
}

void ObdMgr::SetDriveTimeSec(uint32_t seconds)
{
    if (LockData(pdMS_TO_TICKS(20))) {
        this->obd_data.drive_time_sec = seconds;
        UnlockData();
    } else {
        this->obd_data.drive_time_sec = seconds;
    }
}

uint32_t ObdMgr::GetOdometerKm(void)
{
    uint32_t value = 0;
    if (LockData(pdMS_TO_TICKS(20))) {
        value = this->obd_data.odometer_km;
        UnlockData();
    } else {
        value = this->obd_data.odometer_km;
    }
    return value;
}

uint32_t ObdMgr::GetTripDistanceKm(void)
{
    uint32_t value = 0;
    if (LockData(pdMS_TO_TICKS(20))) {
        value = this->obd_data.trip_distance_km;
        UnlockData();
    } else {
        value = this->obd_data.trip_distance_km;
    }
    return value;
}

uint32_t ObdMgr::GetDriveTimeSec(void)
{
    uint32_t value = 0;
    if (LockData(pdMS_TO_TICKS(20))) {
        value = this->obd_data.drive_time_sec;
        UnlockData();
    } else {
        value = this->obd_data.drive_time_sec;
    }
    return value;
}

void ObdMgr::SetVoltageLevel(unsigned short volt)
{
    if (LockData(pdMS_TO_TICKS(20))) {
        this->obd_data.voltage = volt;
        UnlockData();
    } else {
        this->obd_data.voltage = volt;
    }

    SystemAPI* system = SystemAPI::getInstance();
    if (system) {
        system->PublishObdBatteryVoltage(volt);
    }
}

void ObdMgr::SetCoolantTemp(unsigned short temp)
{
    if (LockData(pdMS_TO_TICKS(20))) {
        this->obd_data.coolant = temp;
        UnlockData();
    } else {
        this->obd_data.coolant = temp;
    }

    SystemAPI* system = SystemAPI::getInstance();
    if (system) {
        system->PublishObdCoolant(temp);
    }
}

void ObdMgr::SetOutsideTemp(int16_t tempC, bool valid)
{
    if (LockData(pdMS_TO_TICKS(20))) {
        this->obd_data.outside_temp = tempC;
        this->obd_data.outside_temp_valid = valid;
        UnlockData();
    } else {
        this->obd_data.outside_temp = tempC;
        this->obd_data.outside_temp_valid = valid;
    }

    SystemAPI* system = SystemAPI::getInstance();
    if (system) {
        system->PublishObdOutsideTemp(tempC, valid);
    }
}

void ObdMgr::SetMafRate(float val)
{
    if (LockData(pdMS_TO_TICKS(20))) {
        this->obd_data.maf_rate = val;
        UnlockData();
    } else {
        this->obd_data.maf_rate = val;
    }
}

ObdData ObdMgr::GetObdData(void)
{
    ObdData snapshot = {};
    if (LockData(pdMS_TO_TICKS(20))) {
        snapshot = this->obd_data;
        UnlockData();
    } else {
        snapshot = this->obd_data;
    }
    return snapshot;
}

void ObdMgr::QueryDistAfterErrorClear(uint16_t &dist_value)
{
#ifdef OBD_SIMUL_MODE
    dist_value = rand() % 1000;
    TEST_LOG("pid 0131 ok value=%u km [sim]", (unsigned int)dist_value);
#else
    float dist = myELM327.distSinceCodesCleared();
    int8_t finalState = myELM327.nb_rx_state;
    if (finalState == ELM_SUCCESS) {
        dist_value = (uint16_t)dist;
        TEST_LOG("pid 0131 ok value=%u km", (unsigned int)dist_value);
    } else {
        dist_value = 0;
        LogPidError("0131", finalState);
    }
#endif
}
