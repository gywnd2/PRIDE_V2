#include <main.h>
#include <CommonApi.h>
#include <Mp3Mgr.h>

#ifdef Advertise
    #undef Advertise
#endif

#include <DisplayMgr.h>
#include <StorageMgr.h>
#include <BluetoothMgr.h>
#include <ObdMgr.h>

DisplayMgr* displayMgr   = nullptr;
StorageMgr* storageMgr   = nullptr;
Mp3Mgr* mp3Mgr       = nullptr;
ObdMgr* obdMgr       = nullptr;
BluetoothMgr* bluetoothMgr = nullptr;

void setup()
{
    Serial.begin(115200);
    Serial.println("Setup started");

    SystemAPI* system = SystemAPI::getInstance();
    system->Init();

    // 1. 저장소 초기화
    storageMgr = new StorageMgr();
    storageMgr->Init();
    system->registerStorage(storageMgr);

    // 2. 사운드 및 디스플레이 초기화
    mp3Mgr = new Mp3Mgr();
    mp3Mgr->Init();
    system->registerMp3(mp3Mgr);

    displayMgr = new DisplayMgr();
    displayMgr->Init();
    system->registerDisplay(displayMgr);

    // 3. 부팅 애니메이션 재생
    system->PlaySplash();

    // 4. BLE 통신 초기화
    bluetoothMgr = new BluetoothMgr();
    bluetoothMgr->Init("PRIDE_V2");
    system->registerBt(bluetoothMgr);

    // 5. OBD 서비스 시작
    obdMgr = new ObdMgr();
    system->registerObd(obdMgr);
    obdMgr->Init();

}

void loop()
{
    // 각 매니저의 내부 태스크들이 작동할 수 있도록 양보
    vTaskDelay(pdMS_TO_TICKS(1000));
}