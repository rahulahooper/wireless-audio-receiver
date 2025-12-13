#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "protocol_examples_common.h"
#include "util.h"

#include "common.h"
#include "playback.h"
#include "receive.h"

#define ESP_CORE_0      0       // physical core 0
#define ESP_CORE_1      1       // physical core 1

SharedBuffer* activeBuffer;         // transmitting task transmits this packet
SharedBuffer* backBuffer;           // sampling task fills this packet

const UBaseType_t playbackDoneNotifyIndex = 0;      // set by the playback task when it is done processing audio data
const UBaseType_t dataReadyNotifyIndex = 0;         // set by the receive task when there is new data available for the playback task

TaskHandle_t receiveTaskHandle = NULL;
TaskHandle_t playbackTaskHandle = NULL;


////////////////////////////////////////////////////////////////////
// init_shared_buffers()
//
////////////////////////////////////////////////////////////////////
void init_shared_buffers(bool useExternalDac)
{
    // Initialize the active buffer
    activeBuffer = (SharedBuffer*)malloc(sizeof(SharedBuffer));
    activeBuffer->payloadStart = 0;
    activeBuffer->numSamples = 0;

    // The external dac requires 32-bit samples, the internal dac requires 8-bit samples
    activeBuffer->sampleSizeBytes = useExternalDac ? sizeof(uint32_t) : sizeof(uint8_t);

    activeBuffer->payload = (uint8_t*)malloc(SHARED_BUFFER_MAX_SAMPLES * activeBuffer->sampleSizeBytes);
    memset(activeBuffer->payload, 0, SHARED_BUFFER_MAX_SAMPLES * activeBuffer->sampleSizeBytes);

    // Initialize the back buffer
    backBuffer = (SharedBuffer*)malloc(sizeof(SharedBuffer));
    backBuffer->payloadStart = 0;
    backBuffer->numSamples = 0;

    // The samples we send over Wifi are 24-bit
    backBuffer->sampleSizeBytes = AUDIO_PACKET_BYTES_PER_SAMPLE;

    backBuffer->payload = (uint8_t*)malloc(SHARED_BUFFER_MAX_SAMPLES * backBuffer->sampleSizeBytes);
    memset(backBuffer->payload, 0, SHARED_BUFFER_MAX_SAMPLES * backBuffer->sampleSizeBytes);
}


////////////////////////////////////////////////////////////////////
// app_main()
//
////////////////////////////////////////////////////////////////////
void app_main(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(nvs_flash_init());
    
    PlaybackTaskConfig_t playbackTaskConfig = {
        .sampleRate = 48000,
        .useExternalDac = true,
    };

    ReceiveTaskConfig_t receiveTaskConfig = {
        .streamFromBuffer = false,
        .buffer           = audio_table_500hz,
        .bufferSize       = audio_table_500hz_size,
        .maxPacketTimeoutsPerConnection = 3, 
    };

    init_shared_buffers(playbackTaskConfig.useExternalDac);

    // Create "receive" task
    // 
    BaseType_t receiveTaskStatus = xTaskCreatePinnedToCore(receive_task_main, "receive_task", 8192, &receiveTaskConfig, 5, &receiveTaskHandle, ESP_CORE_0);

    if (receiveTaskStatus != pdPASS)
    {
        ESP_LOGE(__func__, "Failed to create receive task!\n");
        return;
    }

    // Create "playback" task
    //
    BaseType_t playbackTaskStatus = xTaskCreatePinnedToCore(playback_task_main, "playback_task", 8192, &playbackTaskConfig, 5, &playbackTaskHandle, ESP_CORE_1);

    if (playbackTaskStatus != pdPASS)
    {
        ESP_LOGE(__func__, "Failed to create playback task!\n");
        return;
    }

}
