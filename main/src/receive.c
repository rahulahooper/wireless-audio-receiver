//////////////////////////////////////////////////////////////
///
/// receive_task.c
///
/// The receive task is responsible for receiving audio
/// packets from a Wifi client. This task periodically
/// exchanges data with the playback task using double-
/// buffering. Specifically, the receive task loads audio
/// data into a "backBuffer". When the playback task signals 
/// that it needs new data, the receive task moves data 
/// from the back buffer to the "activeBuffer". It then 
/// signals to the playback task that new data is available.
/// 
//////////////////////////////////////////////////////////////

#include "receive.h"

extern SharedBuffer* activeBuffer;                     // transmitting task transmits this packet  
extern SharedBuffer* backBuffer;                       // sampling task fills this packet         

extern const UBaseType_t playbackDoneNotifyIndex;      // set by the playback task when it is done processing audio data
extern const UBaseType_t dataReadyNotifyIndex;         // set by the receive task when there is new data available for the playback task

extern TaskHandle_t playbackTaskHandle;

static ReceiveTaskState_t receiveTaskState;

static EventGroupHandle_t s_wifi_event_group;          // FreeRTOS event group to signal when a client is connected or disconnected

////////////////////////////////////////////////////////////////////
// wifi_setup_driver()
//
////////////////////////////////////////////////////////////////////
esp_err_t wifi_setup_driver(wifi_init_config_t* cfg)
{
    ESP_LOGI(__func__, "Setting up WiFi driver\n");        

    esp_netif_create_default_wifi_sta();            // Setup wifi station for SNTP connection
    esp_netif_create_default_wifi_ap();             // Setup wifi access point

    ESP_ERROR_CHECK(esp_wifi_init(cfg));

    receiveTaskState = RECEIVE_TASK_STATE_SETUP_SERVER;

    return ESP_OK;
}


////////////////////////////////////////////////////////////////////
// wifi_event_handler()
//
////////////////////////////////////////////////////////////////////
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                    int32_t event_id, void* event_data)
{
    if (event_base != WIFI_EVENT && event_base != IP_EVENT)
    {
        ESP_LOGE(__func__, "received an event that it isn't supposed to handle: %s\n", event_base);
        return;
    }

    ESP_LOGI(__func__, "Handling event %ld\n", event_id);
    switch (event_id)
    {
        case WIFI_EVENT_AP_STACONNECTED:
        {
            wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
            ESP_LOGI(__func__, "station "MACSTR" join, AID=%d",
                    MAC2STR(event->mac), event->aid);

            xEventGroupSetBits(s_wifi_event_group, WIFI_EVENT_AP_STACONNECTED_BIT);
            break;
        } 
        case WIFI_EVENT_AP_STADISCONNECTED:
        {
            wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
            ESP_LOGI(__func__, "station "MACSTR" leave, AID=%d, reason=%d",
                    MAC2STR(event->mac), event->aid, event->reason);

            receiveTaskState = RECEIVE_TASK_STATE_WAIT_FOR_CLIENT;
            
            // TODO: Need to check if there are any resources we need to clean up when
            //       a client disconnects (ie. socket descriptors, Wifi resources, etc.)
            xEventGroupSetBits(s_wifi_event_group, WIFI_EVENT_AP_STADISCONNECTED_BIT);
            break;
        }
        case WIFI_EVENT_STA_START:
        {
            ESP_ERROR_CHECK(esp_wifi_connect());
            break;
        }
    }
}


////////////////////////////////////////////////////////////////////
// _wifi_softap_start()
//
////////////////////////////////////////////////////////////////////
esp_err_t _wifi_softap_start()
{
    // esp_netif_create_default_wifi_ap();

    // wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    // ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = EXAMPLE_ESP_WIFI_SSID,
            .ssid_len = strlen(EXAMPLE_ESP_WIFI_SSID),
            .channel = EXAMPLE_ESP_WIFI_CHANNEL,
            .password = EXAMPLE_ESP_WIFI_PASS,
            .max_connection = EXAMPLE_MAX_STA_CONN,
#ifdef CONFIG_ESP_WIFI_SOFTAP_SAE_SUPPORT
            .authmode = WIFI_AUTH_WPA3_PSK,
            .sae_pwe_h2e = WPA3_SAE_PWE_HUNT_AND_PECK,
#else
            .authmode = WIFI_AUTH_WPA2_PSK,
#endif
            .pmf_cfg = {
                    .required = true,
            },
        },
    };

    if (strlen(EXAMPLE_ESP_WIFI_PASS) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(__func__, "Wifi access point created. SSID:%s password:%s channel:%d",
            EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS, EXAMPLE_ESP_WIFI_CHANNEL);
    
    return ESP_OK;
}


