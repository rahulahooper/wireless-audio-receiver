////////////////////////////////////////////////////////////////////
///
/// playback.c
///
/// The playback task is responsible for sending audio
/// data to an external DAC (PCM4201). The playback 
/// task exchanges data with the receive task using
/// double-buffering. When it is out of data, the 
/// playback task notifies the receive task and blocks
/// until the new data is available.
///
////////////////////////////////////////////////////////////////////

#include "playback.h"

extern SharedBuffer* activeBuffer;                     // transmitting task transmits this packet  
extern SharedBuffer* backBuffer;                       // sampling task fills this packet         

extern const UBaseType_t playbackDoneNotifyIndex;      // set by the playback task when it is done processing audio data
extern const UBaseType_t dataReadyNotifyIndex;         // set by the receive task when there is new data available for the playback task

extern TaskHandle_t receiveTaskHandle;

////////////////////////////////////////////////////////////////////
// dac_on_convert_done_callback()
//
////////////////////////////////////////////////////////////////////
static bool IRAM_ATTR  dac_on_convert_done_callback(dac_continuous_handle_t handle, const dac_event_data_t *event, void *user_data)
{
    QueueHandle_t que = (QueueHandle_t)user_data;
    BaseType_t need_awoke;
    /* When the queue is full, drop the oldest item */
    if (xQueueIsQueueFullFromISR(que)) {
        dac_event_data_t dummy;
        xQueueReceiveFromISR(que, &dummy, &need_awoke);
    }
    /* Send the event from callback */
    xQueueSendFromISR(que, event, &need_awoke);
    return need_awoke;
}


////////////////////////////////////////////////////////////////////
// setup_dac()
//
// Sets up the internal ESP32 DAC.
////////////////////////////////////////////////////////////////////
void setup_dac(QueueHandle_t* queue, dac_continuous_handle_t* dacHandle, const uint32_t sampleRate)
{
    // Allocate resources for queue
    *queue = xQueueCreate(10, sizeof(dac_event_data_t));
    assert(*queue);

    // Allocate resources for DAC
    dac_continuous_config_t dacConfig = 
    {
        .chan_mask  = DAC_CHANNEL_MASK_CH0,
        .desc_num   = 4,
        .buf_size   = PLAYBACK_TASK_REQ_SAMPLES,    // each sample is 1 byte
        .freq_hz    = sampleRate,
        .offset     = 0,
        .clk_src    = DAC_DIGI_CLK_SRC_APLL,
        .chan_mode  = DAC_CHANNEL_MODE_SIMUL,        // not necessary for our purposes
    };

    ESP_ERROR_CHECK(dac_continuous_new_channels(&dacConfig, dacHandle));

    // Register a callback for when the DAC has converted previously loaded data
    dac_event_callbacks_t dacCallback =
    {
        .on_convert_done = dac_on_convert_done_callback,
        .on_stop         = NULL,
    };

    ESP_ERROR_CHECK(dac_continuous_register_event_callback(*dacHandle, &dacCallback, *queue));
    ESP_ERROR_CHECK(dac_continuous_enable(*dacHandle));
    ESP_ERROR_CHECK(dac_continuous_start_async_writing(*dacHandle));
}


////////////////////////////////////////////////////////////////////
// setup_i2s()
//
////////////////////////////////////////////////////////////////////
void setup_i2s(i2s_chan_handle_t* i2sHandle, i2s_chan_handle_t* i2sComHandle, const uint32_t sampleRate)
{
    // Configure the I2S channel
    i2s_chan_config_t i2sChanConfig = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    i2s_new_channel(&i2sChanConfig, i2sHandle, NULL);

    i2s_std_config_t i2sStdConfig = 
    {
        .clk_cfg = 
        {
            .sample_rate_hz = 48000, //sampleRate,
            .clk_src = I2S_CLK_SRC_APLL,
            .mclk_multiple = I2S_MCLK_MULTIPLE_192,
        },

        .slot_cfg =
        {
            .data_bit_width = I2S_DATA_BIT_WIDTH_24BIT,   // The PCM1753 defaults to 24-bit left-justified I2S. Therefore,
                                                          //  any data we load into the I2S buffer must be zero-extended to be 24-bits
            .slot_bit_width = I2S_DATA_BIT_WIDTH_24BIT, 
            .slot_mode      = I2S_SLOT_MODE_MONO,         // The PCM1753 expects two channels, but we only have one
                                                          //  channel's worth of data. The I2S TX buffer will duplicate
                                                          //  the data onto the second channel.
            .slot_mask      = I2S_STD_SLOT_BOTH,          // The PCM1753 expects two channels
            .ws_width       = I2S_DATA_BIT_WIDTH_24BIT,   // WS should be high for the entirety of a slot
            .bit_shift      = false,                      // The PCM1753 defaults to 24-bit left-justified I2S, which as no bit shift.
            .msb_right      = false,                      // ¯\_(ツ)_/¯

        },

        .gpio_cfg = 
        {
            .mclk = GPIO_NUM_0,
            .bclk = GPIO_NUM_12,
            .ws   = GPIO_NUM_27,
            .dout = GPIO_NUM_14,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags =
            {
                .bclk_inv = false,
                .mclk_inv = false,
                .ws_inv   = true,                       // The PCM1753 asserts WS for the left channel and 
                                                        //  de-asserts WS for the right channel
            }
        }
    };

    // Initialize and enable the channels
    i2s_channel_init_std_mode(*i2sHandle, &i2sStdConfig);
    i2s_channel_enable(*i2sHandle);         

    // The I2S is now running and sending data to the PCM1753

    vTaskDelay(100);
}


