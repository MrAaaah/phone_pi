C code for a Raspberry Pi that records wav files using an I2S mic.
- Recording start/stop controlled by a swicth.
- Wifi start/stop using another switch.
- 2 leds: wifi status and recording status.

Developped on a rpi3b+

## libs

WiringPI for the GPIO communication (leds/switchs)
PortAudio to record the i2s mic
