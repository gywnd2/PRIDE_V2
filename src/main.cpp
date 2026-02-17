#include <main.h>
#include <CommonApi.h>
#include <DisplayMgr.h>
#include <StorageMgr.h>
#include <BluetoothMgr.h>
#include <ObdMgr.h>
#include <Mp3Mgr.h>
#include <WifiMgr.h>
#include <ui.h>

#define TEST_LOG(fmt, ...) Serial.printf("[Main] " fmt "\n", ##__VA_ARGS__)

DisplayMgr* displayMgr   = nullptr;
StorageMgr* storageMgr   = nullptr;
Mp3Mgr* mp3Mgr           = nullptr;
ObdMgr* obdMgr           = nullptr;
BluetoothMgr* bluetoothMgr = nullptr;
WifiMgr* wifiMgr = nullptr;

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

        if (input == "reset") {
            Serial.println("[System] Reset command received. Rebooting...");
            delay(500);
            ESP.restart();
        }
    }

    vTaskDelay(pdMS_TO_TICKS(50));
}
