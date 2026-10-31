/*
 * Questo file fa parte del progetto PiNES.
 * Copyright (C) 2026 Ivan Vettori — GPLv3.
 *
 * SOSTITUISCI questi numeri con i pin liberi del tuo
 * Pico una volta decisa la mappa completa (display SPI+DMA, audio PWM
 * pin15, SD su SPI1, ecc.). Qui ne ho messi 16 (8 per joystick)
 * validi solo per joystick1, gli altri sono fittizi, possono essere modificati.
 */
#ifndef NES_JOYSTICK_CONFIG_H
#define NES_JOYSTICK_CONFIG_H
#include "NesJoystick.h"

#define ENABLE_JOYSTICK_1  1
#define ENABLE_JOYSTICK_2  0

#define JOY1_UP_PIN      2
#define JOY1_DOWN_PIN    3
#define JOY1_LEFT_PIN    4
#define JOY1_RIGHT_PIN   5
#define JOY1_A_PIN       6
#define JOY1_B_PIN       7
#define JOY1_SELECT_PIN  0
#define JOY1_START_PIN   1

// TODO: rimappa su pin liberi reali
#define JOY2_UP_PIN      7
#define JOY2_DOWN_PIN    8
#define JOY2_LEFT_PIN    9
#define JOY2_RIGHT_PIN   1
#define JOY2_A_PIN       6
#define JOY2_B_PIN       0
#define JOY2_SELECT_PIN  0
#define JOY2_START_PIN   0

inline void initNesJoysticks(NesJoystick** joy1, NesJoystick** joy2) {
#if ENABLE_JOYSTICK_1
    *joy1 = new NesJoystick(NES_JOYSTICK_PORT_1);
    (*joy1)->begin(JOY1_UP_PIN, JOY1_DOWN_PIN, JOY1_LEFT_PIN, JOY1_RIGHT_PIN,
                   JOY1_A_PIN, JOY1_B_PIN, JOY1_SELECT_PIN, JOY1_START_PIN);
#else
    *joy1 = nullptr;
#endif
#if ENABLE_JOYSTICK_2
    *joy2 = new NesJoystick(NES_JOYSTICK_PORT_2);
    (*joy2)->begin(JOY2_UP_PIN, JOY2_DOWN_PIN, JOY2_LEFT_PIN, JOY2_RIGHT_PIN,
                   JOY2_A_PIN, JOY2_B_PIN, JOY2_SELECT_PIN, JOY2_START_PIN);
#else
    *joy2 = nullptr;
#endif
}
#endif
