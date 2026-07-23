/*
 * Transparent USB <-> UART bridge for the Flipper WiFi Devboard (ESP32-S2).
 *
 * A dumb pipe, deliberately. Bytes arriving on USB go out of Serial1 and back,
 * unchanged and unbuffered beyond what the hardware needs. All the protocol
 * lives on the laptop in meshcore_emulator.py; this board must not know that
 * MeshCore exists, because anything it "helps" with is something that can
 * disagree with a real node.
 *
 * Wiring, node side of the bridge:
 *
 *     Devboard U_TX (GPIO17) -> Flipper pin 14 (RX)
 *     Devboard U_RX (GPIO18) <- Flipper pin 13 (TX)
 *     GND                    <-> Flipper pin 11 or 18
 *
 * The GPIO numbers come from the devboard's own firmware, which names them
 * CLI_TXD and CLI_RXD (blackmagic-esp32-s2, main/cli/cli-commands-gpio.c) --
 * not from guesswork.
 *
 * 3.3 V logic on both sides, so no level shifting.
 */

#include <Arduino.h>

#ifndef BRIDGE_TX_PIN
#define BRIDGE_TX_PIN 17
#endif
#ifndef BRIDGE_RX_PIN
#define BRIDGE_RX_PIN 18
#endif
#ifndef BRIDGE_BAUD
#define BRIDGE_BAUD 115200
#endif

/* One MeshCore frame is at most 176 bytes; a chunk this size moves a whole
 * frame per iteration without ever holding one back. */
static const size_t kChunk = 256;

void setup() {
    Serial.begin(BRIDGE_BAUD);
    Serial1.begin(BRIDGE_BAUD, SERIAL_8N1, BRIDGE_RX_PIN, BRIDGE_TX_PIN);

    /* No banner on Serial: this is a byte-exact pipe, and a greeting would land
     * in the middle of someone's protocol. */
}

void loop() {
    static uint8_t buffer[kChunk];

    int available = Serial.available();
    if(available > 0) {
        size_t n = Serial.readBytes(buffer, available > (int)kChunk ? kChunk : available);
        Serial1.write(buffer, n);
    }

    available = Serial1.available();
    if(available > 0) {
        size_t n = Serial1.readBytes(buffer, available > (int)kChunk ? kChunk : available);
        Serial.write(buffer, n);
    }
}
