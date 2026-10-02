#pragma once

#include <furi.h>

#ifdef __cplusplus
extern "C" {
#endif

void furi_hal_spi_bus_init(void);
void furi_hal_spi_bus_lock(void);
/** Try to take the shared SPI bus lock without blocking (for the 4 ms input
 *  poll: skip the poll instead of stalling a whole display frame behind it).
 *  @return true if the lock was taken (caller must unlock), false if busy */
bool furi_hal_spi_bus_try_lock(void);
void furi_hal_spi_bus_unlock(void);

#ifdef __cplusplus
}
#endif
