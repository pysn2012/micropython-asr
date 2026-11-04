The following firmware is built for the XiaoCheng ESP32-S3 handheld (16MB flash,
Octal PSRAM, ES7210+ES8311 shared-I2S audio).
boards with 4MiB or more of flash, including WROOM and MINI modules.

This firmware supports configurations with and without SPIRAM (also known as
PSRAM) and will auto-detect a connected SPIRAM chip at startup and allocate
the MicroPython heap accordingly. However if your board has Octal SPIRAM, then
use the "spiram-oct" variant.
