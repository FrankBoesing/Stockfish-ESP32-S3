#include <Arduino.h>

// Bring-up placeholder only. Stockfish has not yet been integrated.
// Keep this entry point minimal until the upstream build requirements are known.

void setup() {
    Serial.begin(115200);
    delay(250);
    Serial.println();
    Serial.println("Stockfish ESP32-S3: platform bring-up");
    Serial.println("Engine integration not yet started.");
}

void loop() {
    // UCI transport will be added after the engine build strategy is validated.
    delay(1000);
}
