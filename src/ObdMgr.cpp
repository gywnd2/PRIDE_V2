#include <ObdMgr.h>
#include <CommonApi.h>
#include <string.h>

#define TEST_LOG(fmt, ...) Serial.printf("[ObdMgr] " fmt "\n", ##__VA_ARGS__)
#define TEST_LINE() Serial.printf("[ObdMgr] %s\n", __func__)

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
        1
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
                bool wasStableSession = self->_hadPidSuccess;
                self->SetOBDStatus(OBD_DISCONNECTED);
                self->_awaitingRpmRecovery = wasStableSession;
                system->btSubscriber.SetEvent(BT_REQUEST_DISCONNECT);

                if (wasStableSession) {
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
    if (bleStream != nullptr)
    {
        self->myELM327.begin(*bleStream, true, 2000);
    }

    int retry_count = 0;
    while (!self->myELM327.connected)
    {
        retry_count++;
        if (retry_count > 10)
        {
            self->SetOBDStatus(OBD_INIT_FAILED);
            system->btSubscriber.SetEvent(BT_REQUEST_DISCONNECT);
            self->_connectTaskRunning = false;
            self->_connectTask = NULL;
            vTaskDelay(pdMS_TO_TICKS(OBD_RECONNECT_INTERVAL_MS));
            self->PostEvent(OBD_MGR_EVENT_START_CONNECT);
            vTaskDelete(NULL);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
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
#else
    while (true)
    {
        float coolant = myELM327.engineCoolantTemp();
        if (myELM327.nb_rx_state == ELM_SUCCESS)
        {
            coolant_temp = (uint16_t)coolant;
            break;
        }
        else if (myELM327.nb_rx_state == ELM_NO_DATA || myELM327.nb_rx_state == ELM_GENERAL_ERROR)
        {
            coolant_temp = OBD_QUERY_INVALID_RESPONSE;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
#endif
}

void ObdMgr::QueryVoltage(uint16_t &voltage_level)
{
#ifdef OBD_SIMUL_MODE
    vTaskDelay(pdMS_TO_TICKS(OBD_SIMUL_QUERY_TIME));
    voltage_level = rand() % (18 - 6 + 1) + 6;
#else
    while (true)
    {
        float voltage = myELM327.batteryVoltage();
        if (myELM327.nb_rx_state == ELM_SUCCESS)
        {
            voltage_level = (uint16_t)voltage;
            break;
        }
        else if (myELM327.nb_rx_state == ELM_NO_DATA)
        {
            voltage_level = OBD_QUERY_INVALID_RESPONSE;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
#endif
}

void ObdMgr::QueryRPM(uint16_t &rpm_value)
{
    int rpm_retry_count = 0;
#ifdef OBD_SIMUL_MODE
    vTaskDelay(pdMS_TO_TICKS(OBD_SIMUL_QUERY_TIME));
    rpm_value = rand() % (6000 - 1000 + 1) + 1000;
#else
    while (true)
    {
        float rpm = myELM327.rpm();
        if (myELM327.nb_rx_state == ELM_SUCCESS)
        {
            rpm_value = (uint16_t)rpm;
            break;
        }
        else if (myELM327.nb_rx_state == ELM_NO_DATA)
        {
            if (rpm_retry_count >= RPM_REQ_RETRY_MAX)
            {
                rpm_value = OBD_QUERY_INVALID_RESPONSE;
                SetOBDStatus(OBD_DISCONNECTED);
                break;
            }
            rpm_retry_count++;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
#endif
}

bool ObdMgr::QueryOutsideTemp(float& outsideTempC)
{
#ifdef OBD_SIMUL_MODE
    outsideTempC = (float)(rand() % 40) - 10.0f;
    return true;
#else
    outsideTempC = 0.0f;
    if (GetOBDStatus() != OBD_CONNECTED) return false;
    if (!TryLockObdQuery()) return false;

    bool success = false;
    const int maxAttempts = 40; // ~2s at 50ms intervals
    for (int i = 0; i < maxAttempts; ++i) {
        outsideTempC = myELM327.ambientAirTemp();
        if (myELM327.nb_rx_state == ELM_SUCCESS) {
            success = true;
            break;
        }
        if (myELM327.nb_rx_state != ELM_GETTING_MSG) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    UnlockObdQuery();
    return success;
#endif
}

void ObdMgr::QueryOBDData(void *param)
{
    TEST_LINE();
    ObdMgr* self = static_cast<ObdMgr*>(param);
    if (self == NULL)
    {
        vTaskDelete(NULL);
        return;
    }

    ObdData data;
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
                if (data.rpm != OBD_QUERY_INVALID_RESPONSE) {
                    self->SetRPM(data.rpm);
                    if (!sessionRpmSuccess) {
                        sessionRpmSuccess = true;
                        self->PostEvent(OBD_MGR_EVENT_RPM_SUCCESS);
                    }
                }
                self->UnlockObdQuery();
            }
            last_rpm_time = current_time;
        }

        if (current_time - last_query_time >= 28150)
        {
            if (self->TryLockObdQuery())
            {
                self->QueryVoltage(data.voltage);
                self->SetVoltageLevel(data.voltage);
                self->QueryCoolant(data.coolant);
                self->SetCoolantTemp(data.coolant);
                self->QueryDistAfterErrorClear(data.distance);
                self->SetDistance(data.distance);
                self->UnlockObdQuery();
            }
            last_query_time = current_time;
        }

        if (self->GetOBDStatus() == OBD_DISCONNECTED)
        {
            Serial.println("[ObdMgr] OBD Disconnected. Terminating Task.");
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
#else
    float dist = myELM327.distSinceCodesCleared();
    if (myELM327.nb_rx_state == ELM_SUCCESS) dist_value = (uint16_t)dist;
    else dist_value = 0;
#endif
}
