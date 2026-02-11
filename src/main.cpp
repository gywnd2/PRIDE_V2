#include <main.h>
#include <CommonApi.h>
#include <DisplayMgr.h>
#include <StorageMgr.h>
#include <BluetoothMgr.h>
#include <ObdMgr.h>
#include <Mp3Mgr.h>
#include <ui.h>

DisplayMgr* displayMgr   = nullptr;
StorageMgr* storageMgr   = nullptr;
Mp3Mgr* mp3Mgr           = nullptr;
ObdMgr* obdMgr           = nullptr;
BluetoothMgr* bluetoothMgr = nullptr;

void setup()
{
    Serial.begin(115200);
    SystemAPI* system = SystemAPI::getInstance();
    system->Init();

    storageMgr = new StorageMgr();
    storageMgr->Init();
    system->registerStorage(storageMgr);

    mp3Mgr = new Mp3Mgr();
    mp3Mgr->Init();
    system->registerMp3(mp3Mgr);

    displayMgr = new DisplayMgr();
    displayMgr->Init();
    system->registerDisplay(displayMgr);

    system->PlaySplash();

    bluetoothMgr = new BluetoothMgr();
    bluetoothMgr->Init("PRIDE_V2");
    system->registerBt(bluetoothMgr);

    obdMgr = new ObdMgr();
    system->registerObd(obdMgr);
    obdMgr->Init();
}

void loop()
{
    static SystemAPI* system = SystemAPI::getInstance();

    static uint32_t startTime = 0;
    static int step = 0; // 진행 단계를 기록

    if (Serial.available() > 0) {
        // 시리얼 버퍼에서 문자열 읽기
        String input = Serial.readStringUntil('\n');
        input.trim(); // 공백이나 줄바꿈 제거

        if (input == "reset") {
            Serial.println("[System] Reset command received. Rebooting...");
            delay(500); // 메시지가 전송될 시간을 잠시 벌어줌
            ESP.restart(); // ESP32 소프트웨어 리셋
        }
    }

    if(displayMgr->IsSplashFinished())
    {
        if(startTime == 0) {
            startTime = millis(); // 시작 시간 기록
        }
        uint32_t elapsed = millis() - startTime;

        // 2. 단 한 번씩만 순차적으로 실행되도록 구조 변경
        if (system->LockLvgl(pdMS_TO_TICKS(10))) {
            if (step == 0 && elapsed >= 1000) {
                update_coolant_gauge(150);
                update_battery_gauge(20);
                step = 1;
            }
            else if (step == 1 && elapsed >= 2000) {
                update_coolant_gauge(0);
                update_battery_gauge(0);
                step = 2;
            }
            else if (step == 2 && elapsed >= 3000) {
                update_coolant_gauge(90);
                update_battery_gauge(18);
                step = 3; // 이제 더 이상 실행 안 됨
            }
            system->UnlockLvgl();
        }
    }

    vTaskDelay(pdMS_TO_TICKS(100));
}