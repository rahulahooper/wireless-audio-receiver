#ifndef PLAYBACK_H
#define PLAYBACK_H

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "driver/dac_continuous.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"

#include "common.h"

//////////////////////////////////////////////
///  Functions
//////////////////////////////////////////////


void playback_task_main(void* pvParameters);

void destroy_dac(bool useExternalDac, dac_continuous_handle_t* dacHandle, 
                 QueueHandle_t* dacQueue, i2s_chan_handle_t* i2sHandle);

void setup_i2s(i2s_chan_handle_t* i2sHandle, i2s_chan_handle_t* i2sComHandle, const uint32_t sampleRate);

void setup_dac(QueueHandle_t* queue, dac_continuous_handle_t* dacHandle, const uint32_t sampleRate);

//////////////////////////////////////////////
/// Typedefs 
//////////////////////////////////////////////


typedef struct PlaybackTaskConfig_t
{
    uint32_t sampleRate;            // sampling rate for digital-to-analog converter (Hz)
    bool     useExternalDac;        // if false, configures the ESP32 built-in DAC
    uint32_t dataBitWidth;          // number of bits per audio sample
} PlaybackTaskConfig_t;


#endif  // PLAYBACK_H