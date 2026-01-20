#ifndef __BLUETOOTH__
#define __BLUETOOTH__

#include <Arduino.h>
#include <deque>
#include <NimBLEDevice.h>

// DFRobot 라이브러리와 NimBLE 라이브러리 간의 Advertise 매크로 충돌 방지
#ifdef Advertise
    #undef Advertise
#endif


class NimBLEStream : public Stream
{
private:
    std::deque<char> rxBuffer;
    portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    NimBLERemoteCharacteristic* pRemoteCharacteristic = nullptr;

public:
    NimBLEStream() {}

    // override 키워드 제거, bool isNotify 추가
    void onNotify(NimBLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify);

    void setCharacteristic(NimBLERemoteCharacteristic* pChar);

    // Stream 인터페이스 구현 (기존과 동일)
    int available() override;
    int read() override;
    int peek() override;
    void flush() override;
    size_t write(uint8_t c) override;
    size_t write(const uint8_t* buffer, size_t size) override;
};

class BluetoothMgr
{
    private:
        NimBLEClient* pClient = nullptr;
        NimBLEStream bleStream;

        uint8_t obd_addr[6] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xba};
        TaskHandle_t taskHandler = nullptr;
        bool isConnected = false;

        static void Subscribe(void* pvParameters);
        static void ConnectOBDTask(void* pvParameters);

    public:
        BluetoothMgr();
        ~BluetoothMgr();

        void Init(const char* deviceName);
        void Connect(uint8_t remoteAddress[]);
        void Disconnect();

        Stream* GetBleStream() { return &bleStream; }
        bool GetConnected() { return isConnected; }
};

#endif