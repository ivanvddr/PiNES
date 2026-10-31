/*
 * Questo file fa parte del progetto PiNES.
 *
 * Copyright (C) 2026 Ivan Vettori
 *
 * Estende il pattern di FunduinoJoystick (progetto Pi64) aggiungendo i
 * pulsanti che un controller NES reale possiede e che la scheda
 * direzionale a 5 vie + 1 pulsante non copriva: B, Select, Start (oltre
 * a Fire->A già presente). Si assume — come da indicazioni — che la
 * scheda fisica disponga di tutti gli 8 ingressi necessari; qui sotto
 * trovi solo la logica software, il mapping sui pin liberi del Pico è a
 * cura di nes_joystick_config.h.
 *
 * Licenza GPLv3.
 */
#ifndef NES_JOYSTICK_H
#define NES_JOYSTICK_H
#include <Arduino.h>

#define NES_JOYSTICK_PORT_1  1
#define NES_JOYSTICK_PORT_2  2

// Bit order standard di un controller NES (usato anche dal protocollo
// shift-register $4016/$4017): A,B,Select,Start,Up,Down,Left,Right
#define NES_BTN_A      0x01
#define NES_BTN_B      0x02
#define NES_BTN_SELECT 0x04
#define NES_BTN_START  0x08
#define NES_BTN_UP     0x10
#define NES_BTN_DOWN   0x20
#define NES_BTN_LEFT   0x40
#define NES_BTN_RIGHT  0x80

class NesJoystick {
public:
    NesJoystick(uint8_t port = NES_JOYSTICK_PORT_1);

    // Inizializzazione con pin assignment (logica negativa, INPUT_PULLUP,
    // stesso schema hardware di FunduinoJoystick: LOW = premuto)
    void begin(int upPin, int downPin, int leftPin, int rightPin,
               int aPin, int bPin, int selectPin, int startPin);

    // Lettura DIRETTA senza debounce (per il protocollo shift register
    // $4016/$4017, che richiede stato sempre aggiornato).
    // Ritorna un byte con i bit NES_BTN_* impostati (1 = premuto), già
    // nell'ordine A,B,Select,Start,Up,Down,Left,Right usato da
    // PiNES.ino per popolare il shift register del joypad.
    uint8_t read();

    // Lettura con debounce (utile solo per UI/menu, non per il joypad
    // emulato: il protocollo NES vuole stato istantaneo)
    uint8_t readDebounced();

    bool isUpPressed()     { return readPin(pinUp); }
    bool isDownPressed()   { return readPin(pinDown); }
    bool isLeftPressed()   { return readPin(pinLeft); }
    bool isRightPressed()  { return readPin(pinRight); }
    bool isAPressed()      { return readPin(pinA); }
    bool isBPressed()      { return readPin(pinB); }
    bool isSelectPressed() { return readPin(pinSelect); }
    bool isStartPressed()  { return readPin(pinStart); }

    uint8_t getPort() const { return port; }
    void printStatus();
    volatile uint8_t getLastState() const { return lastState; }

private:
    int pinUp, pinDown, pinLeft, pinRight;
    int pinA, pinB, pinSelect, pinStart;
    uint8_t port;

    volatile uint8_t lastState;
    volatile uint8_t debouncedState;
    uint32_t lastChangeTime;
    static const uint32_t DEBOUNCE_MS = 20;

    bool readPin(int pin);
    uint8_t readHardware();
};
#endif
