#include <WifiMgr.h>
#include <CommonApi.h>
#include <WiFi.h>
#include <time.h>

#define TEST_LOG(fmt, ...) Serial.printf("[WifiMgr] " fmt "\n", ##__VA_ARGS__)
#define TEST_LINE() Serial.printf("[WifiMgr] %s\n", __func__)

static bool s_wifiEventHookRegistered = false;

static const char* wifi_reason_hint(int reason)
{
    // Common reason codes observed on ESP32/IDF variants.
    // Keep this lightweight and conservative.
    switch (reason) {
        case 2:   // AUTH_EXPIRE
        case 15:  // 4WAY_HANDSHAKE_TIMEOUT
        case 202: // AUTH_FAIL
        case 203: // ASSOC_FAIL
        case 204: // HANDSHAKE_TIMEOUT
            return "auth failure possible (check password/security)";
        case 201: // NO_AP_FOUND
            return "AP not found/weak signal";
        default:
            return "check reason code in esp_wifi docs";
    }
}

static void wifi_event_logger(WiFiEvent_t event, WiFiEventInfo_t info)
{
#if defined(ARDUINO_EVENT_WIFI_STA_DISCONNECTED)
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
        int reason = (int)info.wifi_sta_disconnected.reason;
        TEST_LOG("STA disconnected event: reason=%d (%s)", reason, wifi_reason_hint(reason));
    }
#elif defined(SYSTEM_EVENT_STA_DISCONNECTED)
    if (event == SYSTEM_EVENT_STA_DISCONNECTED) {
        int reason = (int)info.wifi_sta_disconnected.reason;
        TEST_LOG("STA disconnected event: reason=%d (%s)", reason, wifi_reason_hint(reason));
    }
#else
    (void)event;
    (void)info;
#endif
}

static int32_t clamp_rssi(int32_t rssi)
{
    if (rssi > -40) return -40;
    if (rssi < -100) return -100;
    return rssi;
}

static bool has_wifi_credentials()
{
    const bool cred1 = (WIFI_CRED_SSID1[0] != '\0' &&
                        WIFI_CRED_PASSWORD1[0] != '\0');
    const bool cred2 = (WIFI_CRED_SSID2[0] != '\0' &&
                        WIFI_CRED_PASSWORD2[0] != '\0');
    return cred1 || cred2;
}

static uint8_t get_wifi_cred_count()
{
    uint8_t count = 0;
    if (WIFI_CRED_SSID1[0] != '\0' && WIFI_CRED_PASSWORD1[0] != '\0') count++;
    if (WIFI_CRED_SSID2[0] != '\0' && WIFI_CRED_PASSWORD2[0] != '\0') count++;
    return count;
}

static bool get_wifi_credential_by_order(uint8_t order, const char** ssid, const char** password, uint8_t* slot)
{
    if (!ssid || !password || !slot) return false;

    uint8_t idx = 0;
    if (WIFI_CRED_SSID1[0] != '\0' && WIFI_CRED_PASSWORD1[0] != '\0') {
        if (idx == order) {
            *ssid = WIFI_CRED_SSID1;
            *password = WIFI_CRED_PASSWORD1;
            *slot = 1;
            return true;
        }
        idx++;
    }
    if (WIFI_CRED_SSID2[0] != '\0' && WIFI_CRED_PASSWORD2[0] != '\0') {
        if (idx == order) {
            *ssid = WIFI_CRED_SSID2;
            *password = WIFI_CRED_PASSWORD2;
            *slot = 2;
            return true;
        }
    }
    return false;
}

void WifiMgr::Init()
{
    TEST_LINE();
    TEST_LOG("credential loaded=%d", (int)WIFI_CRED_LOADED);
    if (!s_wifiEventHookRegistered) {
        WiFi.onEvent(wifi_event_logger);
        s_wifiEventHookRegistered = true;
        TEST_LOG("Wi-Fi event logger registered");
    }
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
    uint8_t nextCredOrder = 0;
    uint32_t attemptCount = 0;

    while (true) {
        if (!has_wifi_credentials()) {
            static bool missingLogged = false;
            if (!missingLogged) {
                Serial.println("[WifiMgr] Missing wifi_credentials.txt (SSID/PASSWORD). Wi-Fi connect disabled.");
                missingLogged = true;
            }
            self->PublishWifiState(false, -100);
            vTaskDelay(pdMS_TO_TICKS(WIFI_RETRY_INTERVAL_MS));
            continue;
        }

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
            attemptCount++;

            const char* ssid = nullptr;
            const char* password = nullptr;
            uint8_t slot = 0;
            uint8_t credCount = get_wifi_cred_count();
            if (credCount == 0 || !get_wifi_credential_by_order(nextCredOrder, &ssid, &password, &slot)) {
                Serial.println("[WifiMgr] No valid credential pair available");
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }
            nextCredOrder = (uint8_t)((nextCredOrder + 1) % credCount);

            TEST_LOG("Wi-Fi retry #%u: credential #%u, SSID=\"%s\"",
                     (unsigned int)attemptCount,
                     (unsigned int)slot,
                     ssid);

            // Ensure previous connecting state is cleared before changing STA config.
            WiFi.disconnect(false, false);
            vTaskDelay(pdMS_TO_TICKS(120));
            WiFi.begin(ssid, password);

            uint32_t beginMs = millis();
            while ((millis() - beginMs) < WIFI_CONNECT_TIMEOUT_MS) {
                wl_status_t waitStatus = WiFi.status();
                if (waitStatus == WL_CONNECTED ||
                    waitStatus == WL_CONNECT_FAILED ||
                    waitStatus == WL_NO_SSID_AVAIL) {
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(250));
            }

            wl_status_t finalStatus = WiFi.status();
            if (finalStatus != WL_CONNECTED) {
                if (finalStatus == WL_NO_SSID_AVAIL) {
                    TEST_LOG("Connect failed (#%u, SSID=\"%s\"): SSID not found",
                             (unsigned int)slot, ssid);
                } else if (finalStatus == WL_CONNECT_FAILED) {
                    TEST_LOG("Connect failed (#%u, SSID=\"%s\"): auth failed (check password)",
                             (unsigned int)slot, ssid);
                } else {
                    TEST_LOG("Connect failed (#%u, SSID=\"%s\"): status=%d (retry in 5s)",
                             (unsigned int)slot, ssid, (int)finalStatus);
                }
                // Keep next attempt clean and avoid ESP_ERR_WIFI_STATE.
                WiFi.disconnect(false, false);
                vTaskDelay(pdMS_TO_TICKS(120));
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