////////////////////////////////////////////////////////////////////
// setup_server()
//
////////////////////////////////////////////////////////////////////
esp_err_t setup_server(int addr_family, struct sockaddr_in* dest_addr, int* sockFd)
{

    ESP_ERROR_CHECK(_wifi_softap_start());

    // Set up socket address information
    if (addr_family != AF_INET) {
        ESP_LOGE(__func__, "Only supporting IPV4 addresses. Received %u\n", addr_family);
        return ESP_ERR_INVALID_ARG;
    }

    dest_addr->sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr->sin_family = AF_INET;
    dest_addr->sin_port = htons(PORT);
    int ip_protocol = IPPROTO_IP;

    int sock = socket(addr_family, SOCK_DGRAM, ip_protocol);

    if (sock < 0) {
        ESP_LOGE(__func__, "Unable to create socket: errno %d", errno);
    }

    ESP_LOGI(__func__, "Socket created!!\n");

#if defined(CONFIG_LWIP_NETBUF_RECVINFO) && !defined(CONFIG_EXAMPLE_IPV6)
    int enable = 1;
    lwip_setsockopt(sock, IPPROTO_IP, IP_PKTINFO, &enable, sizeof(enable));
#endif

#if defined(CONFIG_EXAMPLE_IPV4) && defined(CONFIG_EXAMPLE_IPV6)
    if (addr_family == AF_INET6) {
        // Note that by default IPV6 binds to both protocols, it is must be disabled
        // if both protocols used at the same time (used in CI)
        int opt = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, &opt, sizeof(opt));
    }
#endif

    // Set timeout
    struct timeval timeout;
    timeout.tv_sec = 3;
    timeout.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);

    int err = bind(sock, (struct sockaddr *)dest_addr, sizeof(struct sockaddr));
    if (err < 0) {
        ESP_LOGE(__func__, "Socket unable to bind: errno %d, err %d", errno, err);
        return -1;
    }
    ESP_LOGI(__func__, "Socket bound, port %d", PORT);

    *sockFd = sock;
    receiveTaskState = RECEIVE_TASK_STATE_WAIT_FOR_CLIENT;
    return ESP_OK;
}


////////////////////////////////////////////////////////////////////
// copy_to_back_buffer()
//
// Helper function for copying received audio data
// to the background buffer.
////////////////////////////////////////////////////////////////////
void copy_audio_packet_to_back_buffer(const AudioPacket_t* audioPacket, SharedBuffer* backBuffer, bool* overflow)
{
    assert(backBuffer->payloadStart == 0);
    assert(audioPacket->payloadStart == 0);
    assert(audioPacket->numSamples < SHARED_BUFFER_MAX_SAMPLES);

    // Packets sent by the client should be aligned to 2-byte (16-bit) boundary
    assert((audioPacket->payloadStart & 0x1) == 0);

    // Clear out the background buffer if the audioPacket will cause it to overflow
    // This shouldn't technically be possible since the playback task should drain 
    // this buffer long before it overflows
    if (audioPacket->numSamples + backBuffer->numSamples > SHARED_BUFFER_MAX_SAMPLES)
    {
        ESP_LOGI(__func__, "background buffer would overflow (%u + %u > %u). Draining buffer...\n",
            audioPacket->numSamples, backBuffer->numSamples, SHARED_BUFFER_MAX_SAMPLES);
        uint16_t samplesToDrain = audioPacket->numSamples;
        uint32_t bytesToDrain = samplesToDrain * backBuffer->sampleSizeBytes;

        backBuffer->numSamples -= samplesToDrain;
        memmove(backBuffer->payload, backBuffer->payload + bytesToDrain, backBuffer->numSamples * backBuffer->sampleSizeBytes);
        
        if (overflow)
        {
            *overflow = true;
        }
    }
    else if (overflow)
    {
        *overflow = false;
    }

    memcpy(&backBuffer->payload[backBuffer->numSamples * backBuffer->sampleSizeBytes],      // append to the existing samples in the back buffer
            &audioPacket->payload[audioPacket->payloadStart],                               // copy from wherever the data starts in the received audio packet
            audioPacket->numSamples * AUDIO_PACKET_BYTES_PER_SAMPLE);                       // copy all the samples in the received audio packet

    backBuffer->numSamples += audioPacket->numSamples;

    // We've already confirmed this, but just to double check, 
    // ensure that the back buffer hasn't overflowed
    
    assert(backBuffer->numSamples <= SHARED_BUFFER_MAX_SAMPLES);
}


