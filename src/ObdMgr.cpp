#include <ObdMgr.h>
#include <CommonApi.h>

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

void ObdMgr::InitOBD(void)
{
    Serial.println("[ObdMgr] S3 BLE OBD task started");
    SetOBDStatus(BT_INIT_SUCCESS);
    vTaskDelay(pdMS_TO_TICKS(100));

    // Core 0에서 연결 태스크 실행
    xTaskCreatePinnedToCore(ConnectBTTask, "ConnectBTTask", 8192, this, 2, NULL, 0);
}

void ObdMgr::ConnectBTTask(void *param)
{
    ObdMgr* self = static_cast<ObdMgr*>(param);
    SystemAPI* system = SystemAPI::getInstance();

    if (self == NULL)
    {
        vTaskDelete(NULL);
        return;
    }

    Serial.println("[ObdMgr] Starting BLE Scan/Connect...");
    self->SetOBDStatus(BT_CONNECTING);

#ifndef OBD_SIMUL_MODE
    // BluetoothMgr에게 연결 요청
    system->ConnectOBD();

    // 연결 대기 (최대 20초)
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
        vTaskDelete(NULL);
        return;
    }

    Serial.println("\n[ObdMgr] BLE Connected. Initializing ELM327...");

    // SystemAPI에서 래핑된 Stream(NimBLEStream)을 가져옴
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
            vTaskDelete(NULL);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#endif

    self->SetOBDStatus(OBD_CONNECTED);
    // 데이터 쿼리 태스크는 Core 1에서 실행 (UI와 분리 고려)
    xTaskCreatePinnedToCore(QueryOBDData, "QueryOBDData", 4096, self, 3, &(self->query_obd_data_task), 1);
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

// ... (QueryDistAfterErrorClear, QueryMaf도 동일한 패턴으로 수정) ...

void ObdMgr::QueryOBDData(void *param)
{
    ObdMgr* self = static_cast<ObdMgr*>(param);
    if (self == NULL)
    {
        vTaskDelete(NULL);
        return;
    }

    ObdData data;
    unsigned long last_rpm_time = millis();
    unsigned long last_30sec_time = millis();

    while (true)
    {
        unsigned long current_time = millis();

        // 1. RPM 쿼리 (1초 주기)
        if (current_time - last_rpm_time >= 1000)
        {
            if (!self->obd_busy)
            {
                self->obd_busy = true;
                self->QueryRPM(data.rpm);
                self->SetRPM(data.rpm);
                self->obd_busy = false;
            }
            last_rpm_time = current_time;
        }

        // 2. 기타 정보 쿼리 (60초 주기 - 요청하신 60000ms 기준)
        if (current_time - last_30sec_time >= 60000)
        {
            if (!self->obd_busy)
            {
                self->obd_busy = true;
                self->QueryVoltage(data.voltage);
                self->SetVoltageLevel(data.voltage);
                self->QueryCoolant(data.coolant);
                self->SetCoolantTemp(data.coolant);
                self->QueryDistAfterErrorClear(data.distance);
                self->SetDistance(data.distance);
                self->obd_busy = false;
            }
            last_30sec_time = current_time;
        }

        // 연결 끊김 체크
        if (self->GetOBDStatus() == OBD_DISCONNECTED)
        {
            Serial.println("[ObdMgr] OBD Disconnected. Terminating Task.");
            vTaskDelete(NULL);
            return;
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void ObdMgr::SetOBDStatus(int status)
{
    this->obd_status = status;
}

int ObdMgr::GetOBDStatus()
{
    return this->obd_status;
}

void ObdMgr::SetRPM(unsigned short rpmValue)
{
    this->obd_data.rpm = rpmValue;
}

void ObdMgr::SetVoltageLevel(unsigned short volt)
{
    this->obd_data.voltage = volt;
}

void ObdMgr::SetCoolantTemp(unsigned short temp)
{
    this->obd_data.coolant = temp;
}

void ObdMgr::SetDistance(unsigned short dist)
{
    this->obd_data.distance = dist;
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