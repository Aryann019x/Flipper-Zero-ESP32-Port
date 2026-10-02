/* Minimal u8g2 tile-buffer allocator for the Flipper ESP32 port.
 *
 * The full upstream u8g2_d_memory.c carries ~100 static framebuffers
 * (~132 KB .bss, all KEEP()ed by the ESP-IDF linker) for displays this
 * firmware never drives. The only allocator the port uses is u8g2_m_16_8_f
 * (128x64 full-frame, 1024 bytes) via u8g2_glue.c (u8g2_Setup_st756x_flipper).
 *
 * This file provides exactly that symbol. u8g2_d_memory.c stays in the tree
 * untouched for reference; CMakeLists.txt selects this file instead.
 * Saves ~131 KB of static DRAM — required to fit classic ESP32 (WROOM32).
 */

#include <stdint.h>

uint8_t* u8g2_m_16_8_f(uint8_t* page_cnt);
uint8_t* u8g2_m_16_8_f(uint8_t* page_cnt) {
    static uint8_t buf[1024];
    *page_cnt = 8;
    return buf;
}
