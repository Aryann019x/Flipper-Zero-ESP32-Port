# Board validation status

> Validated DIY targets. Stock boards (t_embed, esp32s3, thormini,
> waveshare_c6_*) follow upstream status.

| Board | Hardware | Status | Notes |
|---|---|---|---|
| `esp32_ili9341_diy` | ESP32-WROOM32 + ILI9341 2.4" + 5-way pad | VALIDATED | Golden reference. Buttons, NRF, SD all confirmed on hardware. |
| `esp32s3_st7789_diy` | ESP32-S3 N16R8 + ST7789 2.8" + XPT2046 touch | VALIDATED | Light tap, SD mount, left/right orientation confirmed on hardware. |

Other DIY boards (ILI9341 family, classic ST7789 touch, 8MB/16MB
quad-PSRAM variants) are experimental and live on the `diy-boards`
branch; they are not shipped here. See `BOARDS_STATUS.md` there for
their status and hardware caveats.
