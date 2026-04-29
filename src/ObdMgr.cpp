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
static int8_t s_lastOdoLogState = ELM_GENERAL_ERROR;
static constexpr uint16_t ELM_INIT_TIMEOUT_MS = 600;
static constexpr uint16_t ELM_PID_TIMEOUT_MS = 1500;
static constexpr uint16_t ELM_QUERY_WAIT_INTERVAL_MS = 30;
static constexpr char ELM_INIT_PROTOCOL = ISO_15765_11_BIT_500_KBAUD;
static constexpr uint32_t ODO_QUERY_INTERVAL_MS = 1000;
static constexpr uint8_t ODO_LINK_LOST_RETRY_MAX = 2;
static constexpr uint32_t OBD_DIAG_LOG_INTERVAL_MS = 30000;

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
    if (!system || !_odometerStartValid) return;
    if (_odometerLastKm < _odometerStartKm) return;

    uint32_t deltaKm = _odometerLastKm - _odometerStartKm;
    if (deltaKm == 0) return;

    uint32_t totalKm = 0;
    if (system->AddServiceOdoDistance(deltaKm, &totalKm)) {
        TEST_LOG("service odo finalized start=%u end=%u delta=%u total=%u",
                 (unsigned int)_odometerStartKm,
                 (unsigned int)_odometerLastKm,
                 (unsigned int)deltaKm,
                 (unsigned int)totalKm);
        _odometerStartKm = _odometerLastKm;
    } else {
        TEST_LOG("service odo finalize failed start=%u end=%u delta=%u",
                 (unsigned int)_odometerStartKm,
                 (unsigned int)_odometerLastKm,
                 (unsigned int)deltaKm);
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
        TEST_LOG("pid 0131 ok value=%u km", (unsigned int)odometer_km);
    } else {
        LogPidError("0131", finalState);
    }
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
                        TEST_LOG("pid 0131 retry %u/%u after state=%d (%s)",
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