////////////////////////////////////////////////////////////////////
// destroy_dac()
//
////////////////////////////////////////////////////////////////////
void destroy_dac(bool useExternalDac, dac_continuous_handle_t* dacHandle, 
                 QueueHandle_t* dacQueue, i2s_chan_handle_t* i2sHandle)
{
    if (useExternalDac)
    {
        // Stop i2s channel
        ESP_ERROR_CHECK(i2s_channel_disable(*i2sHandle));

        // Delete i2s channel
        ESP_ERROR_CHECK(i2s_del_channel(*i2sHandle));
    }
    else
    {
        // Stop dac conversions
        ESP_ERROR_CHECK(dac_continuous_disable(*dacHandle));

        // Free dac resources
        ESP_ERROR_CHECK(dac_continuous_del_channels(*dacHandle));

        // Free queue resouces
        vQueueDelete(*dacQueue);
    }
}


////////////////////////////////////////////////////////////////////
// playback_task_main()
//
////////////////////////////////////////////////////////////////////
void playback_task_main(void* pvParameters)
{
    PlaybackTaskConfig_t playbackTaskConfig = *(PlaybackTaskConfig_t*)pvParameters;

    ESP_LOGI(__func__, "Playback task ready\n");
    while (receiveTaskHandle == NULL)
    {
        ESP_LOGI(__func__, "Waiting for receive task to come up\n");
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    // Setup the digital-to-analog converter (dac)
    QueueHandle_t dacQueue;                 // shared between intenal and external DACs
    dac_continuous_handle_t dacHandle;      // handle for internal DAC
    i2s_chan_handle_t i2sHandle;            // handle for external DAC (PCM1753)
    i2s_chan_handle_t i2sComHandle;
    if (playbackTaskConfig.useExternalDac)
    {
        setup_i2s(&i2sHandle, &i2sComHandle, playbackTaskConfig.sampleRate);
    }
    else
    {
        setup_dac(&dacQueue, &dacHandle, playbackTaskConfig.sampleRate);
    }

    bool error = false;

    while (!error)
    {
        // Notify the receive task that we are waiting for new data
        PRINTF_DEBUG((__func__, "Notifying receive task of playback done\n"));
        xTaskNotifyGiveIndexed(receiveTaskHandle, playbackDoneNotifyIndex);

        // Wait for new data
        while (!ulTaskNotifyTakeIndexed(dataReadyNotifyIndex, pdTRUE, pdMS_TO_TICKS(1000)))
        {
            vTaskDelay(1);
        }

        PRINTF_DEBUG((__func__, "Got data ready notification.\n"));

        // Verify the new data
        //  1. Check whether the playback task has provided enough data to start playback
        //  2. Ensure that the data within the active buffer starts at index 0

        if (activeBuffer->numSamples < PLAYBACK_TASK_REQ_SAMPLES)
        {
            ESP_LOGE(__func__, "Received insufficient data from receive_task: %u / %u bytes\n",
                activeBuffer->numSamples, PLAYBACK_TASK_REQ_SAMPLES);
            continue;
        } 
        else if (activeBuffer->payloadStart != 0)
        {
            ESP_LOGE(__func__, "active buffer data doesn't start at idx 0 (%u). Not playing data\n",
                activeBuffer->payloadStart);
            activeBuffer->numSamples = 0;
            activeBuffer->payloadStart = 0;
            continue;
        }

        // Send data from active buffer to DAC
        if (playbackTaskConfig.useExternalDac)
        {
            i2s_event_data_t eventData;
            (void)eventData;
            assert(activeBuffer->sampleSizeBytes == sizeof(uint32_t));

            esp_err_t ret = ESP_OK;

            int64_t start, stop;
            get_system_time(&start);
            while (activeBuffer->numSamples > PLAYBACK_TASK_REQ_SAMPLES && ret == ESP_OK)
            {
                size_t bytesWritten;
                uint32_t timeoutMs = 100;

                ret = i2s_channel_write(i2sHandle, 
                                  activeBuffer->payload + activeBuffer->payloadStart, 
                                  activeBuffer->numSamples * activeBuffer->sampleSizeBytes, 
                                  &bytesWritten, timeoutMs);
                activeBuffer->payloadStart += bytesWritten;
                activeBuffer->numSamples  -= (bytesWritten >> 2);   // 1 sample = 4 bytes
            }

            if (ret != ESP_OK)
            {
                get_system_time(&stop);
                ESP_LOGE(__func__, "Error during I2s Write: %s. Read %u bytes in %llu microseconds.\n",
                    esp_err_to_name(ret), activeBuffer->payloadStart, (stop - start));
            }

        }
        else
        {
            // Use the internal ESP32 DAC
            dac_event_data_t eventData;
            assert(activeBuffer->sampleSizeBytes == sizeof(uint8_t));

            while (activeBuffer->numSamples > PLAYBACK_TASK_REQ_SAMPLES)
            {
                xQueueReceive(dacQueue, &eventData, portMAX_DELAY);
                size_t loadedBytes = 0;
                ESP_ERROR_CHECK(dac_continuous_write_asynchronously(dacHandle, eventData.buf, eventData.buf_size,
                                                                    &activeBuffer->payload[activeBuffer->payloadStart],
                                                                    activeBuffer->numSamples * activeBuffer->sampleSizeBytes, 
                                                                    &loadedBytes));

                activeBuffer->payloadStart += MIN(loadedBytes, activeBuffer->numSamples);
                activeBuffer->numSamples  -= MIN(loadedBytes, activeBuffer->numSamples);
            }
        }
    }

    // We should never reach here
    destroy_dac(playbackTaskConfig.useExternalDac, &dacHandle, &dacQueue, &i2sHandle);
    vTaskDelete(NULL);
}