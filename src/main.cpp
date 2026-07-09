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

static bool is_hex_command(const String& s)
{
    if (s.length() < 4 || s.length() > 8) return false;
    for (size_t i = 0; i < s.length(); ++i) {
        char c = s.charAt(i);
        bool isHex = (c >= '0' && c <= '9') ||
                     (c >= 'A' && c <= 'F') ||
                     (c >= 'a' && c <= 'f');
        if (!isHex) return false;
    }
    return true;
}

static bool is_hex_len(const String& s, size_t minLen, size_t maxLen)
{
    if (s.length() < minLen || s.length() > maxLen) return false;
    for (size_t i = 0; i < s.length(); ++i) {
        char c = s.charAt(i);
        bool isHex = (c >= '0' && c <= '9') ||
                     (c >= 'A' && c <= 'F') ||
                     (c >= 'a' && c <= 'f');
        if (!isHex) return false;
    }
    return true;
}

static uint8_t service_percent_from_km(uint32_t serviceKm, uint32_t cycleKm)
{
    if (cycleKm == 0U) cycleKm = SERVICE_OIL_CYCLE_DEFAULT_KM;
    if (serviceKm >= cycleKm) return 0;

    uint32_t remainingKm = cycleKm - serviceKm;
    return (uint8_t)(((remainingKm * 100U) + (cycleKm / 2U)) / cycleKm);
}

static void print_service_diag(SystemAPI* system)
{
    ServiceOdoState odoState = {};
    uint32_t cycleKm = SERVICE_OIL_CYCLE_DEFAULT_KM;
    uint32_t currentOdoKm = 0;
    uint32_t calculatedServiceKm = 0;
    UiSharedState snap = {};

    bool odoOk = storageMgr && storageMgr->ReadServiceOdoState(&odoState);
    bool cycleOk = storageMgr && storageMgr->ReadServiceOilCycleKm(&cycleKm);
    if (cycleKm == 0U) cycleKm = SERVICE_OIL_CYCLE_DEFAULT_KM;

    if (obdMgr) {
        currentOdoKm = obdMgr->GetOdometerKm();
    }
    if (currentOdoKm == 0U && odoOk) {
        currentOdoKm = odoState.last_seen_odo_km;
    }

    if (odoOk) {
        calculatedServiceKm = odoState.last_service_km;
        if (currentOdoKm > odoState.last_seen_odo_km) {
            uint32_t deltaKm = currentOdoKm - odoState.last_seen_odo_km;
            calculatedServiceKm = (UINT32_MAX - calculatedServiceKm < deltaKm)
                ? UINT32_MAX
                : (calculatedServiceKm + deltaKm);
        }
    }

    bool snapOk = system && system->GetUiSharedSnapshot(&snap, pdMS_TO_TICKS(50));
    uint8_t calculatedPercent = service_percent_from_km(calculatedServiceKm, cycleKm);

    Serial.printf(
        "[svc] odo_ok=%d cycle_ok=%d rev=%lu acc_valid=%d source_base=%lu last_seen=%lu stored_service=%lu current_source=%lu cycle=%lu calc_service=%lu calc_percent=%u ui_ok=%d ui_service=%lu ui_cycle=%lu ui_percent=%u ui_due=%d\n",
        odoOk ? 1 : 0,
        cycleOk ? 1 : 0,
        (unsigned long)odoState.revision,
        odoState.base_odo_valid ? 1 : 0,
        (unsigned long)odoState.base_odo_km,
        (unsigned long)odoState.last_seen_odo_km,
        (unsigned long)odoState.last_service_km,
        (unsigned long)currentOdoKm,
        (unsigned long)cycleKm,
        (unsigned long)calculatedServiceKm,
        (unsigned int)calculatedPercent,
        snapOk ? 1 : 0,
        (unsigned long)snap.serviceOdoKm,
        (unsigned long)snap.serviceOilCycleKm,
        (unsigned int)snap.oilPercent,
        snap.serviceDue ? 1 : 0
    );
}

