#include <BluetoothMgr.h>
#include <CommonApi.h>
#if defined(ESP32) && __has_include("esp_bt.h")
#include "esp_bt.h"
#include "esp32-hal-bt.h"
#include "esp32-hal-bt-mem.h"
#endif

#if defined(ESP32) && __has_include("esp32-hal-bt.h")
extern "C" bool btInUse(void) {
    return true;
}
#endif

#define TEST_LOG(fmt, ...) UartLogf("[BluetoothMgr] " fmt "\n", ##__VA_ARGS__)
#define TEST_LINE() UartLogf("[BluetoothMgr] %s\n", __func__)

#ifndef BT_OBD_SCAN_DEBUG
#define BT_OBD_SCAN_DEBUG 0
#endif

#ifndef BT_OBD_SCAN_SECONDS
#define BT_OBD_SCAN_SECONDS 4
#endif

#ifndef BT_OBD_SCAN_MAX_ROUNDS
#define BT_OBD_SCAN_MAX_ROUNDS 5
#endif

#ifndef BT_OBD_TARGET_NAME
#define BT_OBD_TARGET_NAME "OBDBLE"
#endif

static void publish_bt_state(bool connected)
{
    SystemAPI* system = SystemAPI::getInstance();
    if (!system) return;
    system->PublishBtConnected(connected);
}

static bool name_equals_target(const std::string& nameStd)
{
    if (nameStd.empty()) return false;
    String name = String(nameStd.c_str());
    name.trim();
    name.toUpperCase();

    String target = String(BT_OBD_TARGET_NAME);
    target.trim();
    target.toUpperCase();
    return name == target;
}

static bool name_has_obd_hint(const std::string& nameStd)
{
    if (nameStd.empty()) return false;
    String name = String(nameStd.c_str());
    name.toUpperCase();
    return (name.indexOf("OBD") >= 0) ||
           (name.indexOf("ELM") >= 0) ||
           (name.indexOf("VGATE") >= 0) ||
           (name.indexOf("VLINK") >= 0);
}

