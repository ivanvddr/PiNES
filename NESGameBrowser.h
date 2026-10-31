/*
 * Questo file fa parte del progetto PiNES.
 *
 * Copyright (C) 2026 Ivan Vettori
 *
 * Porting del pattern C64GameBrowser (progetto Pi64), specializzato
 * sulla sola navigazione delle ROM .nes (giochi). Costruito sopra
 * NESLoader, che espone la stessa interfaccia del Loader di Pi64
 * (initFilesystem / openFile / fileExists / setSelectedROM /
 * getCurrentFilesystem): il browser non tocca mai SD o LittleFS
 * direttamente, passa sempre dal loader.
 *
 * DIFFERENZE RISPETTO A C64GameBrowser:
 *  1. update() ritorna NESBrowserEvent invece di bool: oltre a
 *     SELECTED (A/START, l'equivalente del FIRE su Pi64) esiste BACK
 *     (B/SELECT) per tornare al menu principale senza riavviare.
 *  2. Joystick: NesJoystick e' ATTIVO-ALTO (bit a 1 = premuto),
 *     FunduinoJoystick era attivo-basso: check (state & BIT).
 *  3. setPath() usa un path ASSOLUTO (su Pi64 veniva concatenato a
 *     GAME_DIR): di default qui vale NES_GAME_DIR "/roms", che e'
 *     la cartella gia' usata da PiNES.ino.
 *  4. Nome file fino a 63 caratteri (i nomi No-Intro/GoodNES delle
 *     ROM NES sono lunghi) e MAX_NES_GAMES=300: l'array ordinato sta
 *     comunque in ~21KB di heap, liberato al delete del browser
 *     PRIMA che loadROM() malloc-hi la cache intera della ROM.
 *  5. Riga info iNES: durante la navigazione vengono letti 16 byte
 *     di header della ROM evidenziata e mostrati mapper/PRG/CHR
 *     ("MAPPER 1 - PRG 128K - CHR 64K"); i mapper oltre MMC3 (4)
 *     vengono segnalati come UNSUPPORTED (NESLoader li rifiuta).
 *  6. Contatore "n / N" in alto a destra e nome troncato a 26 char
 *     con "..." (a size 2 ci stanno ~26 caratteri in 320px).
 *  7. Anteprime JPG identiche (TJpg_Decoder, 200x150, fallback
 *     noImage.jpg nella stessa cartella delle ROM).
 *
 * Il JPEG delle anteprime deve chiamarsi come la ROM ma con estensione
 * .jpg (es. "Super Mario Bros.nes" -> "Super Mario Bros.jpg").
 * Richiede la libreria TJpg_Decoder (Bodmer), gia' usata da Pi64:
 * imposta NES_BROWSER_JPG a 0 per compilarne una versione senza.
 *
 * Licenza GPLv3.
 */
#ifndef NES_GAME_BROWSER_H
#define NES_GAME_BROWSER_H

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <SD.h>
#include <LittleFS.h>
#include "NESLoader.h"
#include "NesJoystick.h"   // bit NES_BTN_* per update()

// Anteprime JPG via TJpg_Decoder (libreria di Bodmer, come su Pi64)
#ifndef NES_BROWSER_JPG
  #define NES_BROWSER_JPG 1
#endif

#if NES_BROWSER_JPG
  #include <TJpg_Decoder.h>
#endif

// Cartella di default delle ROM (stesso ruolo di GAME_DIR "/Pi64")
#define NES_GAME_DIR             "/PiNES"

// Debounce navigazione (identico a C64GameBrowser)
#define NES_BROWSER_DEBOUNCE_MS  200

// Limite array ordinato (su Pi64: MAX_SORTED_GAMES 200; qui 300 per
// libreria NES tipicamente piu' corposa. ~21KB di heap massimi)
#define MAX_NES_GAMES            300

// Lunghezza massima nome file conservata (su Pi64: 32 char)
#define NES_FILENAME_LEN         64

// ===== EVENTI BROWSER =====
enum class NESBrowserEvent : uint8_t {
    NONE      = 0,
    SELECTED  = 1,   // A o START: ROM confermata
    BACK      = 2    // B o SELECT: torna al menu principale
};

class NESGameBrowser {
public:
    NESGameBrowser(TFT_eSPI* display, NESLoader* loaderRef);
    ~NESGameBrowser() { freeSortedFileList(); }

    // Inizializza browser: filesystem (via NESLoader), scansione cartella,
    // lista ordinata, prima ROM e disegno. 
    bool begin(FSType fsType = FSType::FS_SD);

    // Logo piccolo (angolo alto a sinistra), vedi PiNESLogo.h
    void setLogo(const unsigned short* logoData, int width, int height);

    // Ridisegno completo
    void draw();

    // Navigazione con wrap-around (accesso O(1) all'array ordinato)
    void nextGame();
    void prevGame();

    // Aggiorna stato input. joyState e' il byte ATTIVO-ALTO di
    // NesJoystick::read(). LEFT/RIGHT navigano, A/START conferma,
    // B/SELECT torna al menu.
    NESBrowserEvent update(uint8_t joyState);

    // Path completo della ROM evidenziata: lo salva anche nel loader
    // (setSelectedROM) e lo ritorna, come getPrgSelected() su Pi64.
    String getRomSelected();

    // Path ASSOLUTO della cartella ROM (default NES_GAME_DIR)
    void setPath(const char* cpath);

    // Estensione filtrata (default ".nes")
    void setFileExt(const char* fext);

    // Info di debug/log
    int getTotalGames() const { return totalGames; }
    int getCurrentIndex() const { return currentIndex; }

private:
    TFT_eSPI* tft;
    NESLoader* loader;

    FSType currentFS;
    char browserPath[NES_FILENAME_LEN] = {0};
    char browserExt[8] = {0};

    const unsigned short* logo;
    int logoWidth;
    int logoHeight;
    bool hasLogo;

    // array ordinato alfabeticamente
    int totalGames;
    int currentIndex;
    char currentFile[NES_FILENAME_LEN];
    char romInfo[56];          // riga info header iNES (mapper/PRG/CHR)
    uint32_t lastJoyTime;
    bool needsRedraw;
    char** sortedFileNames;   // array di nomi ordinati

    // filesystem (sempre tramite loader, come C64GameBrowser)
    File openFile(const char* path, const char* mode = "r");
    bool fileExists(const char* path);

    // sorting e accesso ordinato
    int  countRomFiles();
    bool buildSortedFileList();
    void freeSortedFileList();
    bool getFileAtIndex(int index, char* outName);

    // utility
    bool isRom(const char* name);
    String getDisplayName(const char* filename);
    bool hasImageFile(const char* romName);
    void readRomInfo();        // legge l'header iNES della ROM corrente

    // disegno
    void drawBackground();
    void drawLogo();
    void drawGameImage();
    void drawNoImagePlaceholder(int x, int y, int w, int h);
    void drawGameName();
    void drawGameInfo();
    void drawCounter();
    void drawArrows();
    void drawCredits();
    void showError(const char* msg);

#if NES_BROWSER_JPG
    // callback TJpg_Decoder (identica a C64GameBrowser)
    static bool tftOutput(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap);
    static TFT_eSPI* staticTFT;
#endif
};
#endif
