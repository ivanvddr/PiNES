/*
 * Questo file fa parte del progetto PiNES.
 * Copyright (C) 2026 Ivan Vettori — GPLv3.
 * Vedi NesJoystick.h per le note di design (estensione di FunduinoJoystick).
 */
#include "NesJoystick.h"

NesJoystick::NesJoystick(uint8_t joystickPort) {
    port = joystickPort;
    pinUp = pinDown = pinLeft = pinRight = -1;
    pinA = pinB = pinSelect = pinStart = -1;
    lastState = 0;
    debouncedState = 0;
    lastChangeTime = 0;
}

void NesJoystick::begin(int upPin, int downPin, int leftPin, int rightPin,
                         int aPin, int bPin, int selectPin, int startPin) {
    pinUp = upPin; pinDown = downPin; pinLeft = leftPin; pinRight = rightPin;
    pinA = aPin; pinB = bPin; pinSelect = selectPin; pinStart = startPin;

    pinMode(pinUp, INPUT_PULLUP);
    pinMode(pinDown, INPUT_PULLUP);
    pinMode(pinLeft, INPUT_PULLUP);
    pinMode(pinRight, INPUT_PULLUP);
    pinMode(pinA, INPUT_PULLUP);
    pinMode(pinB, INPUT_PULLUP);
    pinMode(pinSelect, INPUT_PULLUP);
    pinMode(pinStart, INPUT_PULLUP);

    Serial.printf("NesJoystick: Port %d initialized\n", port);
    Serial.printf("  UP:%d DOWN:%d LEFT:%d RIGHT:%d A:%d B:%d SELECT:%d START:%d\n",
                  pinUp, pinDown, pinLeft, pinRight, pinA, pinB, pinSelect, pinStart);

    lastState = readHardware();
    debouncedState = lastState;
}

bool NesJoystick::readPin(int pin) {
    if (pin < 0) return false;
    return digitalRead(pin) == LOW;   // logica negativa: LOW = premuto
}

uint8_t NesJoystick::readHardware() {
    uint8_t s = 0;
    if (readPin(pinA))      s |= NES_BTN_A;
    if (readPin(pinB))      s |= NES_BTN_B;
    if (readPin(pinSelect)) s |= NES_BTN_SELECT;
    if (readPin(pinStart))  s |= NES_BTN_START;
    if (readPin(pinUp))     s |= NES_BTN_UP;
    if (readPin(pinDown))   s |= NES_BTN_DOWN;
    if (readPin(pinLeft))   s |= NES_BTN_LEFT;
    if (readPin(pinRight))  s |= NES_BTN_RIGHT;
    return s;
}

uint8_t NesJoystick::read() {
    uint8_t s = readHardware();
    lastState = s;
    return s;
}

uint8_t NesJoystick::readDebounced() {
    uint8_t current = readHardware();
    if (current != debouncedState) {
        if (millis() - lastChangeTime > DEBOUNCE_MS) {
            debouncedState = current;
            lastChangeTime = millis();
        }
    }
    return debouncedState;
}

void NesJoystick::printStatus() {
    uint8_t s = read();
    Serial.printf("Joy%d: $%02X (", port, s);
    if (s & NES_BTN_A)      Serial.print("A ");
    if (s & NES_BTN_B)      Serial.print("B ");
    if (s & NES_BTN_SELECT) Serial.print("SELECT ");
    if (s & NES_BTN_START)  Serial.print("START ");
    if (s & NES_BTN_UP)     Serial.print("UP ");
    if (s & NES_BTN_DOWN)   Serial.print("DOWN ");
    if (s & NES_BTN_LEFT)   Serial.print("LEFT ");
    if (s & NES_BTN_RIGHT)  Serial.print("RIGHT ");
    Serial.println(")");
}
