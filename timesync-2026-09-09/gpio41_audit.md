# GPIO41 board and ownership audit

9 September 2026. The user describes an ESP32-S3 CAM board with microSD behind the ESP module; manufacturer and PCB revision are unknown. This description is not a verified board identity. A matching camera pin preset is also not proof of identical non-camera board wiring.

## Verified project configuration

- Timing strobe: GPIO41. Identify LED: GPIO2. Firmware live mode reports these pins.
- None of the four compiled camera bus/control pin presets assign GPIO41.
- Camera grab and UVC paths deliberately call `strobe_gpio_fire()` on the timing output. That function suppresses camera pulses while `s_mode == STROBE_1HZ` or `s_load_monitor` is true.
- No SDMMC/SD SPI initialization was found in application source. This does not prove the PCB lacks electrical connections.
- `configure_pin()` calls `gpio_reset_pin`, output-direction setup and LOW. MCPWM teardown stops/deletes the generator and calls this function again. The static diagnostic only changes direction/level and is allowed while the strobe is OFF.
- Concurrent hardware control actually occurred: another task put node 1 through OTA/camera tests during the frozen trial. That trial is invalid and cannot diagnose a GPIO ownership bug.

Canonical source: `E:/Projects/wireless-ir-mocap/firmware/ftm_clocksync/main/strobe_gpio.c`, `camera_capture.c`, `uvc_webcam.c`, `app_main.c`; comparison-specific source snapshot is in `firmware/3939b066398cec36/main/`.

## Conditional board-specific evidence

Freenove's official ESP32-S3 WROOM pinout exposes GPIO41 as GPIO/MTDI. The camera uses other pins. The microSD interface is GPIO38 CMD, GPIO39 CLK and GPIO40 DATA. GPIO41 is not marked as an onboard camera or SD signal on that particular board. The official SD tutorial independently uses 38/39/40 and describes the rear microSD slot.

- [Freenove official pinout](https://raw.githubusercontent.com/Freenove/Freenove_ESP32_S3_WROOM_Board/main/ESP32S3_Pinout.png)
- [Freenove SD example](https://docs.freenove.com/projects/fnk0102/en/latest/fnk0102/codes/Main/4_SD_Card_Read_%26_Write_Test.html)

Other boards differ: the Espressif ESP32-S3-EYE schematic groups GPIO41 in the microphone interface. Thus the source preset label "S3-EYE/Freenove" describes compatible camera wiring only, not a universal GPIO41 availability claim.

- [Espressif S3-EYE v2.2 schematic](https://dl.espressif.com/dl/schematics/SCH_ESP32-S3-EYE-MB_20211201_V2.2.pdf)
- [Espressif JTAG pin mapping](https://docs.espressif.com/projects/esp-idf/en/v4.4.1/esp32s3/api-guides/jtag-debugging/configure-other-jtag.html)

## Measurement implication

Constant-level controls before reset contained many late transitions. After the user reset all boards, 16 MS/s LOW had zero transitions across all four channels; HIGH had one initial transition per channel at 128 us. At 8 MS/s, each channel had one initial transition at 256 us in both LOW and HIGH controls, but no later transitions. These controls do not establish whether the earlier cause was firmware state, concurrent control, electrical probing or analyzer acquisition. The subsequent five-second common-build NTP-style capture had 5/5/5/5 raw and qualified pulses; FTM had 6 raw and 5 complete per channel, no rejected pulses (boundary incomplete events).

Conclusion: no separate application-level GPIO41 assignment or confirmed microSD conflict was found. A mode transition/ownership issue or board-specific electrical connection remains possible; exact manufacturer/revision or physical continuity testing is needed to settle PCB routing. Do not infer a permanent fix from reset recovery alone.
