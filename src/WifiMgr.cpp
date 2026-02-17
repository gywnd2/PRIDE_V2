#include <WifiMgr.h>
#include <CommonApi.h>
#include <WiFi.h>
#include <time.h>

#define TEST_LOG(fmt, ...) Serial.printf("[WifiMgr] " fmt "\n", ##__VA_ARGS__)
#define TEST_LINE() Serial.printf("[WifiMgr] %s\n", __func__)

static int32_t clamp_rssi(int32_t rssi)
{
    if (rssi > -40) return -40;
    if (rssi < -100) return -100;
    return rssi;
}

void WifiMgr::Init()
{
    TEST_LINE();
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.disconnect(false, false);

    if (_connectTaskHandler != nullptr) {
        vTaskDelete(_connectTaskHandler);
        _connectTaskHandler = nullptr;
    }

    BaseType_t ret = xTaskCreatePinnedToCore(
        WifiMgr::ConnectTask,
        "WifiConnectTask",
        4096,
        this,
        2,
        &_connectTaskHandler,
        0
    );

    if (ret != pdPASS) {
        Serial.println("[WifiMgr] Critical: WifiConnectTask create failed");
        return;
    }

    TEST_LOG("WifiConnectTask created");
}

bool WifiMgr::TryStartWorkerTasks()
{
    if (_workerTasksStarted) return true;

    BaseType_t timeRet = xTaskCreatePinnedToCore(
        WifiMgr::TimeUpdateTask,
        "TimeUpdateTask",
        4096,
        this,
        1,
        &_timeUpdateTaskHandler,
        0
    );
    if (timeRet != pdPASS) {
        Serial.println("[WifiMgr] Critical: TimeUpdateTask create failed");
        _timeUpdateTaskHandler = nullptr;
        return false;
    }

    BaseType_t statusRet = xTaskCreatePinnedToCore(
        WifiMgr::WifiStatusUpdateTask,
        "WifiStatusUpdateTask",
        4096,
        this,
        1,
        &_wifiStatusTaskHandler,
        0
    );
    if (statusRet != pdPASS) {
        Serial.println("[WifiMgr] Critical: WifiStatusUpdateTask create failed");
        if (_timeUpdateTaskHandler != nullptr) {
            vTaskDelete(_timeUpdateTaskHandler);
            _timeUpdateTaskHandler = nullptr;
        }
        _wifiStatusTaskHandler = nullptr;
        return false;
    }

    _workerTasksStarted = true;
    TEST_LOG("Worker tasks started");
    return true;
}

void WifiMgr::PublishWifiState(bool connected, int32_t rssi)
{
    SystemAPI* system = SystemAPI::getInstance();
    if (!system) return;
    system->PublishWifiState(connected, clamp_rssi(rssi));
}

bool WifiMgr::PublishClockText(const char* hhmm)
{
    if (!hhmm || hhmm[0] == '\0') return false;

    SystemAPI* system = SystemAPI::getInstance();
    if (!system) return false;
    system->PublishClockText(hhmm);
    return true;
}

void WifiMgr::ConnectTask(void* pvParameters)
{
    TEST_LINE();
    WifiMgr* self = static_cast<WifiMgr*>(pvParameters);
    if (!self) {
        vTaskDelete(NULL);
        return;
    }

    uint32_t lastAttemptMs = 0;
    bool wasConnected = false;

    while (true) {
        wl_status_t status = WiFi.status();
        if (status == WL_CONNECTED) {
            if (!wasConnected) {
                wasConnected = true;
                TEST_LOG("Connected: %s RSSI=%d dBm",
                         WiFi.localIP().toString().c_str(), WiFi.RSSI());
            }
            self->PublishWifiState(true, WiFi.RSSI());
            if (!self->_workerTasksStarted) {
                self->TryStartWorkerTasks();
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        if (wasConnected) {
            wasConnected = false;
            Serial.println("[WifiMgr] WiFi disconnected");
        }

        self->PublishWifiState(false, -100);

        uint32_t now = millis();
        if ((now - lastAttemptMs) >= WIFI_RETRY_INTERVAL_MS) {
            lastAttemptMs = now;

            TEST_LOG("Connecting to SSID: %s", WIFI_SSID);
            WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

            uint32_t beginMs = millis();
            while (WiFi.status() != WL_CONNECTED &&
                   (millis() - beginMs) < WIFI_CONNECT_TIMEOUT_MS) {
                vTaskDelay(pdMS_TO_TICKS(250));
            }

            if (WiFi.status() != WL_CONNECTED) {
                Serial.println("[WifiMgr] Connect failed, retry in 5s");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

void WifiMgr::TimeUpdateTask(void* pvParameters)
{
    TEST_LINE();
    WifiMgr* self = static_cast<WifiMgr*>(pvParameters);
    if (!self) {
        vTaskDelete(NULL);
        return;
    }

    TickType_t lastSyncTick = 0;
    int shownMinute = -1;
    int shownHour = -1;

    while (true) {
        if (WiFi.status() == WL_CONNECTED) {
            TickType_t nowTick = xTaskGetTickCount();
            bool needSync = (lastSyncTick == 0) ||
                            ((nowTick - lastSyncTick) >= pdMS_TO_TICKS(NTP_SYNC_INTERVAL_MS));

            if (needSync) {
                configTzTime("KST-9", "pool.ntp.org", "time.nist.gov");

                struct tm timeInfo = {};
                if (getLocalTime(&timeInfo, 5000)) {
                    lastSyncTick = nowTick;
                    TEST_LOG("NTP sync OK: %04d-%02d-%02d %02d:%02d:%02d",
                             timeInfo.tm_year + 1900, timeInfo.tm_mon + 1, timeInfo.tm_mday,
                             timeInfo.tm_hour, timeInfo.tm_min, timeInfo.tm_sec);
                } else {
                    TEST_LOG("NTP sync failed");
                }
            }

            struct tm localTime = {};
            if (getLocalTime(&localTime, 1000)) {
                if (localTime.tm_min != shownMinute || localTime.tm_hour != shownHour) {
                    char hhmm[6] = {0};
                    snprintf(hhmm, sizeof(hhmm), "%02d:%02d", localTime.tm_hour, localTime.tm_min);
                    if (self->PublishClockText(hhmm)) {
                        shownMinute = localTime.tm_min;
                        shownHour = localTime.tm_hour;
                    }
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void WifiMgr::WifiStatusUpdateTask(void* pvParameters)
{
    TEST_LINE();
    WifiMgr* self = static_cast<WifiMgr*>(pvParameters);
    if (!self) {
        vTaskDelete(NULL);
        return;
    }

    while (true) {
        wl_status_t status = WiFi.status();
        if (status == WL_CONNECTED) {
            self->PublishWifiState(true, WiFi.RSSI());
        } else {
            self->PublishWifiState(false, -100);
        }

        vTaskDelay(pdMS_TO_TICKS(WIFI_STATUS_UPDATE_INTERVAL_MS));
    }
}
