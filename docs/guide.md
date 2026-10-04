
# RC Information Board (RCIB) Building Guide (started 2026)

This is intended to be a straightforward guide to building your own RC timing board. My own use case is for the board to take in serial data from RC-Timing, pass it straight through unchanged to any existing RC timing boards a club may have (RIDE boards for example in the UK), and then to also use the data to feed timing data to one or more RCIB boards. RCIB is the acronym I give to this project, simply it stands for RC Information Board. It is a wireless RC timing display board system.

I'll split the guide into several smaller sections - I may or may not create other pages with illustrated explanations as the need arises.

## Sections

1. Building the Transmitter
2. Building Receiver Type #1 - WS2812 18cm digits
3. Building Receiver Type #2 - 18cm PCB digits
4. Building Receiver Type #3 - Start Lights

## Section 1- Building the Transmitter

This project utilises the ESP32-S3-DevKitC-1-N16R8 throughout - whenever a microcontroller is necessary, this will be it. If you are intending on building a system based on RCIB, buy these in bulk as it will be cheaper. How many you need will depend on how many parts you are intending to build. You will need at least 2 - one for the transmitter and one for a receiver/display of some sort. Obviously if you need more displays you will need more microcontrollers.

*Hardware*

To build a transmitter, you will require:
1. A ESP32-S3-DevKitC-1-N16R8 microcontoller. These are cheap on eBay and Amazon.
2. A USB-C to USB-A converter to allow you to plug in your existing timing board (if you have one). I would buy one with a fitting to allow you to mount it to an enclosure.
3. An enclosure to house the ESP32 and USB cable/adapter. I have designed an enclosure which you can 3D print. Link here (coming soon).
4. A USB-C cable to connect the ESP32 to the PC running your RC Timing software.

*Software*

You will need to flash the ESP32 with the transmitter code, the latest version can be found here (link coming soon). This serves as both a pass-through for existing timing boards, as well as parsing the timing information for use in RCIB receiver boards.

The easiest way to flash the ESP32 is to just build the code and flash it. To do this you will need EIM (a tool for managing ESP-IDF installations). The instructions [here](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/windows-setup.html) should help you. Ins




<!--stackedit_data:
eyJoaXN0b3J5IjpbLTE2MDU0MDc2MDUsMTc1ODY2OTkwOCwtMT
UxMjk5MjIyOV19
-->