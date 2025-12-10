#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>
#include <sys/time.h>
#include <sys/param.h>

#include "esp_log.h"
#include "esp_check.h"

#define TAG __func__

#define DEBUG 0
#if DEBUG
    #define PRINTF_DEBUG( msg ) ESP_LOGI msg
#else
    #define PRINTF_DEBUG( msg )
#endif

#define AUDIO_PACKET_MAX_SAMPLES          300                   // maximum amount of audio data we can receive from client at a time
#define PLAYBACK_TASK_REQ_SAMPLES         AUDIO_PACKET_MAX_SAMPLES
#define PLAYBACK_TASK_DESIRED_SAMPLES     (uint16_t)(PLAYBACK_TASK_REQ_SAMPLES * 1.5)
#define SHARED_BUFFER_MAX_SAMPLES         (12 * AUDIO_PACKET_MAX_SAMPLES)     // SharedBuffer is guaranteed to be able to accommodate this many samples

////////////////////////////////////////////////////////////////////
// struct SharedBuffer 
//
////////////////////////////////////////////////////////////////////
typedef struct SharedBuffer
{
    uint16_t payloadStart;
    uint16_t numSamples;                   // number of audio samples within the payload (TODO: rename to numSamples)
    uint16_t sampleSizeBytes;              // the size of each sample within the payload, in bytes
    uint8_t* payload;
} SharedBuffer;

////////////////////////////////////////////////////////////////////
// get_system_time()
//
////////////////////////////////////////////////////////////////////
static esp_err_t get_system_time(int64_t* time_us)
{
    struct timeval tv_now;
    gettimeofday(&tv_now, NULL);
    *time_us = (int64_t)tv_now.tv_sec * 1000000L + (int64_t)tv_now.tv_usec;
    
    return ESP_OK;
}

#endif // COMMON_H
