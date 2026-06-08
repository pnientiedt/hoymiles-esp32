#include <Arduino.h>
#include "config.h"

void setup() {
    Serial.begin(115200);
    Serial.println("Hoymiles ESP32 bridge starting...");
}

void loop() {
    delay(1000);
}
