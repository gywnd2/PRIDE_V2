#ifndef __WIFI__
#define __WIFI__

#include <Arduino.h>

class WifiMgr
{
private:
    static constexpr const char* WIFI_SSID = "HJF_2.4G";
    static constexpr const char* WIFI_PASSWORD = "0314455445";
    static constexpr uint32_t WIFI_RETRY_INTERVAL_MS = 5000;
    static constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 10000;
    static constexpr uint32_t WIFI_STATUS_UPDATE_INTERVAL_MS = 1000;
    static constexpr uint32_t NTP_SYNC_INTERVAL_MS = 3600000;

    TaskHandle_t _connectTaskHandler = nullptr;
    TaskHandle_t _timeUpdateTaskHandler = nullptr;
    TaskHandle_t _wifiStatusTaskHandler = nullptr;
    bool _workerTasksStarted = false;

    static void ConnectTask(void* pvParameters);
    static void TimeUpdateTask(void* pvParameters);
    static void WifiStatusUpdateTask(void* pvParameters);

    bool TryStartWorkerTasks();
    void PublishWifiState(bool connected, int32_t rssi);
    bool PublishClockText(const char* hhmm);

public:
    WifiMgr()
    {
        Serial.println("====WifiMgr");
    }

    ~WifiMgr()
    {
        Serial.println("~~~~WifiMgr");
    }

    void Init();
};

#endif
