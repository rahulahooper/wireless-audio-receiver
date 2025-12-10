/* 
 * util.h
 * 
 * Contains  helper functions and structs that
 * are useful for debuggin.
 * 
 */

#ifndef UTIL_H
#define UTIL_H

#include "esp_err.h"
#include "esp_log.h"

#include "common.h"

////////////////////////////////////////////////////////////////////
// TrafficMeter 
// 
// Light-weight flowmeter for byte traffic. An application can
// update this meter periodically with the bytes it has processed.
// The TrafficMeter will maintain statistics about how many bytes
// have been processed over time, at what interval, etc.
//
////////////////////////////////////////////////////////////////////
typedef struct TrafficMeter
{
    uint64_t totalBytes_;
    int64_t timeNow_;
    int64_t timeElapsed_;
    uint32_t numUpdates_;
    
} TrafficMeter;

void TM_Init(TrafficMeter** tm)
{
    *tm = (TrafficMeter*)malloc(sizeof(TrafficMeter));
}

void TM_Start(TrafficMeter* tm)
{
    memset(tm, 0, sizeof(TrafficMeter));
    get_system_time(&tm->timeNow_);
}

void TM_Update(TrafficMeter* tm, uint32_t bytes)
{
    tm->totalBytes_ += bytes;
    tm->numUpdates_++;

    int64_t prevTime = tm->timeNow_;
    get_system_time(&tm->timeNow_);
    int64_t timeElapsed = (tm->timeNow_ - prevTime);

    tm->timeElapsed_ += timeElapsed;
}

int64_t TM_GetTimeElapsed(TrafficMeter* tm)
{
    return tm->timeElapsed_;
}

float TM_GetAvgTimeBetweenUpdates(TrafficMeter* tm)
{
    return tm->timeElapsed_ / tm->numUpdates_ / 1e6;
}

float TM_GetBytesPerSecond(TrafficMeter* tm)
{
    return 1.0f * tm->totalBytes_ / tm->timeElapsed_ * 1e6;
}

void TM_Print(TrafficMeter* tm, const char* tag)
{
    ESP_LOGI(tag, "%f bytes/sec, %f sec/update",
        TM_GetBytesPerSecond(tm), TM_GetAvgTimeBetweenUpdates(tm));
}

#endif // UTIL_H