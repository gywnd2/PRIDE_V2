#ifndef __OBD__
#define __OBD__

#include <Arduino.h>
#include <ELMduino.h>
#include "esp_task_wdt.h"
#include <NimBLEDevice.h>

#define RPM_REQ_RETRY_MAX 2
#define BT_CONNECTION_CHECK_INTERVAL 1
#define OBD_SIMUL_QUERY_TIME 50  // 시뮬레이션 모드 시 대기 시간

struct ObdData
{
    uint16_t coolant;
    uint16_t voltage;
    uint16_t rpm;
    uint16_t distance;
    float maf_rate;
};

enum ObdStatus
{
    BT_INIT_FAILED = 0,
    BT_INIT_SUCCESS,
    BT_CONNECTING,
    BT_CONNECT_FAILED,
    OBD_INIT_FAILED,
    OBD_INIT_SUCCESS,
    OBD_CONNECTED,
    OBD_DISCONNECTED
};

enum ObdResponse
{
    OBD_QUERY_INVALID_RESPONSE = 0,
};

class ObdMgr
{
private:
    volatile bool obd_busy = false;

    ELM327 myELM327;
    ObdData obd_data;
    int obd_status = BT_INIT_FAILED;
    const String obd_name = "OBDII";

protected:
    void QueryCoolant(uint16_t &coolant_temp);
    void QueryVoltage(uint16_t &voltage_level);
    void QueryRPM(uint16_t &rpm_value);
    void QueryDistAfterErrorClear(uint16_t &distance);
    void QueryMaf(float &fuel_consumption);

    TaskHandle_t query_obd_data_task = NULL;

public:
    ObdMgr()
    {
        Serial.println("====ObdMgr");
    }

    ~ObdMgr()
    {
        Serial.println("~~~~ObdMgr");
    }

    void Init(void);
    static void ConnectBTTask(void *param);
    static void QueryOBDData(void *param);

    ObdData GetObdData(void);
    void SetCoolantTemp(uint16_t val);
    void SetVoltageLevel(uint16_t val);
    void SetRPM(uint16_t val);
    void SetDistance(uint16_t val);
    void SetMafRate(float val);
    void SetOBDStatus(int status);
    int GetOBDStatus(void);
};

#endif