#include <WifiMgr.h>
#include <CommonApi.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include "esp_heap_caps.h"
#include "mbedtls/platform.h"

#define TEST_LOG(fmt, ...) UartLogf("[WifiMgr] " fmt "\n", ##__VA_ARGS__)
#define TEST_LINE() UartLogf("[WifiMgr] %s\n", __func__)

#ifndef WIFI_RF_TX_POWER
#define WIFI_RF_TX_POWER WIFI_POWER_11dBm
#endif

#ifndef WIFI_RF_MODEM_SLEEP
#define WIFI_RF_MODEM_SLEEP true
#endif

#ifndef WIFI_WEATHER_USE_HTTPS
#define WIFI_WEATHER_USE_HTTPS 0
#endif

static bool s_wifiEventHookRegistered = false;
static bool s_mbedtlsAllocatorConfigured = false;

static void* tls_calloc_prefer_psram(size_t n, size_t size)
{
    if (size != 0 && n > (SIZE_MAX / size)) return nullptr;
    size_t total = n * size;
    bool preferPsram = (total >= 2048);

    void* p = nullptr;
    if (preferPsram) {
        p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!p) {
            p = heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
    } else {
        p = heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!p) {
            p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
    }
    if (!p) {
        p = heap_caps_calloc(n, size, MALLOC_CAP_8BIT);
    }
    return p;
}

static void tls_free_prefer_psram(void* ptr)
{
    heap_caps_free(ptr);
}

static void ensure_mbedtls_allocator_configured()
{
    if (s_mbedtlsAllocatorConfigured) return;
    int rc = mbedtls_platform_set_calloc_free(tls_calloc_prefer_psram, tls_free_prefer_psram);
    if (rc == 0) {
        s_mbedtlsAllocatorConfigured = true;
        TEST_LOG("mbedTLS allocator configured: small alloc internal-first, large alloc PSRAM-first");
    } else {
        TEST_LOG("mbedTLS allocator configure failed: rc=%d", rc);
    }
}

static inline uint32_t weather_stack_hwm_words()
{
    return (uint32_t)uxTaskGetStackHighWaterMark(NULL);
}

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

template <size_t DOC_CAP>
static bool https_get_json(const String& url, StaticJsonDocument<DOC_CAP>* outDoc, uint32_t timeoutMs)
{
    if (!outDoc) return false;

    auto log_tls_heap = [](const char* stage, const String& targetUrl) {
        size_t freeInternal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t largestInternal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        size_t free8bit = heap_caps_get_free_size(MALLOC_CAP_8BIT);
        size_t freeSpiram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        TEST_LOG("[TLS_HEAP] %s free_int=%u largest_int=%u free_8bit=%u free_spiram=%u url=%s",
                 stage ? stage : "unknown",
                 (unsigned int)freeInternal,
                 (unsigned int)largestInternal,
                 (unsigned int)free8bit,
                 (unsigned int)freeSpiram,
                 targetUrl.c_str());
    };

    log_tls_heap("before begin", url);

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    http.useHTTP10(true);
    http.setReuse(false);
    http.setConnectTimeout(timeoutMs);
    http.setTimeout(timeoutMs);

    if (!http.begin(client, url)) {
        TEST_LOG("HTTP begin failed: %s", url.c_str());
        log_tls_heap("http.begin failed", url);
        return false;
    }

    log_tls_heap("before GET", url);
    int httpCode = http.GET();
    log_tls_heap("after GET", url);
    if (httpCode != HTTP_CODE_OK) {
        TEST_LOG("HTTP GET failed: code=%d url=%s", httpCode, url.c_str());
        http.end();
        log_tls_heap("HTTP error after end", url);
        return false;
    }

    outDoc->clear();
    WiFiClient* stream = http.getStreamPtr();
    if (!stream) {
        TEST_LOG("HTTP stream pointer null: %s", url.c_str());
        http.end();
        log_tls_heap("stream null after end", url);
        return false;
    }

    DeserializationError err = deserializeJson(*outDoc, *stream);
    if (err) {
        TEST_LOG("JSON parse failed: %s url=%s", err.c_str(), url.c_str());
        http.end();
        log_tls_heap("json parse error after end", url);
        return false;
    }

    http.end();
    log_tls_heap("after json/end", url);
    return true;
}

