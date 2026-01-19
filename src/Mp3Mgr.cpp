#include "Mp3Mgr.h"
#include "CommonApi.h"

bool Mp3Mgr::Init(void)
{
    // 하드웨어 시리얼 초기화 (P4 포트 핀 번호 사용)
    dfpSerial.begin(9600, SERIAL_8N1, DFPLAYER_RX, DFPLAYER_TX);

    // 시리얼 연결 확인 (DFPlayer는 응답이 느릴 수 있어 잠시 대기)
    delay(200);

    if (!dfPlayer.begin(dfpSerial))
    {
        Serial.println("[Mp3Mgr] Failed to initialize DFPlayer Mini!");
        return false;
    }

    Serial.println("[Mp3Mgr] Initialized DFPlayer Mini successfully.");
    dfPlayer.volume(DFPLAYER_VOLUME);

    this->_pendingTrack = 1;

    if (taskHandler != NULL) {
        vTaskDelete(taskHandler);
        taskHandler = NULL;
    }

    xTaskCreatePinnedToCore(
        Mp3Mgr::Subscribe,
        "Mp3EventSubscriber",
        4096,
        this,
        4,
        &taskHandler,
        0
    );

    return true;
}

void Mp3Mgr::Subscribe(void* pvParameters)
{
    Mp3Mgr* self = static_cast<Mp3Mgr*>(pvParameters);
    SystemAPI* system = SystemAPI::getInstance();
    SoundEventData event;

    while(true)
    {
        if(system->soundSubscriber.ReceiveEvent(&event, portMAX_DELAY))
        {
            if(event.type == SOUND_PLAY_TRACK)
            {
                Serial.printf("[Mp3Mgr] Task: Starting track %d\n", event.track);
                self->dfPlayer.play(event.track);
            }
        }
    }

}