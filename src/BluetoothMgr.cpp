#include <BluetoothMgr.h>
#include <CommonApi.h>

void NimBLEStream::onNotify(NimBLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify)
{
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
        // 라이브러리 callback 타입에 맞춰 placeholders 4개(_1, _2, _3, _4) 사용
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
    int size = rxBuffer.size(); // length() -> size()
    portEXIT_CRITICAL(&mux);
    return size;
}

int NimBLEStream::read()
{
    portENTER_CRITICAL(&mux);
    if (rxBuffer.empty()) // size == 0 체크와 동일
    {
        portEXIT_CRITICAL(&mux);
        return -1;
    }

    char c = rxBuffer.front(); // 가장 오래된 데이터 가져오기
    rxBuffer.pop_front();      // 가져온 데이터 삭제 (erase 대신 사용)
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
}

BluetoothMgr::~BluetoothMgr()
{
    if (pClient) NimBLEDevice::deleteClient(pClient);
    Serial.println("~~~~BluetoothMgr");
}

void BluetoothMgr::Init(const char* deviceName)
{
    NimBLEDevice::init(deviceName);
    Serial.println("[BluetoothMgr] NimBLE Initialized: " + String(deviceName));

    if (taskHandler != NULL) {
        vTaskDelete(taskHandler);
        taskHandler = NULL;
    }

    xTaskCreate(BluetoothMgr::Subscribe, "BT_Sub", 4096, this, 1, &taskHandler);
}

void BluetoothMgr::Subscribe(void* pvParameters)
{
    BluetoothMgr* self = static_cast<BluetoothMgr*>(pvParameters);
    SystemAPI* sys = SystemAPI::getInstance();

    while (true) {
        if (sys->btSubscriber.IsEventPending()) {
            switch (sys->btSubscriber.GetEventType()) {
                case BT_REQUEST_CONNECT_OBD:
                    Serial.println("[BluetoothMgr] Create connect OBD task.");
                    xTaskCreate(
                        BluetoothMgr::ConnectOBDTask, // 태스크 함수
                        "ConnectOBDTask",          // 이름
                        4096,                      // 스택 크기
                        self,                      // 파라미터
                        2,                         // 우선순위
                        NULL                       // 태스크 핸들러 필요 없음
                    );
                    break;
                case BT_REQUEST_DISCONNECT:
                    self->Disconnect();
                    break;
                default: break;
            }
            sys->btSubscriber.ClearEvent();
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void BluetoothMgr::Connect(uint8_t remoteAddress[])
{
    Serial.println("[BluetoothMgr] Attempting BLE connection...");

    if (pClient == nullptr) pClient = NimBLEDevice::createClient();

    NimBLEAddress addr(remoteAddress, 0); // 0: Public, 1: Random
    if (pClient->connect(addr)) {
        // OBD2 표준 서비스 탐색 (장치마다 다를 수 있음, 보통 0xFFF0)
        NimBLERemoteService* pSvc = pClient->getService("FFF0");
        if (pSvc) {
            NimBLERemoteCharacteristic* pChr = pSvc->getCharacteristic("FFF1");
            if (pChr) {
                bleStream.setCharacteristic(pChr);
                isConnected = true;
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
}

void BluetoothMgr::Disconnect()
{
    if (pClient && pClient->isConnected()) {
        pClient->disconnect();
        isConnected = false;
        Serial.println("[BluetoothMgr] Disconnected");
    }
}

void BluetoothMgr::ConnectOBDTask(void* pvParameters)
{
    BluetoothMgr* self = static_cast<BluetoothMgr*>(pvParameters);

    if (self != nullptr) {
        self->Connect(self->obd_addr);
    }

    Serial.println("[BluetoothMgr] ConnectOBDTask finished and deleting itself.");
    vTaskDelete(NULL);
}