////////////////////////////////////////////////////////////////////
// copy_back_buffer_to_active_buffer()
//
// Transfers contents of back buffer to active buffer.
// (the function name is kind of misleading, since the 
// data is *moved*, not just copied)
////////////////////////////////////////////////////////////////////
void copy_back_buffer_to_active_buffer()
{
    // if the active buffer is too full to accommodate the back buffer,
    // clear the contents of the active buffer
    if ((activeBuffer->numSamples + backBuffer->numSamples) > SHARED_BUFFER_MAX_SAMPLES)
    {
        ESP_LOGE(__func__, "back buffer too large to copy into active buffer, resetting active buffer");
        ESP_LOGE(__func__, "active buffer size = %u, back buffer size = %u, limit = %u\n",
            activeBuffer->numSamples, backBuffer->numSamples, SHARED_BUFFER_MAX_SAMPLES);

        activeBuffer->payloadStart = 0;
        activeBuffer->numSamples = 0;
    }

    // Now there should be enough space to accommodate the back buffer
    assert((activeBuffer->numSamples + backBuffer->numSamples) <= SHARED_BUFFER_MAX_SAMPLES);

    // Move any leftover data in the active buffer to the beginning of the active buffer
    PRINTF_DEBUG((__func__, "BEFORE: active payload start = %u, size = %u, back buffer size = %u\n", 
        activeBuffer->payloadStart, activeBuffer->numSamples, backBuffer->numSamples));

    memmove(&activeBuffer->payload[0], 
            &activeBuffer->payload[activeBuffer->payloadStart], 
            activeBuffer->numSamples * activeBuffer->sampleSizeBytes);

    activeBuffer->payloadStart = 0;

    assert(backBuffer->payloadStart == 0);

    // Copy background buffer data into the active buffer

    // If we are writing to the external DAC, the samples need to be
    // resized to 32-bits. 
    if (activeBuffer->sampleSizeBytes == sizeof(uint32_t))
    {
        uint padding = (activeBuffer->sampleSizeBytes - backBuffer->sampleSizeBytes);
        assert(padding < activeBuffer->sampleSizeBytes);

        for (int i = 0; i < backBuffer->numSamples; i++)
        {
            uint activeBufferIdx = (activeBuffer->numSamples + i) * activeBuffer->sampleSizeBytes;
            uint backBufferIdx   = i * backBuffer->sampleSizeBytes;

            // zero out the current sample in the active buffer
            memset(activeBuffer->payload + activeBufferIdx, 0, activeBuffer->sampleSizeBytes); 

            // replace with the current sample in the back buffer
            memcpy(activeBuffer->payload + activeBufferIdx + padding, backBuffer->payload + backBufferIdx, backBuffer->sampleSizeBytes);
        }
    }

    // If we are writing to the internal DAC, the samples need to be 
    // resized to 8-bits. 
    else if (activeBuffer->sampleSizeBytes == sizeof(uint8_t))
    {
        for (int i = 0; i < backBuffer->numSamples; i++)
        {
            uint8_t* dst = (uint8_t*)&activeBuffer->payload[activeBuffer->numSamples + i * activeBuffer->sampleSizeBytes];
            uint16_t* src = (uint16_t*)&backBuffer->payload[i * backBuffer->sampleSizeBytes];

            *dst = *src & 0xFF;
        }
    }

    activeBuffer->numSamples += backBuffer->numSamples;

    backBuffer->numSamples = 0;
    backBuffer->payloadStart = 0;

    PRINTF_DEBUG((__func__, "AFTER: active payload start = %u, size = %u, back buffer size = %u\n", 
        activeBuffer->payloadStart, activeBuffer->numSamples, backBuffer->numSamples));
}