static bool discover_obd_target(NimBLEAddress* outFoundAddr, int* outFoundRssi)
{
    if (!outFoundAddr) return false;
    *outFoundAddr = NimBLEAddress();
    if (outFoundRssi) *outFoundRssi = -127;

    NimBLEScan* scan = NimBLEDevice::getScan();
    if (!scan) {
        TEST_LOG("BLE scan unavailable: getScan() returned null");
        return false;
    }

    if (scan->isScanning()) {
        scan->stop();
    }

    scan->setActiveScan(true);
    scan->setInterval(45);
    scan->setWindow(30);
    scan->setDuplicateFilter(1);

    for (int round = 0; round < (int)BT_OBD_SCAN_MAX_ROUNDS; ++round) {
        scan->clearResults();
        const uint32_t scanDurationMs = (uint32_t)BT_OBD_SCAN_SECONDS * 1000U;
        TEST_LOG("BLE scan round %d/%d start: target=\"%s\" duration=%u ms",
                 round + 1,
                 (int)BT_OBD_SCAN_MAX_ROUNDS,
                 BT_OBD_TARGET_NAME,
                 (unsigned int)scanDurationMs);

        // getResults(duration, ...) blocks until scan completes.
        NimBLEScanResults results = scan->getResults(scanDurationMs, false);
        const int count = results.getCount();
        TEST_LOG("BLE scan round %d done: found=%d", round + 1, count);

        bool found = false;
        NimBLEAddress bestAddr;
        int bestRssi = -127;
        int bestTier = -1;

        for (int i = 0; i < count; ++i) {
            const NimBLEAdvertisedDevice* dev = results.getDevice((uint32_t)i);
            if (!dev) continue;

            const NimBLEAddress& addr = dev->getAddress();
            const std::string nameStd = dev->getName();
            const bool hasName = !nameStd.empty();
            const bool exact = name_equals_target(nameStd);
            const bool hinted = name_has_obd_hint(nameStd);
            const bool connectable = dev->isConnectable();
            const int rssi = (int)dev->getRSSI();
            const bool hasFff0 = dev->isAdvertisingService(NimBLEUUID("FFF0"));

            if (BT_OBD_SCAN_DEBUG && i < 12) {
                TEST_LOG("scan[%d] mac=%s rssi=%d name=\"%s\" conn=%d exact=%d hint=%d fff0=%d",
                         i,
                         addr.toString().c_str(),
                         rssi,
                         hasName ? nameStd.c_str() : "",
                         connectable ? 1 : 0,
                         exact ? 1 : 0,
                         hinted ? 1 : 0,
                         hasFff0 ? 1 : 0);
            }

            if (!connectable) continue;
            int tier = -1;
            if (exact) tier = 3;
            else if (hinted) tier = 2;
            else if (hasFff0) tier = 1;
            if (tier < 0) continue;

            if (!found || tier > bestTier || (tier == bestTier && rssi > bestRssi)) {
                found = true;
                bestAddr = addr;
                bestRssi = rssi;
                bestTier = tier;
            }
        }

        scan->clearResults();

        if (found && !bestAddr.isNull()) {
            *outFoundAddr = bestAddr;
            if (outFoundRssi) *outFoundRssi = bestRssi;
            const char* reason = (bestTier == 3) ? "name exact" :
                                 (bestTier == 2) ? "name hint" : "service hint";
            TEST_LOG("OBD target discovered (%s): name=\"%s\" mac=%s rssi=%d",
                     reason,
                     BT_OBD_TARGET_NAME,
                     bestAddr.toString().c_str(),
                     bestRssi);
            return true;
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }

    TEST_LOG("OBD target not found after %d rounds: \"%s\"",
             (int)BT_OBD_SCAN_MAX_ROUNDS,
             BT_OBD_TARGET_NAME);
    return false;
}

static void log_remote_services(NimBLEClient* client)
{
    if (!client) return;
    const auto& services = client->getServices();
    TEST_LOG("Remote service count=%u", (unsigned int)services.size());
    for (size_t i = 0; i < services.size() && i < 12; ++i) {
        NimBLERemoteService* svc = services[i];
        if (!svc) continue;
        TEST_LOG("service[%u]=%s",
                 (unsigned int)i,
                 svc->getUUID().toString().c_str());
    }
}

void NimBLEStream::onNotify(NimBLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify)
{
    (void)pChar;
    (void)isNotify;
    portENTER_CRITICAL(&mux);
    for(size_t i = 0; i < length; i++)
    {
        rxBuffer.push_back((char)pData[i]);
    }
    portEXIT_CRITICAL(&mux);
}

void NimBLEStream::setCharacteristic(NimBLERemoteCharacteristic* pChar)
{
    setCharacteristics(pChar, pChar);
}

void NimBLEStream::setCharacteristics(NimBLERemoteCharacteristic* pNotifyChar, NimBLERemoteCharacteristic* pWriteChar)
{
    pNotifyCharacteristic = pNotifyChar;
    pWriteCharacteristic = pWriteChar ? pWriteChar : pNotifyChar;

    if (pNotifyCharacteristic && pNotifyCharacteristic->canNotify()) {
        pNotifyCharacteristic->subscribe(true, std::bind(&NimBLEStream::onNotify, this,
                                                        std::placeholders::_1,
                                                        std::placeholders::_2,
                                                        std::placeholders::_3,
                                                        std::placeholders::_4));
    }
}

int NimBLEStream::available()
{
    portENTER_CRITICAL(&mux);
    int size = rxBuffer.size();
    portEXIT_CRITICAL(&mux);
    return size;
}

int NimBLEStream::read()
{
    portENTER_CRITICAL(&mux);
    if (rxBuffer.empty())
    {
        portEXIT_CRITICAL(&mux);
        return -1;
    }

    char c = rxBuffer.front();
    rxBuffer.pop_front();
    portEXIT_CRITICAL(&mux);

    return (int)c;
}

int NimBLEStream::peek()
{
    portENTER_CRITICAL(&mux);
    if (rxBuffer.empty())
    {
        portEXIT_CRITICAL(&mux);
        return -1;
    }

    char c = rxBuffer.front();
    portEXIT_CRITICAL(&mux);
    return (int)c;
}

void NimBLEStream::flush()
{
    portENTER_CRITICAL(&mux);
    rxBuffer.clear();
    portEXIT_CRITICAL(&mux);
}

size_t NimBLEStream::write(uint8_t c)
{
    if (!pWriteCharacteristic) return 0;
    if (!(pWriteCharacteristic->canWrite() || pWriteCharacteristic->canWriteNoResponse())) return 0;
    bool response = pWriteCharacteristic->canWrite() && !pWriteCharacteristic->canWriteNoResponse();
    pWriteCharacteristic->writeValue(&c, 1, response);
    return 1;
}

size_t NimBLEStream::write(const uint8_t* buffer, size_t size)
{
    if (!buffer || size == 0) return 0;
    if (!pWriteCharacteristic) return 0;
    if (!(pWriteCharacteristic->canWrite() || pWriteCharacteristic->canWriteNoResponse())) return 0;
    bool response = pWriteCharacteristic->canWrite() && !pWriteCharacteristic->canWriteNoResponse();
    pWriteCharacteristic->writeValue((uint8_t*)buffer, size, response);
    return size;
}

BluetoothMgr::BluetoothMgr()
{
    Serial.println("====BluetoothMgr");
    TEST_LINE();
}

BluetoothMgr::~BluetoothMgr()
{
    if (pClient) NimBLEDevice::deleteClient(pClient);
    Serial.println("~~~~BluetoothMgr");
}

void BluetoothMgr::Init(const char* deviceName)
{
    TEST_LOG("Init begin deviceName=%s", deviceName ? deviceName : "(null)");
#if defined(ESP32) && __has_include("esp_bt.h")
    TEST_LOG("btInUse=%d btStarted=%d btStatus=%d",
             btInUse() ? 1 : 0, btStarted() ? 1 : 0, (int)esp_bt_controller_get_status());
    TEST_LOG("pre NimBLE init btStatus=%d", (int)esp_bt_controller_get_status());
#endif

    TEST_LOG("calling NimBLEDevice::init");
    NimBLEDevice::init(deviceName);
    Serial.println("[BluetoothMgr] NimBLE Initialized: " + String(deviceName));
    TEST_LOG("NimBLEDevice::init done");
    // UI BT icon reflects controller enabled state, not OBD link state.
    publish_bt_state(true);
    connectObdTaskRunning = false;
    connectObdTaskHandler = nullptr;
    lastObdConnectRequestMs = 0;

    if (taskHandler != NULL) {
        vTaskDelete(taskHandler);
        taskHandler = NULL;
    }

    BaseType_t ret = xTaskCreatePinnedToCore(
        BluetoothMgr::Subscribe,
        "BT_Sub",
        4096,
        this,
        1,
        &taskHandler,
        0
    );
    if (ret != pdPASS) {
        Serial.println("[BluetoothMgr] Critical: BT_Sub task create failed");
    } else {
        TEST_LOG("BT_Sub task created");
    }
}

void BluetoothMgr::Subscribe(void* pvParameters)
{
    TEST_LINE();
    BluetoothMgr* self = static_cast<BluetoothMgr*>(pvParameters);
    SystemAPI* sys = SystemAPI::getInstance();
    BtEventData event;

    while (true) {
        if (sys->btSubscriber.ReceiveEvent(&event, portMAX_DELAY)) {
            switch (event.type) {
                case BT_REQUEST_CONNECT_OBD:
                {
                    uint32_t nowMs = millis();
                    uint32_t elapsed = nowMs - self->lastObdConnectRequestMs;
                    if (self->isConnected) {
                        TEST_LOG("BT_REQUEST_CONNECT_OBD ignored: already connected");
                        break;
                    }
                    if (self->connectObdTaskRunning) {
                        TEST_LOG("BT_REQUEST_CONNECT_OBD ignored: connect task running");
                        break;
                    }
                    if (self->lastObdConnectRequestMs != 0 &&
                        elapsed < BluetoothMgr::OBD_CONNECT_RETRY_INTERVAL_MS) {
                        TEST_LOG("BT_REQUEST_CONNECT_OBD throttled: wait=%u ms",
                                 (unsigned int)(BluetoothMgr::OBD_CONNECT_RETRY_INTERVAL_MS - elapsed));
                        break;
                    }

                    Serial.println("[BluetoothMgr] Create connect OBD task.");
                    self->lastObdConnectRequestMs = nowMs;
                    self->connectObdTaskRunning = true;
                    BaseType_t ret = xTaskCreatePinnedToCore(
                        BluetoothMgr::ConnectOBDTask,
                        "ConnectOBDTask",
                        4096,
                        self,
                        2,
                        &self->connectObdTaskHandler,
                        0
                    );
                    if (ret != pdPASS) {
                        self->connectObdTaskRunning = false;
                        self->connectObdTaskHandler = nullptr;
                        Serial.println("[BluetoothMgr] Critical: ConnectOBDTask create failed");
                    }
                    break;
                }
                case BT_REQUEST_DISCONNECT:
                    self->Disconnect();
                    break;
                case BT_REQUEST_RESET_CONNECTION:
                    self->Disconnect();
                    self->lastObdConnectRequestMs = 0;
                    sys->btSubscriber.SetEvent(BT_REQUEST_CONNECT_OBD);
                    break;
                default:
                    break;
            }
        }
    }
}

void BluetoothMgr::Connect(uint8_t remoteAddress[])
{
    (void)remoteAddress;
    TEST_LOG("Connect flow: discovery-first target=\"%s\"", BT_OBD_TARGET_NAME);

    if (pClient == nullptr) pClient = NimBLEDevice::createClient();
    if (pClient == nullptr) {
        TEST_LOG("NimBLE createClient failed");
        isConnected = false;
        publish_bt_state(true);
        return;
    }

    NimBLEAddress discoveredAddr;
    int discoveredRssi = -127;
    if (!discover_obd_target(&discoveredAddr, &discoveredRssi)) {
        Serial.println("[BluetoothMgr] Connect failed");
        isConnected = false;
        publish_bt_state(true);
        return;
    }

    TEST_LOG("Discovery success: mac=%s rssi=%d",
             discoveredAddr.toString().c_str(),
             discoveredRssi);

    bool linkConnected = pClient->connect(discoveredAddr);
    if (!linkConnected) {
        TEST_LOG("Connect after discovery failed: mac=%s err=%d",
                 discoveredAddr.toString().c_str(),
                 pClient->getLastError());
        Serial.println("[BluetoothMgr] Connect failed");
        isConnected = false;
        publish_bt_state(true);
        return;
    }

    TEST_LOG("BLE link connected: peer=%s rssi=%d",
             pClient->getPeerAddress().toString().c_str(),
             pClient->getRssi());

    NimBLERemoteService* pSvc = pClient->getService("FFF0");
    if (!pSvc) {
        TEST_LOG("Required service FFF0 not found");
        log_remote_services(pClient);
        pClient->disconnect();
        isConnected = false;
        publish_bt_state(true);
        return;
    }

    NimBLERemoteCharacteristic* pNotifyChr = pSvc->getCharacteristic("FFF1");
    NimBLERemoteCharacteristic* pWriteChr = pSvc->getCharacteristic("FFF2");

    if (!pNotifyChr) {
        TEST_LOG("Required characteristic FFF1 not found in service FFF0");
        pClient->disconnect();
        isConnected = false;
        publish_bt_state(true);
        return;
    }

    if (!pWriteChr) {
        TEST_LOG("Characteristic FFF2 not found, fallback write on FFF1");
        pWriteChr = pNotifyChr;
    }

    TEST_LOG("GATT channels: notify=%s write=%s notify_cap=%d write_cap=%d write_nr=%d",
             pNotifyChr->getUUID().toString().c_str(),
             pWriteChr->getUUID().toString().c_str(),
             pNotifyChr->canNotify() ? 1 : 0,
             pWriteChr->canWrite() ? 1 : 0,
             pWriteChr->canWriteNoResponse() ? 1 : 0);

    bleStream.setCharacteristics(pNotifyChr, pWriteChr);
    isConnected = true;
    publish_bt_state(true);
    Serial.println("[BluetoothMgr] BLE & ELM Stream Connected");
}

void BluetoothMgr::Disconnect()
{
    if (pClient && pClient->isConnected()) {
        pClient->disconnect();
        Serial.println("[BluetoothMgr] Disconnected");
    }
    isConnected = false;
    publish_bt_state(true);
}

void BluetoothMgr::ConnectOBDTask(void* pvParameters)
{
    TEST_LINE();
    BluetoothMgr* self = static_cast<BluetoothMgr*>(pvParameters);

    if (self != nullptr) {
        self->Connect(self->obd_addr);
        self->connectObdTaskRunning = false;
        self->connectObdTaskHandler = nullptr;
    }

    Serial.println("[BluetoothMgr] ConnectOBDTask finished and deleting itself.");
    vTaskDelete(NULL);
}
