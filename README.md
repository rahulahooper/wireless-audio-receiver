## Wireless Audio Receiver

This repo is one half of a wireless audio streaming system between two ESP32s. The code here specifically implements the receiving side of the wireless system.

The receiver software consists of two parallel threads (or tasks, in FreeRTOS lingo): the receive task and the playback task. 

The receive task, as its name implies, receives audio data from a transmitting ESP32 over UDP, using Wifi as the underlying [Layer 2 protocol](https://en.wikipedia.org/wiki/Internet_protocol_suite). The playback task is resonsible for taking that audio data and sending it to an external digital-to-analog converter (DAC). The DAC is a [PCM1753](https://www.ti.com/product/PCM1753), which receives audio data using the [I2S](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/peripherals/i2s.html) protocol. As mentioned beofre, the two tasks run in parallel, on separate cores. By splitting the receive and the playback tasks into two separate threads, we ensure that no audio packets are missed when receiving over Wifi, and that audio data is always streaming to the DAC.

### Task Synchronization

The two tasks exchange data with one another using [double-buffering](https://wiki.osdev.org/Double_Buffering). The idea is that the receive task places audio data into a "back buffer" while the playback task simultaneously streams data out of a "front buffer". These two buffers are kept separate to avoid contention between the two tasks. When the playback task exhausts all of the data in the "front buffer", it signals to the receive task that it needs new data. The receive task, which periodically polls for this signal, transfers data from the back buffer to the active buffer, then signals to the playback task that new data is available.  

### Circuit

The schematic and layout of the audio receiver are included in the repo. The circuit is responsible for receiving audio data from the ESP32 (specifially, from the playback task), filtering + amplifying the data, and sending it to some kind of speaker. The circuit can drive either a low-impedance speaker (I tested with an off-the-shelf 8-ohm speaker), or it can send data to an actual guitar amplifier.

The circuit has a bunch of switches on it so that we can test things like the ESP32's internal DAC or to bypass the anti-aliasing filter. If configured for "proper" usage, the signal flow through the circuit looks like this:

1. ESP32 streams digital audio data to PCM1753 via I2S
2. PCM1753 converts audio to analog and feeds it to a series of op amp filters
3. The op amp ([OP4134](https://www.ti.com/product/OPA4134) or its cheaper alternative, the [OPA1679](https://www.ti.com/product/OPA1679/part-details/OPA1679IDR)), attenuates and low-pass filters the audio signal.
4. If the audio data is AC-coupled and delivered to the guitar amplifier.
  - If audio data is going to a speaker instead, then it is sent to an LM386 power amplifier and AC-coupled before being sent to the speaker.

### ESP32 DAC Notes

(For reference only, only important to readers who are working with the internal ESP32 DAC).

The ESP32 has an internal digital to analog converter that converts data using DMA. The user specifies the size of the DMA buffer when they first configure the DAC. To write to the DAC, the user calls the function `dac_continuous_write_asynchronously()`, providing both a pointer to the DMA buffer as well as a separate buffer containing new data. The function then copies the new data into the DMA buffer. It is important to note that the _entire_ DMA buffer is processed by the I2S controller. This means that if the new data provided does not fill up the DMA buffer, then the DMA buffer will contain both new data and stale data.  