////////////////////////////////////////////////////////////////////
// validate_audio_packet()
//
// Verifies the CRC of an audio packet and checks
// for any missed packets. 
////////////////////////////////////////////////////////////////////
void validate_audio_packet(AudioPacket_t* audioPacket, uint16_t* expectedSeqnum, bool* isValid)
{
    // verify the CRC of the audio packet
    bool     packetDoesNotWrap = (audioPacket->payloadStart == 0);
    *isValid = packetDoesNotWrap;

    if (!(*isValid))
    {
        ESP_LOGE(__func__, "Invalid packet (seqnum = %u)\n", audioPacket->seqnum); 
        return;
    }

    // Check if a packet was duplicated or dropped 
    if (*expectedSeqnum < audioPacket->seqnum)
    {
        // A packet got dropped in transit
        ESP_LOGI(__func__, "SERVER BEHIND: expected seqnum %u, received packet seqnum %u\n",
            *expectedSeqnum, audioPacket->seqnum);
        *expectedSeqnum = audioPacket->seqnum + 1;
    }
    else if (*expectedSeqnum > audioPacket->seqnum)
    {
        if (*expectedSeqnum - audioPacket->seqnum > UINT16_MAX / 2)
        {
            // On the client size, the seqnum overflowed
            ESP_LOGI(__func__, "SERVER BEHIND / CLIENT WRAPPED: expected seqnum %u, received packet seqnum %u\n",
                *expectedSeqnum, audioPacket->seqnum);
            *expectedSeqnum = audioPacket->seqnum + 1;
        }
        else
        {
            // Client sent a packet that the server has already seen
            ESP_LOGI(__func__, "SERVER AHEAD: expected seqnum %u, received packet seqnum %u\n",
                *expectedSeqnum, audioPacket->seqnum);
            *isValid = false;
        }
    }
    else
    {
        *expectedSeqnum += 1;
    }
}