void setup()
{
    Serial.begin(115200);
    TEST_LOG("setup begin");
    Serial.println("[Build] svc-accumulator-jitterdiag-nodebug-20260701-02");

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
    system->RefreshServiceDueFromStorage();
    TEST_LOG("service due refresh done");

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
        } else if (inputLower == "svc") {
            print_service_diag(SystemAPI::getInstance());
        } else if (inputLower.startsWith("svc service ")) {
            String valueText = input.substring(12);
            valueText.trim();
            uint32_t serviceKm = (uint32_t)valueText.toInt();
            bool ok = SystemAPI::getInstance()->SetServiceOdoKm(serviceKm);
            Serial.printf("[svc] set service=%lu ok=%d\n", (unsigned long)serviceKm, ok ? 1 : 0);
            print_service_diag(SystemAPI::getInstance());
        } else if (inputLower.startsWith("svc base ")) {
            String valueText = input.substring(9);
            valueText.trim();
            uint32_t serviceKm = (uint32_t)valueText.toInt();
            bool ok = SystemAPI::getInstance()->SetServiceOdoKm(serviceKm);
            Serial.printf("[svc] set service=%lu ok=%d alias=base\n", (unsigned long)serviceKm, ok ? 1 : 0);
            print_service_diag(SystemAPI::getInstance());
        } else if (inputLower.startsWith("svc cycle ")) {
            String valueText = input.substring(10);
            valueText.trim();
            uint32_t cycleKm = (uint32_t)valueText.toInt();
            bool ok = SystemAPI::getInstance()->SetServiceOilCycleKm(cycleKm);
            Serial.printf("[svc] set cycle=%lu ok=%d\n", (unsigned long)cycleKm, ok ? 1 : 0);
            print_service_diag(SystemAPI::getInstance());
        } else if (inputLower.startsWith("svc odo ")) {
            String valueText = input.substring(8);
            valueText.trim();
            uint32_t currentOdoKm = (uint32_t)valueText.toInt();
            uint32_t serviceKm = 0;
            if (obdMgr) {
                obdMgr->SetOdometerKm(currentOdoKm);
            }
            bool ok = SystemAPI::getInstance()->UpdateServiceOdoFromCurrentOdo(currentOdoKm, &serviceKm);
            Serial.printf(
                "[svc] simulate odo=%lu ok=%d service=%lu\n",
                (unsigned long)currentOdoKm,
                ok ? 1 : 0,
                (unsigned long)serviceKm
            );
            print_service_diag(SystemAPI::getInstance());
        } else if (inputLower.startsWith("pid ") || inputLower == "pid") {
            String pid = input.substring(3);
            pid.trim();
            pid.toUpperCase();

            if (!is_hex_command(pid)) {
                Serial.println("[pid] usage: pid 0000 / 22B002 / 22B0022");
            } else if (obdMgr == nullptr) {
                Serial.printf("[pid] %s fail: obd manager unavailable\n", pid.c_str());
            } else {
                String payload;
                int8_t state = ELM_GENERAL_ERROR;
                bool ok = obdMgr->QueryPidRaw(pid, payload, state);
                Serial.printf("[pid] %s %s state=%d payload=%s\n",
                              pid.c_str(),
                              ok ? "ok" : "fail",
                              (int)state,
                              payload.c_str());
            }
        } else if (inputLower.startsWith("pidh ")) {
            String args = input.substring(5);
            args.trim();
            int split = args.indexOf(' ');
            if (split <= 0) {
                Serial.println("[pidh] usage: pidh 7C6 22B002");
            } else if (obdMgr == nullptr) {
                Serial.println("[pidh] fail: obd manager unavailable");
            } else {
                String header = args.substring(0, split);
                String command = args.substring(split + 1);
                header.trim();
                command.trim();
                header.toUpperCase();
                command.toUpperCase();

                if (!is_hex_len(header, 3, 8) || !is_hex_command(command)) {
                    Serial.println("[pidh] usage: pidh 7C6 22B002");
                } else {
                    String payload;
                    int8_t state = ELM_GENERAL_ERROR;
                    bool ok = obdMgr->QueryPidRawWithHeader(header, command, payload, state);
                    Serial.printf("[pidh] header=%s cmd=%s %s state=%d payload=%s\n",
                                  header.c_str(),
                                  command.c_str(),
                                  ok ? "ok" : "fail",
                                  (int)state,
                                  payload.c_str());
                }
            }
        } else if (inputLower.startsWith("elm ")) {
            String command = input.substring(4);
            command.trim();
            command.toUpperCase();

            if (command.length() < 4) {
                Serial.println("[elm] usage: elm <raw ELM command>");
            } else if (obdMgr == nullptr) {
                Serial.printf("[elm] %s fail: obd manager unavailable\n", command.c_str());
            } else {
                String payload;
                int8_t state = ELM_GENERAL_ERROR;
                bool ok = obdMgr->QueryPidRaw(command, payload, state);
                Serial.printf("[elm] %s %s state=%d payload=%s\n",
                              command.c_str(),
                              ok ? "ok" : "fail",
                              (int)state,
                              payload.c_str());
            }
        } else if (inputLower == "odoprobe") {
            if (obdMgr) {
                obdMgr->PrintOdometerProbe();
            } else {
                Serial.println("[odo-probe] fail: obd manager unavailable");
            }
        } else if (inputLower.startsWith("canprobe")) {
            uint32_t targetKm = 90931U;
            String valueText = input.substring(8);
            valueText.trim();
            if (valueText.length() > 0) {
                targetKm = (uint32_t)valueText.toInt();
            }
            if (obdMgr) {
                obdMgr->PrintCanOdometerProbe(targetKm);
            } else {
                Serial.println("[can-probe] fail: obd manager unavailable");
            }
        }
    }

    vTaskDelay(pdMS_TO_TICKS(50));
}
