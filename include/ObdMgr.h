#ifndef __OBD__
#define __OBD__

#include <Arduino.h>
#include <ELMduino.h>
#include "esp_task_wdt.h"
#include <NimBLEDevice.h>
#include "freertos/semphr.h"
#include "freertos/queue.h"

class SystemAPI;

#define RPM_REQ_RETRY_MAX 2
#define BT_CONNECTION_CHECK_INTERVAL 1
#define OBD_SIMUL_QUERY_TIME 50  // 시뮬레이션 모드 시 대기 시간

struct ObdData
{
    uint16_t coolant;
    uint16_t voltage;
    uint16_t rpm;
    uint32_t odometer_km;      // total distance in km (PID 0131 fallback)
    uint32_t trip_distance_km; // delta since first odometer read after boot
    uint32_t drive_time_sec;   // seconds since boot
    int16_t outside_temp;
    bool outside_temp_valid;
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
    OBD_MGR_EVENT_ODOMETER_SUCCESS
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
    volatile bool _awaitingOdometerRecovery = false;
    volatile bool _goodbyeScreenActive = false;
    uint32_t _bootMs = 0;
    uint32_t _odometerStartKm = 0;
    uint32_t _odometerLastKm = 0;
    bool _odometerStartValid = false;
    static constexpr uint32_t OBD_RECONNECT_INTERVAL_MS = 10000;

    bool LockData(TickType_t waitTime);
    void UnlockData();
    bool TryLockObdQuery();
    void UnlockObdQuery();
    void PostEvent(ObdMgrEventType type);
    bool StartConnectTask();
    bool StartQueryTask();
    void FinalizeServiceOdoSession(SystemAPI* system);
    void HandleQueryLinkLoss(SystemAPI* system, const char* reason);

protected:
    void QueryCoolant(uint16_t &coolant_temp);
    void QueryVoltage(uint16_t &voltage_level);
    void QueryRPM(uint16_t &rpm_value);
    void QueryOdometer(uint32_t &odometer_km);
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
    void SetOdometerKm(uint32_t val);
    void SetTripDistanceKm(uint32_t val);
    void SetDriveTimeSec(uint32_t val);
    uint32_t GetOdometerKm(void);
    uint32_t GetTripDistanceKm(void);
    uint32_t GetDriveTimeSec(void);
    void SetOutsideTemp(int16_t tempC, bool valid = true);
    void SetMafRate(float val);
    void SetOBDStatus(int status);
    int GetOBDStatus(void);
    void ResetServiceOdoSessionBase(void);
    bool QueryOutsideTemp(float& outsideTempC);
    bool QueryPidRaw(const String& pidCommand, String& payloadOut, int8_t& stateOut);
};

#endif
