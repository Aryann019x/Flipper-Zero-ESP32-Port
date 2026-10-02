# Board validation status

> DIY targets in this tree. Stock boards (t_embed, esp32s3, thormini,
> waveshare_c6_*) follow upstream status.

| Board | Hardware | Status | Notes |
|---|---|---|---|
| `esp32_ili9341_diy` | ESP32-WROOM32 + ILI9341 2.4" + 5-way pad | VALIDATED | Golden reference. Buttons, NRF, SD all confirmed on hardware. |
| `esp32s3_st7789_diy` | ESP32-S3 N16R8 + ST7789 2.8" + XPT2046 touch | VALIDATED | Light tap, SD mount, left/right orientation confirmed on hardware. |
| `esp32_st7789_diy` | ESP32-WROOM32 + ST7789 2.8" + XPT2046 touch | BETA | NRF, colors, left/right fixed. Touch still needs hard press (panel physics suspected). |
| `esp32s3_ili9341_diy` | ESP32-S3 N16R8 + ILI9341 2.4"/2.8" + XPT2046 touch | BETA | Display confirmed (boots + UI good on 2.4" non-touch). Touch untested — no touch panel on hand. Mirror/BGR inherited, may need one fix round. |
| `esp32s3_ili9341_8mb` | ESP32-S3 8MB-flash + quad-PSRAM (N8R2-type) + ILI9341 touch | BETA — UNTESTED | Same firmware/pins, 8 MB single-app layout (no OTA self-update). Built clean, never booted on hardware. |
| `esp32s3_ili9341_16mb_quad` | ESP32-S3 16MB-flash + quad-PSRAM (N16R2-type) + ILI9341 touch | BETA — UNTESTED | Same firmware/pins, 16 MB dual-OTA layout, quad PSRAM @ 40 MHz. Built clean, never booted on hardware. |
| `esp32s3_st7789_8mb` | ESP32-S3 8MB-flash + quad-PSRAM (N8R2-type) + ST7789 touch | BETA — UNTESTED | Same firmware/pins as validated ST7789 build, 8 MB single-app layout (no OTA). Built clean, never booted on hardware. |
| `esp32s3_st7789_16mb_quad` | ESP32-S3 16MB-flash + quad-PSRAM (N16R2-type) + ST7789 touch | BETA — UNTESTED | Same firmware/pins, 16 MB dual-OTA layout, quad PSRAM @ 40 MHz. Built clean, never booted on hardware. |
| `esp32s3_ili9341_bod` | ESP32-S3 N16R8 + ILI9341 touch (brownout-recovery) | BETA — UNTESTED | Same pins as `esp32s3_ili9341_diy` + `ESP_PHY_REDUCE_TX_POWER` and max TX 14 dBm (verified in build config). For boards that BOD-loop on weak supplies. Costs WiFi range. |

BETA = boots and runs, but has a known caveat or untested feature above.
Do not treat BETA boards as release-ready.

## Tester bootloop note (2026-10-02)
N16R8 tester bootloop on `esp32s3_ili9341_diy` traced via serial log to the
brownout detector (`BOD: Brownout detector was triggered` during WiFi/BLE
init), not to PSRAM/flash config or the display driver. Fix is hardware-side:
stronger 5 V supply, short data cable, unplug radio modules for the test.
`esp32s3_ili9341_bod` is the firmware mitigation for marginal rails.
