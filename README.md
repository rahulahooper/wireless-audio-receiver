## Wireless Audio Receiver

<figure style="text-align:center">
  <img src="imgs/system_diagram.png" alt="System Diagram">
  <figcaption>System Diagram</figcaption>
</figure>
<br><br>

This repo is one half of a wireless audio streaming system between two ESP32s. The code here specifically implements the receiving side of the wireless system. This includes forwarding audio data to the amplifier. The goal of this repo and the [transmitting side](https://github.com/rahulahooper/wireless-audio-transmitter) of the wireless system is to stream audio from a guitar to an amplifier without having a physical cable between them. 

The receiver software consists of two parallel threads (or tasks, in FreeRTOS lingo): the receive task and the playback task. 

The receive task, as its name implies, receives audio data from a transmitting ESP32 over [UDP](https://en.wikipedia.org/wiki/User_Datagram_Protocol), using Wifi as the underlying [Layer 2 protocol](https://en.wikipedia.org/wiki/Internet_protocol_suite). The playback task is resonsible for taking that audio data and sending it to an external digital-to-analog converter (DAC). The DAC is a [PCM1753](https://www.ti.com/product/PCM1753), which receives audio data using the [I2S](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/peripherals/i2s.html) protocol. As mentioned before, the two tasks run in parallel, on separate cores. By splitting the receive and the playback tasks into two separate threads that run simultaneously, we ensure that the receiver doesn't miss any Wifi packets, and that audio data is always streaming to the DAC.

### Task Synchronization

The two tasks exchange data with one another using [double-buffering](https://wiki.osdev.org/Double_Buffering). The idea is that the receive task receives and loads audio data into a "back buffer" while the playback task simultaneously streams data out of a "front buffer". By having the two tasks operate on different buffers, they never have to contend for the same data.

When the playback task exhausts all of the data in the "front buffer", it signals to the receive task that it needs new data. The receive task, which periodically polls for this signal, transfers data from the back buffer to the front buffer, then signals to the playback task that new data is available.  

### Bathtubs

<figure style="text-align:center; margin-bottom: 20px;">
  <img src="imgs/bathtub.png" alt="bathtub" width=500>
  <figcaption>A Bathtub</figcaption>
</figure>
<br><br>

The most important goal is to ensure that the audio fed to the amplifier is clean (ie. no distortion or discontinuities). The secondary goal is minimizing latency between the guitar and the amplfier. 

To satisfy the first goal, we must ensure that the receive task always has data to forward to the playback task. If it does not have data, the playback task would either have to stall or play stale data, resulting in a distorted audio signal. The receive task avoids this waiting for several buffer's worth of data to arrive over Wifi before it starts forwarding to the playback task. An analogy I've found useful for reasoning about this stuff is water flowing into and out of a bathtub:


We can think of the data received over Wifi as water flowing into a tub. The water, like the incoming audio data, flows into the tub at a variable rate. Similarly, we can think of data sent to the playback task as water draining out of the tub. This occurs at fixed rate. The tub itself represents the cache that the receive task maintains. The goal of the receive task is to maintain a steady outflow of water at all times - this means that we always want to have data available for the playback task. Because the water flows in at a variable rate, the receive task can only accomplish its goal by keeping just a little bit of water in the tub at all times. However, the more that the receive task caches audio data (ie. the longer the water sits in the tub), the greater the latency of the system.


## Theoretical Latency
To compute the theoretical latency of this wireless system, we need to consider:

  1) The amount of time a sample is cached in the transmitting ESP32 before it is sent to the Wifi driver. Samples are cached while the [sampling task](https://github.com/rahulahooper/wireless-audio-transmitter/tree/main?tab=readme-ov-file#wireless-audio-transmitter) on the transmitting ESP32 assembles an audio packet.
      - A sample is cached for _size_of_audio_packet_ / _sampling_rate_ = 300 / 48000 = 6.25ms
  2)  The time it takes for the ESP32s to communicate the data
      - By having the receiving ESP32 periodically echo packets back to the transmitter, we can compute the round-trip time of the wireless communication. 
      - This has been computed to be roughly 3ms --> 1.5ms for one-way communication
  3) The amount of time a sample is cached in the receiving ESP32 before it is sent to the DAC
      - The receive task caches roughly one additional packet of data. This means that when a sample arrives at the receiving ESP32, there is already about a packet's worth of data in the cache waiting to be sent to the playback task.
      - The approximate amount of time the audio spends in the cache is _size_of_audio_packet_ / _sampling_rate_ = 300 / 48000 = 6.25ms. 

**The theoretical total latency of the audio system, then, is roughly 14ms.** While it should be possible to further reduce the latency by reducing the audio packet size, I personally did not notice the 14ms.


### Circuit

<figure style="text-align:center; margin-bottom: 20px;">
  <img src="imgs/pcb.jpg" alt="PCB" width=500>
    <figcaption>
    The circuit board, with lots of debug wires soldered on (ESP32 is on the back side)
  </figcaption>
</figure>
<br><br><br>

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
