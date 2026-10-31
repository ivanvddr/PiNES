/*
 * Questo file fa parte del progetto PiNES.
 *
 * Copyright (C) 2026 Ivan Vettori Licenza GPLv3
 *
 * Porting del pattern C64Menu (progetto Pi64) ridotto alla sola voce
 * "giochi": il menu PiNES serve unicamente da lanciatore del browser
 * delle ROM .nes. La struttura (begin/draw/update, enum voce/azione,
 * debounce a tempo, ridisegno selettivo della sola voce cambiata) e'
 * identica all'originale; cambiano input e livrea.
 *
 * DIFFERENZE RISPETTO A C64Menu:
 *  - Joystick: NesJoystick invece di FunduinoJoystick. NES_BTN_* sono
 *    ATTIVI-ALTI (bit a 1 = premuto), mentre FunduinoJoystick restituiva
 *    stati attivi-bassi (bit a 0 = premuto): tutti i check sono quindi
 *    (state & BIT) invece di !(state & BIT).
 *  - Conferma: A oppure START (l'equivalente NES del FIRE della shield
 *    Funduino a 5 tasti).
 *  - Voce unica GAMES BROWSER: UP/DOWN restano implementati (con
 *    wrap-around) per non dover toccare nulla se in futuro aggiungeró voci.
 *
 */
#ifndef NES_MENU_H
#define NES_MENU_H

#include <Arduino.h>
#include <TFT_eSPI.h>
#include "NesJoystick.h"

// ===== VOCI MENU =====
enum NESMenuItem {
    NES_MENU_GAMES = 0,
    NES_MENU_ITEMS_COUNT = 1
};

// ===== AZIONI MENU =====
enum NESMenuAction {
    NES_ACTION_NONE = 0,
    NES_ACTION_START_GAME = 1
};

class NESMenu {
public:
    NESMenu(TFT_eSPI* display, NesJoystick* joystick);

    // Inizializza menu (disegna tutto)
    void begin();

    // Aggiorna menu (chiamare in loop).
    // Ritorna NES_ACTION_NONE finché non viene confermata la voce.
    NESMenuAction update();

    // Disegna menu completo
    void draw();

    // Imposta logo (opzionale, chiamare prima di begin())
    void setLogo(const unsigned short* logoData, int width, int height);

private:
    TFT_eSPI* tft;
    NesJoystick* joy;

    // Logo
    const unsigned short* logo;
    int logoWidth;
    int logoHeight;
    bool hasLogo;

    // Stato menu
    NESMenuItem selectedItem;
    NESMenuItem lastSelectedItem;

    // Debouncing joystick
    uint32_t lastJoyTime;
    static const uint32_t JOY_DEBOUNCE_MS = 200;

    // Disegno
    void drawBackground();
    void drawLogo();
    void drawHeaderRule();      // filetto rosso sotto il logo (livrea NES)
    void drawMenuItems();
    void drawHint();            // suggerimento comandi sotto la voce
    void drawCredits();
    void drawMenuItem(NESMenuItem item, bool selected);

    // Calcolo posizioni
    int getMenuItemY(NESMenuItem item);

    // Testi menu
    const char* getMenuItemText(NESMenuItem item);
};
#endif
