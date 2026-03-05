#include "Mp3Mgr.h"
#include "CommonApi.h"

#define TEST_LOG(fmt, ...) UartLogf("[Mp3Mgr] " fmt "\n", ##__VA_ARGS__)
#define TEST_LINE() UartLogf("[Mp3Mgr] %s\n", __func__)
static constexpr int MP3_TRACK_WELCOME = 1;

bool Mp3Mgr::Init(void)
{
    TEST_LINE();
    dfpSerial.begin(9600, SERIAL_8N1, DFPLAYER_RX, DFPLAYER_TX);
    TEST_LOG("dfpSerial.begin done");

    delay(200);

    if (!dfPlayer.begin(dfpSerial))
    {
        Serial.println("[Mp3Mgr] Failed to initialize DFPlayer Mini!");
        TEST_LOG("dfPlayer.begin failed");
        return false;
    }

    Serial.println("[Mp3Mgr] Initialized DFPlayer Mini successfully.");
    dfPlayer.volume(DFPLAYER_VOLUME);
    TEST_LOG("volume=%d", DFPLAYER_VOLUME);

    // Welcome sound must be played once right after successful DFPlayer handshake.
    if (!WelcomePlayed) {
        delay(80);
        dfPlayer.play(MP3_TRACK_WELCOME);
        WelcomePlayed = true;
        TEST_LOG("welcome track played: %d", MP3_TRACK_WELCOME);
    }

    this->_pendingTrack = 1;

    if (taskHandler != NULL) {
        vTaskDelete(taskHandler);
        taskHandler = NULL;
    }

    BaseType_t ret = xTaskCreatePinnedToCore(
        Mp3Mgr::Subscribe,
        "Mp3EventSubscriber",
        4096,
        this,
        4,
        &taskHandler,
        0
    );

    if (ret != pdPASS) {
        Serial.println("[Mp3Mgr] Critical: Mp3EventSubscriber task create failed");
        return false;
    }

    TEST_LOG("Mp3EventSubscriber task created");
    return true;
}

void Mp3Mgr::Subscribe(void* pvParameters)
{
    TEST_LINE();
    Mp3Mgr* self = static_cast<Mp3Mgr*>(pvParameters);
    SystemAPI* system = SystemAPI::getInstance();
    SoundEventData event;

    while(true)
    {
        if(system->soundSubscriber.ReceiveEvent(&event, portMAX_DELAY))
        {
            if(event.type == SOUND_PLAY_TRACK)
            {
                TEST_LOG("Task: Starting track %d", event.track);
                self->dfPlayer.play(event.track);
            }
        }
    }
}