template <size_t DOC_CAP>
static bool http_get_json_plain(const String& url, StaticJsonDocument<DOC_CAP>* outDoc, uint32_t timeoutMs)
{
    if (!outDoc) return false;

    WiFiClient client;
    HTTPClient http;
    http.useHTTP10(true);
    http.setReuse(false);
    http.setConnectTimeout(timeoutMs);
    http.setTimeout(timeoutMs);

    if (!http.begin(client, url)) {
        TEST_LOG("HTTP(begin/plain) failed: %s", url.c_str());
        return false;
    }

    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        TEST_LOG("HTTP(GET/plain) failed: code=%d url=%s", httpCode, url.c_str());
        http.end();
        return false;
    }

    outDoc->clear();
    WiFiClient* stream = http.getStreamPtr();
    if (!stream) {
        TEST_LOG("HTTP stream pointer null (plain): %s", url.c_str());
        http.end();
        return false;
    }

    DeserializationError err = deserializeJson(*outDoc, *stream);
    if (err) {
        TEST_LOG("JSON parse failed (plain): %s url=%s", err.c_str(), url.c_str());
        http.end();
        return false;
    }

    http.end();
    return true;
}

static const char* map_weather_code_to_english(int code)
{
    switch (code) {
        case 0: return "Clear sky";
        case 1: return "Mainly clear";
        case 2: return "Partly cloudy";
        case 3: return "Overcast";
        case 45: return "Fog";
        case 48: return "Depositing rime fog";
        case 51: return "Light drizzle";
        case 53: return "Moderate drizzle";
        case 55: return "Dense drizzle";
        case 56: return "Light freezing drizzle";
        case 57: return "Dense freezing drizzle";
        case 61: return "Slight rain";
        case 63: return "Moderate rain";
        case 65: return "Heavy rain";
        case 66: return "Light freezing rain";
        case 67: return "Heavy freezing rain";
        case 71: return "Slight snow fall";
        case 73: return "Moderate snow fall";
        case 75: return "Heavy snow fall";
        case 77: return "Snow grains";
        case 80: return "Slight rain showers";
        case 81: return "Moderate rain showers";
        case 82: return "Violent rain showers";
        case 85: return "Slight snow showers";
        case 86: return "Heavy snow showers";
        case 95: return "Thunderstorm";
        case 96: return "Thunderstorm with slight hail";
        case 99: return "Thunderstorm with heavy hail";
        default: return "Unknown";
    }
}

static bool fetch_location_from_ipwhois(String* outCity, double* outLat, double* outLon, uint32_t timeoutMs)
{
    if (!outCity || !outLat || !outLon) return false;

    StaticJsonDocument<2048> doc;
    bool ok = false;
#if WIFI_WEATHER_USE_HTTPS
    ok = https_get_json("https://ipwho.is/", &doc, timeoutMs);
#else
    ok = http_get_json_plain("http://ipwho.is/", &doc, timeoutMs);
    if (!ok) {
        TEST_LOG("Location HTTP failed, fallback to HTTPS");
        ok = https_get_json("https://ipwho.is/", &doc, timeoutMs);
    }
#endif
    if (!ok) {
        return false;
    }

    bool success = doc["success"] | false;
    if (!success) {
        const char* message = doc["message"] | "unknown";
        TEST_LOG("Location API failed: %s", message);
        return false;
    }

    const char* city = doc["city"] | "";
    double lat = doc["latitude"] | 999.0;
    double lon = doc["longitude"] | 999.0;
    if (city[0] == '\0' || lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) {
        TEST_LOG("Location API invalid payload");
        return false;
    }

    *outCity = city;
    *outLat = lat;
    *outLon = lon;
    return true;
}

