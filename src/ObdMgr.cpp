#include <ObdMgr.h>
#include <CommonApi.h>
#include <string.h>

#define TEST_LOG(fmt, ...) Serial.printf("[ObdMgr] " fmt "\n", ##__VA_ARGS__)
#define TEST_LINE() Serial.printf("[ObdMgr] %s\n", __func__)

#ifndef OBD_RPM_STATE_FILE_LOG_ENABLE
#define OBD_RPM_STATE_FILE_LOG_ENABLE 1
#endif

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
static uint32_t s_lastRpmLogMs = 0;
static int8_t s_lastRpmLogState = ELM_GENERAL_ERROR;
static constexpr uint16_t ELM_INIT_TIMEOUT_MS = 600;
static constexpr char ELM_INIT_PROTOCOL = ISO_15765_11_BIT_500_KBAUD;

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

bool ShouldLogRpm(int8_t state)
{
    const uint32_t now = millis();
    const uint32_t minIntervalMs = (state == ELM_SUCCESS) ? 5000U : 1500U;
    if (state != s_lastRpmLogState || (now - s_lastRpmLogMs) >= minIntervalMs) {
        s_lastRpmLogState = state;
        s_lastRpmLogMs = now;
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
    _awaitingRpmRecovery = false;
    _goodbyeScreenActive = false;

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
                self->_awaitingRpmRecovery = true;
                system->btSubscriber.SetEvent(BT_REQUEST_DISCONNECT);

                if (!self->_goodbyeScreenActive) {
                    system->displaySubscriber.SetEvent(DISPLAY_SHOW_GOODBYE);
                    system->soundSubscriber.SetEvent(SOUND_PLAY_TRACK, 2);
                    self->_goodbyeScreenActive = true;
                }

                vTaskDelay(pdMS_TO_TICKS(OBD_RECONNECT_INTERVAL_MS));
                self->PostEvent(OBD_MGR_EVENT_START_CONNECT);
                break;
            }

            case OBD_MGR_EVENT_RPM_SUCCESS:
            {
                self->_hadPidSuccess = true;
                if (system->GetOBDConnected()) {
                    self->SetOBDStatus(OBD_CONNECTED);
                }
                if (self->_awaitingRpmRecovery) {
                    self->_awaitingRpmRecovery = false;
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
    TEST_LOG("Query coolant (PID 0105): OK value=%u C [sim]", (unsigned int)coolant_temp);
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
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (finalState == ELM_SUCCESS) {
        TEST_LOG("Query coolant (PID 0105): OK value=%u C", (unsigned int)coolant_temp);
    } else {
        TEST_LOG("Query coolant (PID 0105): FAIL state=%d (%s)",
                 (int)finalState, ElmStateToString(finalState));
    }
#endif
}

void ObdMgr::QueryVoltage(uint16_t &voltage_level)
{
#ifdef OBD_SIMUL_MODE
    vTaskDelay(pdMS_TO_TICKS(OBD_SIMUL_QUERY_TIME));
    voltage_level = rand() % (18 - 6 + 1) + 6;
    TEST_LOG("Query battery voltage (PID 0142): OK value=%u V [sim]", (unsigned int)voltage_level);
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
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (finalState == ELM_SUCCESS) {
        TEST_LOG("Query battery voltage (PID 0142): OK value=%.1f V", (double)finalVoltage);
    } else {
        TEST_LOG("Query battery voltage (PID 0142): FAIL state=%d (%s)",
                 (int)finalState, ElmStateToString(finalState));
    }
#endif
}

void ObdMgr::QueryRPM(uint16_t &rpm_value)
{
    int rpm_retry_count = 0;
#ifdef OBD_SIMUL_MODE
    vTaskDelay(pdMS_TO_TICKS(OBD_SIMUL_QUERY_TIME));
    rpm_value = rand() % (6000 - 1000 + 1) + 1000;
    if (ShouldLogRpm(ELM_SUCCESS)) {
        TEST_LOG("Query RPM (PID 010C): OK value=%u rpm [sim]", (unsigned int)rpm_value);
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
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (ShouldLogRpm(finalState)) {
        if (finalState == ELM_SUCCESS) {
            TEST_LOG("Query RPM (PID 010C): OK value=%u rpm", (unsigned int)rpm_value);
        } else {
            TEST_LOG("Query RPM (PID 010C): FAIL state=%d (%s)",
                     (int)finalState, ElmStateToString(finalState));
        }
    }
#endif
}

bool ObdMgr::QueryOutsideTemp(float& outsideTempC)
{
#ifdef OBD_SIMUL_MODE
    outsideTempC = (float)(rand() % 40) - 10.0f;
    TEST_LOG("Query outside temp (PID 0146): OK value=%.1f C [sim]", (double)outsideTempC);
    return true;
#else
    outsideTempC = 0.0f;
    if (GetOBDStatus() != OBD_CONNECTED) return false;
    if (!TryLockObdQuery()) return false;

    bool success = false;
    int8_t finalState = ELM_GENERAL_ERROR;
    const int maxAttempts = 40; // ~2s at 50ms intervals
    for (int i = 0; i < maxAttempts; ++i) {
        outsideTempC = myELM327.ambientAirTemp();
        finalState = myELM327.nb_rx_state;
        if (finalState == ELM_SUCCESS) {
            success = true;
            break;
        }
        if (finalState != ELM_GETTING_MSG) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    UnlockObdQuery();
    if (success) {
        TEST_LOG("Query outside temp (PID 0146): OK value=%.1f C", (double)outsideTempC);
    } else {
        TEST_LOG("Query outside temp (PID 0146): FAIL state=%d (%s)",
                 (int)finalState,
                 ElmStateToString(finalState));
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
    unsigned long last_rpm_time = now;
    // Run non-RPM query once immediately after OBD task starts.
    unsigned long last_query_time = now - 60000;
    bool sessionRpmSuccess = false;

    while (true)
    {
        unsigned long current_time = millis();

        if (current_time - last_rpm_time >= 500)
        {
            if (self->TryLockObdQuery())
            {
                self->QueryRPM(data.rpm);
                const int8_t rpmState = self->myELM327.nb_rx_state;
#if OBD_RPM_STATE_FILE_LOG_ENABLE
                {
                    char rpmLog[128] = {0};
                    if (rpmState == ELM_SUCCESS) {
                        snprintf(rpmLog, sizeof(rpmLog),
                                 "OBD RPM state=%d(%s) rpm=%u",
                                 (int)rpmState,
                                 ElmStateToString(rpmState),
                                 (unsigned int)data.rpm);
                    } else {
                        snprintf(rpmLog, sizeof(rpmLog),
                                 "OBD RPM state=%d(%s)",
                                 (int)rpmState,
                                 ElmStateToString(rpmState));
                    }
                    system->AppendStorageLog(rpmLog);
                }
#endif
                // RPM=0 is a valid value (engine idling/off). Use ELM state instead of value check.
                if (rpmState == ELM_SUCCESS) {
                    self->SetRPM(data.rpm);
                    if (!sessionRpmSuccess ||
                        self->_awaitingRpmRecovery ||
                        self->GetOBDStatus() != OBD_CONNECTED) {
                        sessionRpmSuccess = true;
                        self->PostEvent(OBD_MGR_EVENT_RPM_SUCCESS);
                    }
                } else if (IsObdLinkLostState(rpmState)) {
                    TEST_LOG("RPM error -> link lost handling state=%d (%s)",
                             (int)rpmState,
                             ElmStateToString(rpmState));
                    self->UnlockObdQuery();
                    self->_queryTaskRunning = false;
                    self->query_obd_data_task = NULL;
                    self->PostEvent(OBD_MGR_EVENT_LINK_LOST);
                    vTaskDelete(NULL);
                    return;
                }
                self->UnlockObdQuery();
            }
            last_rpm_time = current_time;
        }

        if (current_time - last_query_time >= 5150)
        {
            bool queryFatal = false;
            int8_t fatalState = ELM_GENERAL_ERROR;
            const char* fatalPid = "";

            if (self->TryLockObdQuery())
            {
                self->QueryVoltage(data.voltage);
                if (IsObdLinkLostState(self->myELM327.nb_rx_state)) {
                    queryFatal = true;
                    fatalState = self->myELM327.nb_rx_state;
                    fatalPid = "0142";
                } else {
                    self->SetVoltageLevel(data.voltage);
                    self->QueryCoolant(data.coolant);
                    if (IsObdLinkLostState(self->myELM327.nb_rx_state)) {
                        queryFatal = true;
                        fatalState = self->myELM327.nb_rx_state;
                        fatalPid = "0105";
                    } else {
                        self->SetCoolantTemp(data.coolant);
                    }
                }
                self->UnlockObdQuery();
            }

            if (queryFatal) {
                TEST_LOG("PID %s error -> link lost handling state=%d (%s)",
                         fatalPid,
                         (int)fatalState,
                         ElmStateToString(fatalState));
                self->_queryTaskRunning = false;
                self->query_obd_data_task = NULL;
                self->PostEvent(OBD_MGR_EVENT_LINK_LOST);
                vTaskDelete(NULL);
                return;
            }

            float outsideTemp = 0.0f;
            if (!queryFatal && self->QueryOutsideTemp(outsideTemp)) {
                self->SetOutsideTemp((int16_t)outsideTemp, true);
            } else if (IsObdLinkLostState(self->myELM327.nb_rx_state)) {
                TEST_LOG("PID 0146 error -> link lost handling state=%d (%s)",
                         (int)self->myELM327.nb_rx_state,
                         ElmStateToString(self->myELM327.nb_rx_state));
                self->_queryTaskRunning = false;
                self->query_obd_data_task = NULL;
                self->PostEvent(OBD_MGR_EVENT_LINK_LOST);
                vTaskDelete(NULL);
                return;
            }
            last_query_time = current_time;
        }

        if (!system->GetOBDConnected())
        {
            Serial.println("[ObdMgr] BLE link lost. Terminating OBD query task.");
            self->_queryTaskRunning = false;
            self->query_obd_data_task = NULL;
            self->PostEvent(OBD_MGR_EVENT_LINK_LOST);
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

void ObdMgr::SetRPM(unsigned short rpmValue)
{
    if (LockData(pdMS_TO_TICKS(20))) {
        this->obd_data.rpm = rpmValue;
        UnlockData();
    } else {
        this->obd_data.rpm = rpmValue;
    }
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

void ObdMgr::SetDistance(unsigned short dist)
{
    if (LockData(pdMS_TO_TICKS(20))) {
        this->obd_data.distance = dist;
        UnlockData();
    } else {
        this->obd_data.distance = dist;
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
    TEST_LOG("Query distance (PID 0131): OK value=%u km [sim]", (unsigned int)dist_value);
#else
    float dist = myELM327.distSinceCodesCleared();
    int8_t finalState = myELM327.nb_rx_state;
    if (finalState == ELM_SUCCESS) {
        dist_value = (uint16_t)dist;
        TEST_LOG("Query distance (PID 0131): OK value=%u km", (unsigned int)dist_value);
    } else {
        dist_value = 0;
        TEST_LOG("Query distance (PID 0131): FAIL state=%d (%s)",
                 (int)finalState, ElmStateToString(finalState));
    }
#endif
}
