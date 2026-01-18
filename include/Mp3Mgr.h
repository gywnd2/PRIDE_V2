#ifndef __MP3__
#define __MP3__

#include <DFRobotDFPlayerMini.h>
#include <HardwareSerial.h>
#include <Arduino.h>

#define DFPLAYER_VOLUME 30
// P4 포트에 맞춰 18, 17로 수정하거나 기존 핀을 사용하세요.
#define DFPLAYER_RX 18
#define DFPLAYER_TX 17

class Mp3Mgr
{
    private:
        HardwareSerial dfpSerial;
        DFRobotDFPlayerMini dfPlayer;
        bool WelcomePlayed;
        TaskHandle_t taskHandler = NULL;
        int _pendingTrack = 0;
        static void Subscribe(void* pvParameters);

    public:
        Mp3Mgr()
        : dfpSerial(1), WelcomePlayed(false)
        {
            Serial.println("====Mp3Mgr");
        }
        ~Mp3Mgr()
        {
            Serial.println("~~~~Mp3Mgr");
        }

        bool Init(void);
};

#endif