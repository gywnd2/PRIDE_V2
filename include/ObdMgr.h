#ifndef __OBD__
#define __OBD__

#include <Arduino.h>
#include <ELMduino.h>
#include "esp_task_wdt.h"
#include <NimBLEDevice.h>
#include "freertos/semphr.h"
#include "freertos/queue.h"

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

enum ObdMgrEventType
{
    OBD_MGR_EVENT_NONE = 0,
    OBD_MGR_EVENT_START_CONNECT,
    OBD_MGR_EVENT_LINK_LOST,
    OBD_MGR_EVENT_RPM_SUCCESS
};

typedef struct
{
    ObdMgrEventType type;
} ObdMgrEventData;

class ObdMgr
{
private:
    volatile bool obd_busy = false;
    SemaphoreHandle_t _dataMutex = nullptr;
    QueueHandle_t _eventQueue = nullptr;

    ELM327 myELM327;
    ObdData obd_data;
    int obd_status = BT_INIT_FAILED;
    const String obd_name = "OBDII";
    TaskHandle_t _eventTask = NULL;
    TaskHandle_t _connectTask = NULL;
    TaskHandle_t query_obd_data_task = NULL;
    volatile bool _connectTaskRunning = false;
    volatile bool _queryTaskRunning = false;
    volatile bool _hadPidSuccess = false;
    volatile bool _awaitingRpmRecovery = false;
    volatile bool _goodbyeScreenActive = false;
    static constexpr uint32_t OBD_RECONNECT_INTERVAL_MS = 10000;

    bool LockData(TickType_t waitTime);
    void UnlockData();
    bool TryLockObdQuery();
    void UnlockObdQuery();
    void PostEvent(ObdMgrEventType type);
    bool StartConnectTask();
    bool StartQueryTask();

protected:
    void QueryCoolant(uint16_t &coolant_temp);
    void QueryVoltage(uint16_t &voltage_level);
    void QueryRPM(uint16_t &rpm_value);
    void QueryDistAfterErrorClear(uint16_t &distance);
    void QueryMaf(float &fuel_consumption);

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
    static void EventTask(void *param);
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
    bool QueryOutsideTemp(float& outsideTempC);
};

#endif
