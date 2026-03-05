#include <main.h>
#include <CommonApi.h>
#include <DisplayMgr.h>
#include <StorageMgr.h>
#include <BluetoothMgr.h>
#include <ObdMgr.h>
#include <Mp3Mgr.h>
#include <WifiMgr.h>

#define TEST_LOG(fmt, ...) UartLogf("[Main] " fmt "\n", ##__VA_ARGS__)

DisplayMgr* displayMgr   = nullptr;
StorageMgr* storageMgr   = nullptr;
Mp3Mgr* mp3Mgr           = nullptr;
ObdMgr* obdMgr           = nullptr;
BluetoothMgr* bluetoothMgr = nullptr;
WifiMgr* wifiMgr = nullptr;

static bool is_hex4(const String& s)
{
    if (s.length() != 4) return false;
    for (size_t i = 0; i < s.length(); ++i) {
        char c = s.charAt(i);
        bool isHex = (c >= '0' && c <= '9') ||
                     (c >= 'A' && c <= 'F') ||
                     (c >= 'a' && c <= 'f');
        if (!isHex) return false;
    }
    return true;
}

void setup()
{
    Serial.begin(115200);
    TEST_LOG("setup begin");
    Serial.println("[Build] bt-guard-20260216-03");

    SystemAPI* system = SystemAPI::getInstance();
    TEST_LOG("SystemAPI::getInstance done, ptr=%p", system);
    system->Init();
    TEST_LOG("SystemAPI::Init done");

    TEST_LOG("create StorageMgr");
    storageMgr = new StorageMgr();
    TEST_LOG("StorageMgr created ptr=%p", storageMgr);
    storageMgr->Init();
    TEST_LOG("StorageMgr::Init done");
    system->registerStorage(storageMgr);
    TEST_LOG("registerStorage done");

    TEST_LOG("create Mp3Mgr");
    mp3Mgr = new Mp3Mgr();
    TEST_LOG("Mp3Mgr created ptr=%p", mp3Mgr);
    mp3Mgr->Init();
    TEST_LOG("Mp3Mgr::Init done");
    system->registerMp3(mp3Mgr);
    TEST_LOG("registerMp3 done");

    TEST_LOG("create BluetoothMgr");
    bluetoothMgr = new BluetoothMgr();
    TEST_LOG("BluetoothMgr created ptr=%p", bluetoothMgr);
    bluetoothMgr->Init("PRIDE_V2");
    TEST_LOG("BluetoothMgr::Init done");
    system->registerBt(bluetoothMgr);
    TEST_LOG("registerBt done");

    TEST_LOG("create ObdMgr");
    obdMgr = new ObdMgr();
    TEST_LOG("ObdMgr created ptr=%p", obdMgr);
    system->registerObd(obdMgr);
    TEST_LOG("registerObd done");
    obdMgr->Init();
    TEST_LOG("ObdMgr::Init done");

    TEST_LOG("create DisplayMgr");
    displayMgr = new DisplayMgr();
    TEST_LOG("DisplayMgr created ptr=%p", displayMgr);
    displayMgr->Init();
    TEST_LOG("DisplayMgr::Init done");
    system->registerDisplay(displayMgr);
    TEST_LOG("registerDisplay done");

    TEST_LOG("create WifiMgr");
    wifiMgr = new WifiMgr();
    TEST_LOG("WifiMgr created ptr=%p", wifiMgr);
    wifiMgr->Init();
    TEST_LOG("WifiMgr::Init done");
    system->registerWifi(wifiMgr);
    TEST_LOG("registerWifi done");

    TEST_LOG("PlaySplash event send");
    system->PlaySplash();
    TEST_LOG("setup end");
}

void loop()
{
    if (Serial.available() > 0) {
        String input = Serial.readStringUntil('\n');
        input.trim();
        String inputLower = input;
        inputLower.toLowerCase();

        if (inputLower == "reset") {
            Serial.println("[System] Reset command received. Rebooting...");
            delay(500);
            ESP.restart();
        } else if (inputLower.startsWith("pid")) {
            String pid = input.substring(3);
            pid.trim();
            pid.toUpperCase();

            if (!is_hex4(pid)) {
                TEST_LOG("usage: pid 0000 (4 hex)");
            } else if (obdMgr == nullptr) {
                TEST_LOG("pid %s fail: obd manager unavailable", pid.c_str());
            } else {
                String payload;
                int8_t state = ELM_GENERAL_ERROR;
                bool ok = obdMgr->QueryPidRaw(pid, payload, state);
                TEST_LOG("pid %s %s state=%d payload=%s",
                         pid.c_str(),
                         ok ? "ok" : "fail",
                         (int)state,
                         payload.c_str());
            }
        }
    }

    vTaskDelay(pdMS_TO_TICKS(50));
}