static bool fetch_weather_from_open_meteo(double lat, double lon, String* outWeather, uint32_t timeoutMs)
{
    if (!outWeather) return false;

    String query = String("api.open-meteo.com/v1/forecast?latitude=") + String(lat, 5) +
                   "&longitude=" + String(lon, 5) +
                   "&current=weather_code&forecast_days=1";

    StaticJsonDocument<1536> doc;
    bool ok = false;
#if WIFI_WEATHER_USE_HTTPS
    ok = https_get_json("https://" + query, &doc, timeoutMs);
#else
    ok = http_get_json_plain("http://" + query, &doc, timeoutMs);
    if (!ok) {
        TEST_LOG("Weather HTTP failed, fallback to HTTPS");
        ok = https_get_json("https://" + query, &doc, timeoutMs);
    }
#endif
    if (!ok) {
        return false;
    }

    JsonVariant codeVariant = doc["current"]["weather_code"];
    if (codeVariant.isNull()) {
        TEST_LOG("Weather API missing current.weather_code");
        return false;
    }

    int weatherCode = codeVariant.as<int>();
    *outWeather = map_weather_code_to_english(weatherCode);
    return true;
}

void WifiMgr::Init()
{
    TEST_LINE();
    //ensure_mbedtls_allocator_configured();
    TEST_LOG("credential loaded=%d", (int)WIFI_CRED_LOADED);
    if (!s_wifiEventHookRegistered) {
        WiFi.onEvent(wifi_event_logger);
        s_wifiEventHookRegistered = true;
        TEST_LOG("Wi-Fi event logger registered");
    }
    WiFi.mode(WIFI_STA);
    bool sleepOk = WiFi.setSleep(WIFI_RF_MODEM_SLEEP);
    TEST_LOG("Wi-Fi modem sleep %s (ok=%d)",
             WIFI_RF_MODEM_SLEEP ? "ON" : "OFF",
             sleepOk ? 1 : 0);
    /*
    bool txPowerOk = WiFi.setTxPower((wifi_power_t)WIFI_RF_TX_POWER);
    TEST_LOG("Wi-Fi tx power target=%d apply=%d current=%d",
             (int)WIFI_RF_TX_POWER,
             txPowerOk ? 1 : 0,
             (int)WiFi.getTxPower());
    */
    WiFi.disconnect(false, false);

    if (_connectTaskHandler != nullptr) {
        vTaskDelete(_connectTaskHandler);
        _connectTaskHandler = nullptr;
    }
    if (_timeUpdateTaskHandler != nullptr) {
        vTaskDelete(_timeUpdateTaskHandler);
        _timeUpdateTaskHandler = nullptr;
    }
    if (_weatherUpdateTaskHandler != nullptr) {
        vTaskDelete(_weatherUpdateTaskHandler);
        _weatherUpdateTaskHandler = nullptr;
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
    TEST_LOG("Worker tasks started");
    StartTimeTask();
    /*
    StartWeatherTask();
    */
}

bool WifiMgr::StartTimeTask()
{
    if (_timeUpdateTaskHandler != nullptr) return true;

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
    TEST_LOG("TimeUpdateTask started");
    return true;
}

bool WifiMgr::StartWeatherTask()
{
    if (_weatherUpdateTaskHandler != nullptr) return true;
    BaseType_t weatherRet = xTaskCreatePinnedToCore(
        WifiMgr::WeatherUpdateTask,
        "WeatherUpdateTask",
        WEATHER_TASK_STACK_SIZE,
        this,
        1,
        &_weatherUpdateTaskHandler,
        0
    );
    if (weatherRet != pdPASS) {
        Serial.println("[WifiMgr] Critical: WeatherUpdateTask create failed");
        _weatherUpdateTaskHandler = nullptr;
        return false;
    }
    TEST_LOG("WeatherUpdateTask started");
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

bool WifiMgr::PublishWeatherText(const char* city, const char* weather)
{
    if (!city || !weather || city[0] == '\0' || weather[0] == '\0') return false;

    SystemAPI* system = SystemAPI::getInstance();
    if (!system) return false;
    system->PublishWeatherText(city, weather);
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
            Serial.printf("[WifiMgr][CONNECT] attempt=%u slot=%u ssid=\"%s\" begin status=%d\n",
                          (unsigned int)attemptCount,
                          (unsigned int)slot,
                          ssid,
                          (int)WiFi.status());

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
            Serial.printf("[WifiMgr][CONNECT] attempt=%u slot=%u result_status=%d elapsed_ms=%u rssi=%d\n",
                          (unsigned int)attemptCount,
                          (unsigned int)slot,
                          (int)finalStatus,
                          (unsigned int)(millis() - beginMs),
                          (int)WiFi.RSSI());
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

void WifiMgr::WeatherUpdateTask(void* pvParameters)
{
    TEST_LINE();
    WifiMgr* self = static_cast<WifiMgr*>(pvParameters);
    if (!self) {
        vTaskDelete(NULL);
        return;
    }

    TickType_t lastAttemptTick = 0;
    TickType_t lastLocationTick = 0;
    String cachedCity;
    double cachedLat = 0.0;
    double cachedLon = 0.0;
    bool hasCachedLocation = false;
    TEST_LOG("Weather task stack=%u bytes, hwm=%u words",
             (unsigned int)WEATHER_TASK_STACK_SIZE,
             (unsigned int)weather_stack_hwm_words());

    while (true) {
        if (WiFi.status() == WL_CONNECTED) {
            TickType_t nowTick = xTaskGetTickCount();
            bool needFetch = (lastAttemptTick == 0) ||
                             ((nowTick - lastAttemptTick) >= pdMS_TO_TICKS(WEATHER_UPDATE_INTERVAL_MS));

            if (needFetch) {
                lastAttemptTick = nowTick;

                String weather;
                bool needLocationRefresh =
                    !hasCachedLocation ||
                    (lastLocationTick == 0) ||
                    ((nowTick - lastLocationTick) >= pdMS_TO_TICKS(WEATHER_LOCATION_REFRESH_INTERVAL_MS));

                if (needLocationRefresh) {
                    TEST_LOG("Weather pre-location hwm=%u words", (unsigned int)weather_stack_hwm_words());
                    String city;
                    double lat = 0.0;
                    double lon = 0.0;
                    bool locOk = fetch_location_from_ipwhois(&city, &lat, &lon, WEATHER_REQUEST_TIMEOUT_MS);
                    TEST_LOG("Weather post-location hwm=%u words", (unsigned int)weather_stack_hwm_words());
                    if (locOk) {
                        cachedCity = city;
                        cachedLat = lat;
                        cachedLon = lon;
                        hasCachedLocation = true;
                        lastLocationTick = nowTick;
                    } else if (!hasCachedLocation) {
                        TEST_LOG("Weather update failed: location fetch");
                        vTaskDelay(pdMS_TO_TICKS(1000));
                        continue;
                    } else {
                        TEST_LOG("Location refresh failed, using cached location (city=\"%s\")",
                                 cachedCity.c_str());
                    }
                }

                bool weatherOk =
                    hasCachedLocation &&
                    fetch_weather_from_open_meteo(cachedLat, cachedLon, &weather, WEATHER_REQUEST_TIMEOUT_MS);
                TEST_LOG("Weather post-forecast hwm=%u words", (unsigned int)weather_stack_hwm_words());
                if (!weatherOk) {
                    TEST_LOG("Weather update failed: weather fetch (city=\"%s\")", cachedCity.c_str());
                } else if (self->PublishWeatherText(cachedCity.c_str(), weather.c_str())) {
                    TEST_LOG("Weather update OK: city=\"%s\", weather=\"%s\"", cachedCity.c_str(), weather.c_str());
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