////////////////////////////////////////////////////////////////////
// stream_from_buffer()
//
// For debugging purposes. Instead of streaming data from a client
// over Wifi, just stream data from a local buffer that lives on
// this ESP32.
////////////////////////////////////////////////////////////////////
esp_err_t stream_from_buffer(const int32_t* buffer, const uint32_t bufferSize)
{
    // Wait for the playback task to come up

    while (playbackTaskHandle == NULL)
    {
        ESP_LOGI(__func__, "Waiting for playback task to come up\n");
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    // Initialize background buffer

    backBuffer->numSamples = 0;
    backBuffer->payloadStart = 0;

    // Read data from buffer in a continuous loop

    uint32_t idx = 0;

    while (true)
    {

        // Copy sample data into background buffer, extending sample data 
        // from 8-bits to 24-bits in the process (the playback task expects
        // 24-bit audio samples);
        
        // Confirm that we aren't about to overflow the back buffer
        assert(PLAYBACK_TASK_DESIRED_SAMPLES < SHARED_BUFFER_MAX_SAMPLES);

        for (int i = 0; i < PLAYBACK_TASK_DESIRED_SAMPLES; i++)
        {
            // The ESP32 is little-endian. The memcpy below copies the 3 LSBs of the sample,
            // which is what we want, since the audio samples stored in the buffer are really
            // 24-bit signed integers that have been sign-extended to 32-bits.
            int32_t sample = buffer[idx];
            memcpy(backBuffer->payload + i * backBuffer->sampleSizeBytes, &sample, backBuffer->sampleSizeBytes);

            idx = (idx + 1) % bufferSize;
        }

        backBuffer->numSamples = PLAYBACK_TASK_DESIRED_SAMPLES;

        while(!ulTaskNotifyTakeIndexed(playbackDoneNotifyIndex, pdTRUE, 10));

        // At this point the playback task is waiting for new data. 
        // Copy sample data into the active buffer
        copy_back_buffer_to_active_buffer();
    
        assert(backBuffer->numSamples == 0);        // background buffer should now be empty
        assert(backBuffer->payloadStart == 0);      
        assert(activeBuffer->payloadStart == 0);    // data in active buffer should start at index 0

        // Signal to the playback task that new data is available
        xTaskNotifyGiveIndexed(playbackTaskHandle, dataReadyNotifyIndex);
    }

    // We should never get here

    return ESP_OK;
}

////////////////////////////////////////////////////////////////////
// stream_from_client()
//
////////////////////////////////////////////////////////////////////
esp_err_t stream_from_client(const int sock, const int maxPacketTimeouts)
{
    struct sockaddr_storage client_addr;
    socklen_t sockaddr_len = sizeof(client_addr);

    AudioPacket_t audioPacket;

    bool isFirstPacket = true;      // are we waiting on the first packet from the client?
    bool isFirstBufSwap = true;     // are we about to swap buffers with the playback task for the first time?
    uint16_t expectedSeqnum = 0;

    char client_addr_str[128];
    memset(client_addr_str, 0, sizeof(client_addr_str));

    uint32_t numPacketTimeouts = 0;

    while (playbackTaskHandle == NULL)
    {
        ESP_LOGI(__func__, "Waiting for playback task to come up\n");
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    bool error = false;
    while (!error)
    {

        uint32_t minSamplesNeededBeforeSwap = isFirstBufSwap ? PLAYBACK_TASK_DESIRED_SAMPLES : PLAYBACK_TASK_REQ_SAMPLES;

        // Keep receiving data into the background buffer until 
        //   a) the receive task has accumulated sufficient data
        //   b) the playback task signals that it is ready for new data
        while((backBuffer->numSamples < minSamplesNeededBeforeSwap) || !ulTaskNotifyTakeIndexed(playbackDoneNotifyIndex, pdTRUE, 0))
        {
            // try to receive a packet

            memset(&audioPacket, 0, sizeof(audioPacket));
            
            int len = recvfrom(sock, &audioPacket, sizeof(AudioPacket_t), 0, (struct sockaddr*)&client_addr, &sockaddr_len);
            
            // check if we failed to receive the packet
            // if this occurs enough times, we assume the client got disconnected

            if (len < 0)
            {
                if (errno == EWOULDBLOCK && ++numPacketTimeouts < maxPacketTimeouts)
                {
                    ESP_LOGE(__func__, "Socket timeout (%lu / %u)\n", numPacketTimeouts, maxPacketTimeouts);
                    expectedSeqnum = 0;
                    continue;
                }
                else
                {
                    ESP_LOGE(__func__, "Socket read failure: errno %d (%s)", errno, strerror(errno));
                    error = true;
                    break;
                }
            }    
            numPacketTimeouts = 0;
        
            // If this is the first packet, cache the client IP address in case we want to echo the packet back
            // TODO: Can we get the IP address in wait_for_client()?
            if (isFirstPacket)
            {
                isFirstPacket = false;

                // We found a new client, print out the IP address
                inet_ntoa_r(((struct sockaddr_in *)&client_addr)->sin_addr, client_addr_str, sizeof(client_addr_str) - 1);
                client_addr_str[sizeof(client_addr_str)-1] = 0;

                ESP_LOGI(__func__, "Connected to client with IP address %s\n", client_addr_str);
            }

            int64_t timerecv;
            get_system_time(&timerecv);

            // validate packet crc
            bool isValid = false;
            validate_audio_packet(&audioPacket, &expectedSeqnum, &isValid);

            if (!isValid) 
            {
                ESP_LOGE(__func__, "Received packet with expected seq num %u was invalid\n", expectedSeqnum-1);
                continue;
            }

            PRINTF_DEBUG((__func__, "Successfully received packet with checksum 0x%x, seqnum %u, payload size %u\n", 
                audioPacket.checksum, audioPacket.seqnum, audioPacket.numSamples));
            
            // Copy packet into the background buffer
            bool overflow = false;
            copy_audio_packet_to_back_buffer(&audioPacket, backBuffer, &overflow);

            PRINTF_DEBUG((__func__, "back buffer size = %u\n", backBuffer->numSamples));

            // if the client requested it, echo the packet back
            // TODO: Defer this to immediately after we have 
            //       provided new data to the playback task
            if (audioPacket.echo)
            {

                ESP_LOGI(__func__, "Echoing back packet with checksum 0x%x, seqnum %u, payload size %u.\n", 
                    audioPacket.checksum, audioPacket.seqnum, audioPacket.numSamples);

                int err = sendto(sock, (void*)&audioPacket, AUDIO_PACKET_HEADER_SIZE, 0, (struct sockaddr*)&client_addr, sockaddr_len);

                if (err < 0)
                {
                    ESP_LOGE(__func__, "Error sending sending response to client at address %s. errno = %s\n", client_addr_str, strerror(errno));
                    error = true;
                }
            }
        }

        if (error) 
        {
            break;
        }

        // At this point the playback task is blocked waiting for new data. 
        copy_back_buffer_to_active_buffer();
        
        assert(backBuffer->numSamples == 0);        // background buffer should now be empty
        assert(backBuffer->payloadStart == 0);      
        assert(activeBuffer->payloadStart == 0);    // data in active buffer should start at index 0

        isFirstBufSwap = false;

        // Signal to the playback task that new data is available
        xTaskNotifyGiveIndexed(playbackTaskHandle, dataReadyNotifyIndex);
    }

    // We should only be here if the client got disconnected

    return ESP_OK;
}


////////////////////////////////////////////////////////////////////
// receive_task_main()
//
////////////////////////////////////////////////////////////////////
void receive_task_main(void *pvParameters)
{
    ReceiveTaskConfig_t receiveTaskConfig = *(ReceiveTaskConfig_t*)pvParameters;
    ESP_LOGI(__func__, "Receive task ready\n");

    int addr_family = AF_INET;
    struct sockaddr_in dest_addr;       // server IP address

    int sock;                           // socket file descriptor

    s_wifi_event_group = xEventGroupCreate();
    xEventGroupClearBits(s_wifi_event_group, WIFI_EVENT_AP_STACONNECTED_BIT | WIFI_EVENT_AP_STADISCONNECTED_BIT);

    // Initialize receive task state machine
    if (receiveTaskConfig.streamFromBuffer)
    {
        receiveTaskState = RECEIVE_TASK_STATE_STREAM_FROM_BUFFER;
    }
    else
    {
        receiveTaskState = RECEIVE_TASK_STATE_SETUP_WIFI_DRIVER;     
    }

    // Handle receive task state machine

    while (1) 
    {

        switch(receiveTaskState)
        {
            case RECEIVE_TASK_STATE_SETUP_WIFI_DRIVER:
            {
                ESP_LOGI(__func__, "RECEIVE_TASK_STATE_SETUP_WIFI_DRIVER\n");
                wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
                wifi_setup_driver(&cfg);
                break;
            } 
            case RECEIVE_TASK_STATE_SETUP_SERVER:
            {
                ESP_LOGI(__func__, "RECEIVE_TASK_STATE_SETUP_SERVER\n");
                ESP_ERROR_CHECK( setup_server(addr_family, &dest_addr, &sock) );
                break;
            }
            case RECEIVE_TASK_STATE_WAIT_FOR_CLIENT:
            {
                ESP_LOGI(__func__, "RECEIVE_TASK_STATE_WAIT_FOR_CLIENT\n");
                EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, 
                                                    WIFI_EVENT_AP_STACONNECTED_BIT,
                                                    pdTRUE, 
                                                    pdFALSE, 
                                                    1000 / portTICK_PERIOD_MS);
                
                if (bits & WIFI_EVENT_AP_STACONNECTED_BIT)
                {
                    ESP_LOGI(__func__, "Client connected!\n");
                    receiveTaskState = RECEIVE_TASK_STATE_RECEIVE_FROM_CLIENT;
                }
                break;
            }
            case RECEIVE_TASK_STATE_RECEIVE_FROM_CLIENT:
            {
                ESP_LOGI(__func__, "RECEIVE_TASK_STATE_RECEIVE_FROM_CLIENT\n");
                // This only returns if a client disconnects
                ESP_ERROR_CHECK(stream_from_client(sock, receiveTaskConfig.maxPacketTimeoutsPerConnection));
                
                receiveTaskState = RECEIVE_TASK_STATE_WAIT_FOR_CLIENT;
                break;
            }

            // for debug
            case RECEIVE_TASK_STATE_STREAM_FROM_BUFFER:
            {
                ESP_LOGI(__func__, "RECEIVE_TASK_STATE_STREAM_FROM_BUFFER\n");
                const int32_t* buffer = receiveTaskConfig.buffer;
                const uint32_t bufferSize = receiveTaskConfig.bufferSize;

                assert(buffer != NULL);
                assert(bufferSize > 0);

                ESP_ERROR_CHECK(stream_from_buffer(buffer, bufferSize));
                break;
            }
            default:
            {
                ESP_LOGE(__func__, "Entered unknown state %u\n", receiveTaskState);
                break;
            }
        }
    }
    vTaskDelete(NULL);
}

