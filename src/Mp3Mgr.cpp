#include "Mp3Mgr.h"

bool Mp3Mgr::InitMp3(void)
{
    // 하드웨어 시리얼 초기화 (P4 포트 핀 번호 사용)
    dfpSerial.begin(9600, SERIAL_8N1, DFPLAYER_RX, DFPLAYER_TX);

    // 시리얼 연결 확인 (DFPlayer는 응답이 느릴 수 있어 잠시 대기)
    delay(500);

    if (!dfPlayer.begin(dfpSerial))
    {
        Serial.println("[Mp3Mgr] Failed to initialize DFPlayer Mini!");
        return false;
    }

    Serial.println("[Mp3Mgr] Initialized DFPlayer Mini successfully.");
    dfPlayer.volume(DFPLAYER_VOLUME);

    // 재생할 트랙 번호를 멤버 변수에 저장
    this->_pendingTrack = 1;

    // 이미 재생 태스크가 실행 중이라면 중복 실행 방지 (필요 시 이전 태스크 삭제)
    if (_playTaskHandle != NULL) {
        vTaskDelete(_playTaskHandle);
        _playTaskHandle = NULL;
    }

    // 태스크 생성 (Core 0 할당으로 Display 태스크와 분리)
    xTaskCreatePinnedToCore(
        Mp3Mgr::PlayTask,   // 태스크 함수
        "Mp3PlayTask",      // 태스크 이름
        8192,               // 스택 크기
        this,               // 파라미터로 현재 인스턴스(this) 전달
        4,                  // 우선순위
        &_playTaskHandle,   // 핸들
        1                   // Core 1
    );

    return true;
}

void Mp3Mgr::PlayTask(void* pvParameters)
{
    // 전달받은 포인터를 Mp3Mgr 인스턴스로 형변환
    Mp3Mgr* self = static_cast<Mp3Mgr*>(pvParameters);

    int track = self->_pendingTrack;
    Serial.printf("[Mp3Mgr] Task: Starting track %d\n", track);

    // MP3 재생 명령
    self->dfPlayer.play(track);

    // [중요] 기존 delay(6000)을 vTaskDelay로 교체
    // 이 시간 동안 태스크는 Block 상태가 되어 다른 태스크(GIF 등)에 자원을 양보합니다.
    vTaskDelay(pdMS_TO_TICKS(6000));

    Serial.println("[Mp3Mgr] Task: Sequence finished");

    // 태스크 종료 시 핸들 초기화 및 삭제
    self->_playTaskHandle = NULL;
    vTaskDelete(NULL);
}