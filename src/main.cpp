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
    static int step = 0; // ì§„í–‰ ?¨ê³„ë¥?ê¸°ë¡

    if (Serial.available() > 0) {
        // ?œë¦¬??ë²„í¼?ì„œ ë¬¸ìž???½ê¸°
        String input = Serial.readStringUntil('\n');
        input.trim(); // ê³µë°±?´ë‚˜ ì¤„ë°”ê¿??œê±°

        if (input == "reset") {
            Serial.println("[System] Reset command received. Rebooting...");
            delay(500); // ë©”ì‹œì§€ê°€ ?„ì†¡???œê°„??? ì‹œ ë²Œì–´ì¤?
            ESP.restart(); // ESP32 ?Œí”„?¸ì›¨??ë¦¬ì…‹
        }
    }

    if(displayMgr->IsSplashFinished())
    {
        if(startTime == 0) {
            startTime = millis(); // ?œìž‘ ?œê°„ ê¸°ë¡
        }
        uint32_t elapsed = millis() - startTime;

        // 2. ????ë²ˆì”©ë§??œì°¨?ìœ¼ë¡??¤í–‰?˜ë„ë¡?êµ¬ì¡° ë³€ê²?
        if (system->LockLvgl(pdMS_TO_TICKS(10))) {
            if (step == 0 && elapsed >= 1000) {
                update_coolant_gauge(150);
                update_battery_gauge(20);
                step = 1;
            }
            else if (step == 1 && elapsed >= 2500) {
                update_coolant_gauge(0);
                update_battery_gauge(0);
                step = 2;
            }
            else if (step == 2 && elapsed >= 4000) {
                update_coolant_gauge(90);
                update_battery_gauge(18);
                step = 3; // ?´ì œ ???´ìƒ ?¤í–‰ ????
            }
            system->UnlockLvgl();
        }
    }

    vTaskDelay(pdMS_TO_TICKS(50));
}

