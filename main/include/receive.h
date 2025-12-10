#ifndef RECEIVE_H
#define RECEIVE_H

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_wifi_types_generic.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

#include "common.h"
#include "pure_tones.h"

////////////////////////////////////////////////////////////////////
/// Macros
////////////////////////////////////////////////////////////////////


#define PORT CONFIG_EXAMPLE_PORT
#define EXAMPLE_ESP_WIFI_SSID       "AudioRelayNetwork"
#define EXAMPLE_ESP_WIFI_PASS       "AudioRelayNetworkPassword"
#define EXAMPLE_ESP_WIFI_CHANNEL    1
#define EXAMPLE_MAX_STA_CONN        5

#define WIFI_EVENT_AP_STACONNECTED_BIT        BIT0     // a wifi station connected to this device
#define WIFI_EVENT_AP_STADISCONNECTED_BIT     BIT1     // a wifi station disconnected from this device

#define AUDIO_PACKET_BYTES_PER_SAMPLE 3                // size of each sample within an audio packet in bytes


////////////////////////////////////////////////////////////////////
/// Typedefs
////////////////////////////////////////////////////////////////////


typedef enum ReceiveTaskState_t
{
    RECEIVE_TASK_STATE_SETUP_WIFI_DRIVER,        // Setup Wifi driver
    RECEIVE_TASK_STATE_SETUP_SERVER,             // Set up UDP socket server
    RECEIVE_TASK_STATE_WAIT_FOR_CLIENT,          // Wait for a client to connect
    RECEIVE_TASK_STATE_RECEIVE_FROM_CLIENT,      // Stream data from the client
    RECEIVE_TASK_STATE_STREAM_FROM_BUFFER,       // (For debug) Stream data from a buffer
} ReceiveTaskState_t;

typedef struct ReceiveTaskConfig_t
{
    bool             streamFromBuffer;        // if true, get from audio data from buffer; if false, get from client ESP32
    const int32_t*   buffer;                  // buffer to get audio from if .streamFromBuffer is true
    uint32_t         bufferSize;              // size of buffer

    SharedBuffer* activeBuffer;
    SharedBuffer* backBuffer;

    uint8_t  maxPacketTimeoutsPerConnection;        // number of timeouts before we assume the client disconnected

} ReceiveTaskConfig_t;

typedef struct AudioPacket_t
{
    uint16_t seqnum;
    bool     echo;                  // client requested an echo from server
    uint16_t numSamples;            // number of 16-bit samples in payload
    uint16_t payloadStart;
    uint16_t checksum;              // crc-16
    uint8_t  payload[AUDIO_PACKET_MAX_SAMPLES * AUDIO_PACKET_BYTES_PER_SAMPLE];
} AudioPacket_t;

static const size_t AUDIO_PACKET_HEADER_SIZE = sizeof(AudioPacket_t) - AUDIO_PACKET_MAX_SAMPLES * AUDIO_PACKET_BYTES_PER_SAMPLE;


////////////////////////////////////////////////////////////////////
///  Functions
////////////////////////////////////////////////////////////////////


////////////////////////////////////////////
// wifi_setup_driver()
//
////////////////////////////////////////////
esp_err_t wifi_setup_driver(wifi_init_config_t* cfg);


////////////////////////////////////////////
// _wifi_softap_start()
//
////////////////////////////////////////////
esp_err_t _wifi_softap_start();


////////////////////////////////////////////
// setup_server()
//
////////////////////////////////////////////
esp_err_t setup_server(int addr_family, struct sockaddr_in* dest_addr, int* sockFd);


////////////////////////////////////////////
// copy_audio_packet_to_back_buffer()
//
// Helper function for copying received audio data
// to the background buffer.
////////////////////////////////////////////
void copy_audio_packet_to_back_buffer(const AudioPacket_t* audioPacket, SharedBuffer* backBuffer, bool* overflow);


////////////////////////////////////////////
// copy_back_buffer_to_active_buffer()
//
// Transfers contents of back buffer to active buffer.
// (the function name is kind of misleading, since the 
// data is *moved*, not just copied)
////////////////////////////////////////////
void copy_back_buffer_to_active_buffer();


////////////////////////////////////////////
// validate_audio_packet()
//
// Verifies the CRC of an audio packet and checks
// for any missed packets. 
////////////////////////////////////////////
void validate_audio_packet(AudioPacket_t* audioPacket, uint16_t* expectedSeqnum, bool* isValid);


////////////////////////////////////////////
// stream_from_buffer()
//
// For debugging purposes. Instead of streaming data from a client
// over Wifi, just stream data from a local buffer that lives on
// this ESP32.
////////////////////////////////////////////
esp_err_t stream_from_buffer(const int32_t* buffer, const uint32_t bufferSize);


////////////////////////////////////////////
// stream_from_client()
//
////////////////////////////////////////////
esp_err_t stream_from_client(const int sock, const int maxPacketTimeouts);


////////////////////////////////////////////
// receive_task_main()
//
////////////////////////////////////////////
void receive_task_main(void *pvParameters);



#endif // RECEIVE_H