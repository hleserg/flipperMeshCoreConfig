/*
 * UART layer: a thin wrapper over furi_hal_serial.
 *
 * Knows nothing about MeshCore — it moves bytes. Pins 13 (TX) / 14 (RX) /
 * 18 (GND), 115200 8N1, which is USART1 on the Flipper (FuriHalSerialIdUsart).
 *
 * RX arrives in interrupt context and is parked in a stream buffer, so
 * meshcore_uart_rx() blocks the *calling* thread, never the GUI thread.
 * Call it from a worker thread only.
 */
#pragma once

#include <furi.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MESHCORE_UART_BAUD    115200u
#define MESHCORE_UART_RX_BUFSZ 512u

typedef struct MeshCoreUart MeshCoreUart;

/** Take over the USART and start receiving.
 *
 * Disables the expansion-module service for the duration, as required by
 * expansion.h before acquiring a serial handle.
 *
 * @return  handle, or NULL if the USART is already in use.
 */
MeshCoreUart* meshcore_uart_open(void);

/** Stop receiving, release the USART and restore the expansion service. */
void meshcore_uart_close(MeshCoreUart* uart);

/** Semi-blocking transmit: returns once the bytes are in the TX pipe. */
void meshcore_uart_tx(MeshCoreUart* uart, const uint8_t* data, size_t len);

/** Read whatever has arrived, waiting up to timeout_ms for the first byte.
 *
 * @return  number of bytes copied into buf (0 on timeout).
 */
size_t meshcore_uart_rx(MeshCoreUart* uart, uint8_t* buf, size_t cap, uint32_t timeout_ms);

/** Drop anything buffered — used to resync before a fresh handshake. */
void meshcore_uart_rx_flush(MeshCoreUart* uart);

/** Count of framing/noise/overrun/parity errors seen since open.
 *
 * A non-zero value with no valid frames almost always means a wiring or baud
 * mismatch, which is worth telling the user about. */
uint32_t meshcore_uart_rx_errors(const MeshCoreUart* uart);
