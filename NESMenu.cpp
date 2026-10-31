/*
 * Questo file fa parte del progetto PiNES.
 *
 * Copyright (C) 2026 Ivan Vettori — GPLv3
 * Implementazione di NESMenu (vedi NESMenu.h per le note di porting
 * rispetto a C64Menu / Pi64).
 */
#include "NESMenu.h"

NESMenu::NESMenu(TFT_eSPI* display, NesJoystick* joystick) {
    tft = display;
    joy = joystick;
    selectedItem = NES_MENU_GAMES;
    lastSelectedItem = NES_MENU_GAMES;
    lastJoyTime = 0;
    logo = nullptr;
    logoWidth = 0;
    logoHeight = 0;
    hasLogo = false;
}

void NESMenu::setLogo(const unsigned short* logoData, int width, int height) {
    logo = logoData;
    logoWidth = width;
    logoHeight = height;
    hasLogo = (logo != nullptr);
}

void NESMenu::begin() {
    Serial.println("NESMenu: Init...");
    draw();
}

void NESMenu::draw() {
    drawBackground();
    drawLogo();
    drawHeaderRule();
    drawMenuItems();
    drawHint();
    drawCredits();
}

void NESMenu::drawBackground() {
    tft->fillScreen(0xDEFB);
}

void NESMenu::drawLogo() {
    if (!hasLogo) {
        // Se non c'e' logo, mostra il titolo testuale (fallback come C64Menu)
        tft->setTextColor(TFT_RED);
        tft->setTextSize(3);
        tft->setTextDatum(TC_DATUM); // Top Center
        tft->drawString("PiNES", 160, 20);
        return;
    }

    // centra logo orizzontalmente
    int logoX = (320 - logoWidth) / 2;
    int logoY = 20;

    // abilita swap bytes per RGB565 (e ripristina, come fa C64Menu: il
    // resto di PiNES lavora con setSwapBytes(false) per la palette)
    tft->setSwapBytes(true);
    tft->pushImage(logoX, logoY, logoWidth, logoHeight, logo, 0x0000);
    tft->setSwapBytes(false);
}

void NESMenu::drawHeaderRule() {
    // filetto rosso sotto l'area logo: separa header e voce menu
    tft->fillRect(90, 100, 140, 2, TFT_RED);
}

void NESMenu::drawCredits() {
    tft->setTextSize(1);
    tft->setTextColor(TFT_DARKGREY);
    tft->setTextDatum(BR_DATUM); // basso a destra (BR)
    tft->drawString("@IV PiNES emulator", 315, 235);
}

void NESMenu::drawMenuItems() {
    for (int i = 0; i < NES_MENU_ITEMS_COUNT; i++) {
        drawMenuItem((NESMenuItem)i, i == selectedItem);
    }
}

void NESMenu::drawMenuItem(NESMenuItem item, bool selected) {
    int y = getMenuItemY(item);
    const char* text = getMenuItemText(item);

    // Cancella area voce (per aggiornamento)
    tft->fillRect(0, y - 2, 320, 26, 0xDEFB);

    // Dimensione testo
    tft->setTextSize(2);
    tft->setTextDatum(TC_DATUM); // in alto centro (TC)

    if (selected) {
        // voce selezionata: testo bianco e cursore '>' rosso
        tft->setTextColor(TFT_BLACK);
        // calcola posizione del '>' (approssimazione larghezza: 12px/char a size 2)
        int textWidth = strlen(text) * 12;
        int cursorX = (320 - textWidth) / 2 - 20; // 20px prima del testo
        tft->setTextColor(TFT_RED);
        tft->drawString(">", cursorX, y);
        tft->setTextColor(TFT_BLACK);
        tft->drawString(text, 160, y);
    } else {
        // voce non selezionata
        tft->setTextColor(TFT_LIGHTGREY);
        tft->drawString(text, 160, y);
    }
}

void NESMenu::drawHint() {
    // suggerimento comandi sotto la voce menu
    tft->setTextSize(1);
    tft->setTextColor(TFT_DARKGREY);
    tft->setTextDatum(TC_DATUM);
    tft->drawString("A / START = OK", 160, 150);
}

int NESMenu::getMenuItemY(NESMenuItem item) {
    // Logo termina circa a Y=90 (20 + altezza logo) + filetto a Y=100
    // Inizia menu da Y=110 (stessa geometria di C64Menu)
    int startY = 110;
    int spacing = 35;
    return startY + (item * spacing);
}

const char* NESMenu::getMenuItemText(NESMenuItem item) {
    switch (item) {
        case NES_MENU_GAMES:
            return "GAMES BROWSER";
        default:
            return "???";
    }
}

NESMenuAction NESMenu::update() {
    // Leggi joystick (NesJoystick::read() = bit ATTIVI-ALTI, 1 = premuto)
    uint8_t joyState = joy->read();

    // debouncing: ignora input troppo veloci
    uint32_t now = millis();
    if (now - lastJoyTime < JOY_DEBOUNCE_MS) {
        return NES_ACTION_NONE;
    }

    // UP: selezione precedente
    if (joyState & NES_BTN_UP) {
        lastJoyTime = now;
        if (selectedItem > 0) {
            selectedItem = (NESMenuItem)(selectedItem - 1);
        } else {
            selectedItem = (NESMenuItem)(NES_MENU_ITEMS_COUNT - 1); // Wrap around
        }
        // ridisegna solo se cambiato
        if (selectedItem != lastSelectedItem) {
            drawMenuItem(lastSelectedItem, false);
            drawMenuItem(selectedItem, true);
            lastSelectedItem = selectedItem;
        }
        return NES_ACTION_NONE;
    }

    // DOWN: selezione successiva
    if (joyState & NES_BTN_DOWN) {
        lastJoyTime = now;
        if (selectedItem < NES_MENU_ITEMS_COUNT - 1) {
            selectedItem = (NESMenuItem)(selectedItem + 1);
        } else {
            selectedItem = (NESMenuItem)0; // Wrap around
        }
        // ridisegna solo se cambiato
        if (selectedItem != lastSelectedItem) {
            drawMenuItem(lastSelectedItem, false);
            drawMenuItem(selectedItem, true);
            lastSelectedItem = selectedItem;
        }
        return NES_ACTION_NONE;
    }

    // A oppure START: conferma selezione (equivalente del FIRE su Pi64)
    if ((joyState & NES_BTN_A) || (joyState & NES_BTN_START)) {
        lastJoyTime = now;

        // Feedback visivo: lampeggio (identico a C64Menu)
        drawMenuItem(selectedItem, false);
        delay(100);
        drawMenuItem(selectedItem, true);
        delay(100);

        // Ritorna azione corrispondente
        switch (selectedItem) {
            case NES_MENU_GAMES:
                return NES_ACTION_START_GAME;
            default:
                return NES_ACTION_NONE;
        }
    }

    return NES_ACTION_NONE;
}
