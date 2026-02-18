#ifndef __WIFI__
#define __WIFI__

#include <Arduino.h>

#if defined(__has_include)
#if __has_include("WifiCredentialsLocal.h")
#include "WifiCredentialsLocal.h"
#else
#define WIFI_CRED_SSID ""
#define WIFI_CRED_PASSWORD ""
#define WIFI_CRED_SSID1 ""
#define WIFI_CRED_PASSWORD1 ""
#define WIFI_CRED_SSID2 ""
#define WIFI_CRED_PASSWORD2 ""
#define WIFI_CRED_COUNT 0
#define WIFI_CRED_LOADED 0
#endif
#else
#define WIFI_CRED_SSID ""
#define WIFI_CRED_PASSWORD ""
#define WIFI_CRED_SSID1 ""
#define WIFI_CRED_PASSWORD1 ""
#define WIFI_CRED_SSID2 ""
#define WIFI_CRED_PASSWORD2 ""
#define WIFI_CRED_COUNT 0
#define WIFI_CRED_LOADED 0
#endif

#ifndef WIFI_CRED_SSID1
#define WIFI_CRED_SSID1 ""
#endif
#ifndef WIFI_CRED_PASSWORD1
#define WIFI_CRED_PASSWORD1 ""
#endif
#ifndef WIFI_CRED_SSID2
#define WIFI_CRED_SSID2 ""
#endif
#ifndef WIFI_CRED_PASSWORD2
#define WIFI_CRED_PASSWORD2 ""
#endif
#ifndef WIFI_CRED_COUNT
#define WIFI_CRED_COUNT 0
#endif
#ifndef WIFI_CRED_LOADED
#define WIFI_CRED_LOADED 0
#endif
#ifndef WIFI_CRED_SSID
#define WIFI_CRED_SSID WIFI_CRED_SSID1
#endif
#ifndef WIFI_CRED_PASSWORD
#define WIFI_CRED_PASSWORD WIFI_CRED_PASSWORD1
#endif

class WifiMgr
{
private:
    static constexpr const char* WIFI_SSID_1 = WIFI_CRED_SSID1;
    static constexpr const char* WIFI_PASSWORD_1 = WIFI_CRED_PASSWORD1;
    static constexpr const char* WIFI_SSID_2 = WIFI_CRED_SSID2;
    static constexpr const char* WIFI_PASSWORD_2 = WIFI_CRED_PASSWORD2;
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
