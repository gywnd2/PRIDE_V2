#include <main.h>
#include <CommonApi.h>
#include <DisplayMgr.h>
#include <StorageMgr.h>
#include <BluetoothMgr.h>
#include <ObdMgr.h>
#include <Mp3Mgr.h>

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

    if (displayMgr->IsLvglInitialized()) {
        if (system->LockLvgl(pdMS_TO_TICKS(5))) {
            lv_timer_handler();
            system->UnlockLvgl();
        }
    }

    vTaskDelay(pdMS_TO_TICKS(5));
}