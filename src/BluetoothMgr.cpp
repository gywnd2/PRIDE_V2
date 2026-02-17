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

#define TEST_LOG(fmt, ...) Serial.printf("[BluetoothMgr] " fmt "\n", ##__VA_ARGS__)
#define TEST_LINE() Serial.printf("[BluetoothMgr] %s\n", __func__)

static void publish_bt_state(bool connected)
{
    SystemAPI* system = SystemAPI::getInstance();
    if (!system) return;
    system->PublishBtConnected(connected);
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
    pRemoteCharacteristic = pChar;
    if (pRemoteCharacteristic && pRemoteCharacteristic->canNotify())
    {
        pRemoteCharacteristic->subscribe(true, std::bind(&NimBLEStream::onNotify, this,
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
    if (pRemoteCharacteristic && pRemoteCharacteristic->canWrite())
    {
        pRemoteCharacteristic->writeValue(&c, 1, false);
        return 1;
    }
    return 0;
}

size_t NimBLEStream::write(const uint8_t* buffer, size_t size)
{
    if (pRemoteCharacteristic && pRemoteCharacteristic->canWrite())
    {
        pRemoteCharacteristic->writeValue((uint8_t*)buffer, size, false);
        return size;
    }
    return 0;
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
    publish_bt_state(false);
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
    Serial.println("[BluetoothMgr] Attempting BLE connection...");

    if (pClient == nullptr) pClient = NimBLEDevice::createClient();

    NimBLEAddress addr(remoteAddress, 0);
    if (pClient->connect(addr)) {
        NimBLERemoteService* pSvc = pClient->getService("FFF0");
        if (pSvc) {
            NimBLERemoteCharacteristic* pChr = pSvc->getCharacteristic("FFF1");
            if (pChr) {
                bleStream.setCharacteristic(pChr);
                isConnected = true;
                publish_bt_state(true);
                Serial.println("[BluetoothMgr] BLE & ELM Stream Connected");
                return;
            }
        }
        Serial.println("[BluetoothMgr] Service/Char not found");
        pClient->disconnect();
    } else {
        Serial.println("[BluetoothMgr] Connect failed");
    }
    isConnected = false;
    publish_bt_state(false);
}

void BluetoothMgr::Disconnect()
{
    if (pClient && pClient->isConnected()) {
        pClient->disconnect();
        Serial.println("[BluetoothMgr] Disconnected");
    }
    isConnected = false;
    publish_bt_state(false);
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
