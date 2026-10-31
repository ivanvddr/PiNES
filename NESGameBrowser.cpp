/*
 * Questo file fa parte del progetto PiNES.
 *
 * Copyright (C) 2026 Ivan Vettori — GPLv3
 * Implementazione di NESGameBrowser.
 *
 */
#include "NESGameBrowser.h"

#if NES_BROWSER_JPG
TFT_eSPI* NESGameBrowser::staticTFT = nullptr;
#endif

NESGameBrowser::NESGameBrowser(TFT_eSPI* display, NESLoader* loaderRef) {
    tft = display;
    loader = loaderRef;
#if NES_BROWSER_JPG
    staticTFT = tft;
#endif
    totalGames = 0;
    currentIndex = 0;
    currentFile[0] = '\0';
    romInfo[0] = '\0';
    lastJoyTime = 0;
    needsRedraw = true;
    logo = nullptr;
    logoWidth = 0;
    logoHeight = 0;
    hasLogo = false;
    // array ordinato nomi file
    sortedFileNames = nullptr;
}

// ============================================================
// Inizializzazione (stessa sequenza di C64GameBrowser::begin)
// ============================================================
bool NESGameBrowser::begin(FSType fsType) {
    Serial.println("[NESGameBrowser] Init...");

    if (browserPath[0] == '\0') {
        snprintf(browserPath, sizeof(browserPath), "%s", NES_GAME_DIR);
    }
    if (browserExt[0] == '\0') {
        snprintf(browserExt, sizeof(browserExt), ".nes");
    }

    // usa NESLoader per init filesystem (idempotente: se il filesystem
    // e' gia' pronto ritorna true senza toccare nulla)
    if (!loader->initFilesystem(fsType)) {
        Serial.println("[NESGameBrowser] Filesystem non disponibile!");
        showError("NO FILESYSTEM!");
        return false;
    }
    currentFS = loader->getCurrentFilesystem();
    Serial.printf("[NESGameBrowser] Filesystem: %s\n",
                  currentFS == FSType::FS_SD ? "SD" : "LittleFS");

    // verifica directory
    Serial.printf("[NESGameBrowser] Path: %s\n", browserPath);
    File dir = loader->openFile(browserPath, "r");
    if (!dir || !dir.isDirectory()) {
        Serial.printf("[NESGameBrowser] Cartella %s non trovata!\n", browserPath);
        char msg[48];
        snprintf(msg, sizeof(msg), "NO %s FOLDER!", browserPath);
        showError(msg);
        return false;
    }
    dir.close();

    // conta giochi (scan veloce, solo nomi)
    totalGames = countRomFiles();
    if (totalGames == 0) {
        Serial.println("[NESGameBrowser] Nessuna ROM trovata!");
        showError("NO GAMES FOUND!");
        return false;
    }
    Serial.printf("[NESGameBrowser] Trovate %d ROM\n", totalGames);

    // costruisco array ordinato
    if (!buildSortedFileList()) {
        Serial.println("[NESGameBrowser] Impossibile ordinare la lista!");
        showError("SORT ERROR!");
        return false;
    }

    // init decoder JPG
#if NES_BROWSER_JPG
    TJpgDec.setJpgScale(1);
    TJpgDec.setCallback(tftOutput);
#endif

    // carica primo gioco
    currentIndex = 0;
    if (!getFileAtIndex(0, currentFile)) {
        Serial.println("[NESGameBrowser] Impossibile caricare la prima ROM!");
        showError("LOAD ERROR!");
        return false;
    }
    readRomInfo();
    Serial.printf("[NESGameBrowser] Prima ROM: %s (%d/%d)\n",
                  currentFile, currentIndex + 1, totalGames);

    // Grazia iniziale: se il tasto di conferma del menu e' ancora premuto
    // quando il browser appare, il primo update() non lo processa (su Pi64
    // ci pensava il lampeggio di conferma; qui e' esplicito).
    lastJoyTime = millis();

    draw();
    return true;
}

// ============================================================
// Lista ordinata (identica a C64GameBrowser, buffer piu' larghi)
// ============================================================
bool NESGameBrowser::buildSortedFileList() {
    if (totalGames == 0 || totalGames > MAX_NES_GAMES) {
        Serial.printf("[NESGameBrowser] Troppe ROM per l'ordinamento (%d > %d)\n",
                      totalGames, MAX_NES_GAMES);
        return false;
    }
    Serial.printf("[NESGameBrowser] Costruzione array ordinato per %d ROM...\n", totalGames);

    // alloca array di puntatori
    sortedFileNames = (char**)malloc(totalGames * sizeof(char*));
    if (!sortedFileNames) {
        Serial.println("[NESGameBrowser] Malloc fallita!");
        return false;
    }
    // inizializza a NULL
    for (int i = 0; i < totalGames; i++) {
        sortedFileNames[i] = nullptr;
    }

    // scansiona e carica tutti i nomi
    File dir = openFile(browserPath, "r");
    if (!dir || !dir.isDirectory()) {
        free(sortedFileNames);
        sortedFileNames = nullptr;
        return false;
    }
    int count = 0;
    File entry;
    // popola array
    while ((entry = dir.openNextFile()) && count < totalGames) {
        if (!entry.isDirectory() && isRom(entry.name())) {
            sortedFileNames[count] = (char*)malloc(NES_FILENAME_LEN);
            if (sortedFileNames[count]) {
                strncpy(sortedFileNames[count], entry.name(), NES_FILENAME_LEN - 1);
                sortedFileNames[count][NES_FILENAME_LEN - 1] = '\0';
                count++;
            }
        }
        entry.close();
    }
    dir.close();

    if (count != totalGames) {
        Serial.printf("[NESGameBrowser] Count mismatch: attesi %d, trovati %d\n",
                      totalGames, count);
        totalGames = count;  // aggiusta
    }

    // ordina alfabeticamente (bubble sort semplice, come Pi64)
    Serial.println("[NESGameBrowser] Ordinamento...");
    for (int i = 0; i < totalGames - 1; i++) {
        for (int j = 0; j < totalGames - i - 1; j++) {
            if (strcasecmp(sortedFileNames[j], sortedFileNames[j + 1]) > 0) {
                // swap
                char* temp = sortedFileNames[j];
                sortedFileNames[j] = sortedFileNames[j + 1];
                sortedFileNames[j + 1] = temp;
            }
        }
    }

    // debug: mostra primi 5
    Serial.println("[NESGameBrowser] Array ordinato:");
    for (int i = 0; i < 5 && i < totalGames; i++) {
        Serial.printf("  [%d] %s\n", i, sortedFileNames[i]);
    }
    if (totalGames > 5) {
        Serial.println("  ...");
    }
    return true;
}

// libera array dei giochi
void NESGameBrowser::freeSortedFileList() {
    if (sortedFileNames) {
        for (int i = 0; i < totalGames; i++) {
            if (sortedFileNames[i]) {
                free(sortedFileNames[i]);
            }
        }
        free(sortedFileNames);
        sortedFileNames = nullptr;
    }
}

// CORE: accesso file per indice (usa array ordinato)
bool NESGameBrowser::getFileAtIndex(int targetIndex, char* outName) {
    if (targetIndex < 0 || targetIndex >= totalGames) {
        Serial.printf("[NESGameBrowser] Indice %d fuori range (0-%d)\n",
                      targetIndex, totalGames - 1);
        return false;
    }
    // ACCESSO DIRETTO all'array ordinato (O(1) invece di O(n))
    if (!sortedFileNames || !sortedFileNames[targetIndex]) {
        Serial.println("[NESGameBrowser] Array ordinato non disponibile!");
        return false;
    }
    strncpy(outName, sortedFileNames[targetIndex], NES_FILENAME_LEN - 1);
    outName[NES_FILENAME_LEN - 1] = '\0';
    Serial.printf("[NESGameBrowser] File[%d]: %s\n", targetIndex, outName);
    return true;
}

// ============================================================
// Navigazione bidirezionale con Wrap-Around
// ============================================================
void NESGameBrowser::nextGame() {
    if (totalGames == 0) return;
    int oldIndex = currentIndex;
    // forward con wrap
    currentIndex = (currentIndex + 1) % totalGames;
    Serial.printf("[NESGameBrowser] Navigation: %d -> %d (total: %d)\n",
                  oldIndex, currentIndex, totalGames);
    if (getFileAtIndex(currentIndex, currentFile)) {
        Serial.printf("[NESGameBrowser] Nuova ROM: %s (%d/%d)\n",
                      currentFile, currentIndex + 1, totalGames);
        // navigazione istantanea (no scan, usa array!) + info iNES
        readRomInfo();
        needsRedraw = true;
        drawGameImage();
        drawGameName();
        drawGameInfo();
        drawCounter();
        needsRedraw = false;
    } else {
        // rollback se fallisce
        Serial.println("[NESGameBrowser] Load fallita, rollback");
        currentIndex = oldIndex;
    }
}

void NESGameBrowser::prevGame() {
    if (totalGames == 0) return;
    int oldIndex = currentIndex;
    // backward con wrap (aggiunge totalGames per evitare negativi)
    currentIndex = (currentIndex - 1 + totalGames) % totalGames;
    Serial.printf("[NESGameBrowser] Navigation: %d -> %d (total: %d)\n",
                  oldIndex, currentIndex, totalGames);
    if (getFileAtIndex(currentIndex, currentFile)) {
        Serial.printf("[NESGameBrowser] Nuova ROM: %s (%d/%d)\n",
                      currentFile, currentIndex + 1, totalGames);
        // navigazione istantanea (no scan, usa array!) + info iNES
        readRomInfo();
        needsRedraw = true;
        drawGameImage();
        drawGameName();
        drawGameInfo();
        drawCounter();
        needsRedraw = false;
    } else {
        // Rollback se fallisce
        Serial.println("[NESGameBrowser] Load fallita, rollback");
        currentIndex = oldIndex;
    }
}

// update con joystick (bit ATTIVI-ALTI di NesJoystick::read())
NESBrowserEvent NESGameBrowser::update(uint8_t joyState) {
    if (totalGames == 0) return NESBrowserEvent::NONE;

    uint32_t now = millis();
    if (now - lastJoyTime < NES_BROWSER_DEBOUNCE_MS) return NESBrowserEvent::NONE;

    if (joyState & NES_BTN_LEFT) {   // LEFT
        lastJoyTime = now;
        prevGame();
        return NESBrowserEvent::NONE;
    }
    if (joyState & NES_BTN_RIGHT) {  // RIGHT
        lastJoyTime = now;
        nextGame();
        return NESBrowserEvent::NONE;
    }
    if ((joyState & NES_BTN_A) || (joyState & NES_BTN_START)) {
        lastJoyTime = now;
        return NESBrowserEvent::SELECTED;  // ROM selezionata!
    }
    if ((joyState & NES_BTN_B) || (joyState & NES_BTN_SELECT)) {
        lastJoyTime = now;
        return NESBrowserEvent::BACK;      // ritorno al menu
    }
    return NESBrowserEvent::NONE;
}

// ============================================================
// Configurazione path/estensione
// ============================================================
void NESGameBrowser::setPath(const char* cpath) {
    sprintf(browserPath, "%s%s", NES_GAME_DIR, cpath);
}

void NESGameBrowser::setFileExt(const char* fext) {
    snprintf(browserExt, sizeof(browserExt), "%s", fext);
}

// Selezione gioco
String NESGameBrowser::getRomSelected() {
    char fullPath[128];
    snprintf(fullPath, sizeof(fullPath), "%s/%s", browserPath, currentFile);
    // Salva nel NESLoader (stesso ruolo di setSelectedGame su Pi64)
    loader->setSelectedROM(fullPath);
    Serial.printf("[NESGameBrowser] File selected: %s\n", fullPath);
    return String(fullPath);
}

// ============================================================
// Utility filesystem / nomi (sempre via loader)
// ============================================================
int NESGameBrowser::countRomFiles() {
    int count = 0;
    File dir = openFile(browserPath, "r");
    if (!dir || !dir.isDirectory()) return 0;
    File entry;
    while ((entry = dir.openNextFile())) {
        if (!entry.isDirectory() && isRom(entry.name())) {
            count++;
        }
        entry.close();
    }
    dir.close();
    Serial.printf("[NESGameBrowser] Scan completato: %d file %s\n", count, browserExt);
    return count;
}

File NESGameBrowser::openFile(const char* path, const char* mode) {
    return loader->openFile(path, mode);
}

bool NESGameBrowser::fileExists(const char* path) {
    return loader->fileExists(path);
}

bool NESGameBrowser::isRom(const char* name) {
    size_t len = strlen(name);
    if (len <= 4) return false;
    return (strcasecmp(name + len - 4, browserExt) == 0);
}

String NESGameBrowser::getDisplayName(const char* filename) {
    String name = String(filename);
    int dotPos = name.lastIndexOf('.');
    if (dotPos > 0) name = name.substring(0, dotPos);
    // a size 2 (font classico TFT_eSPI) in 320px ci stanno ~26 caratteri:
    // tronca con "..." per non sconfinare dai bordi
    if (name.length() > 26) {
        name = name.substring(0, 23) + "...";
    }
    return name;
}

bool NESGameBrowser::hasImageFile(const char* romName) {
    String baseName = getDisplayName(romName);
    char jpgPath[128];
    snprintf(jpgPath, sizeof(jpgPath), "%s/%s.jpg", browserPath, baseName.c_str());
    return fileExists(jpgPath);
}

// ============================================================
// Info header iNES della ROM corrente (novita' rispetto a Pi64)
// ============================================================
void NESGameBrowser::readRomInfo() {
    romInfo[0] = '\0';
    if (currentFile[0] == '\0') return;

    char fullPath[128];
    snprintf(fullPath, sizeof(fullPath), "%s/%s", browserPath, currentFile);
    File f = openFile(fullPath, "r");
    if (!f) return;

    uint8_t header[16];
    if (f.read(header, 16) != 16) {
        f.close();
        return;
    }
    f.close();

    // magic "NES\x1A" (stesso controllo di NESLoader::loadROM)
    if (header[0] != 'N' || header[1] != 'E' || header[2] != 'S' || header[3] != 0x1A) {
        snprintf(romInfo, sizeof(romInfo), "INVALID iNES HEADER");
        return;
    }

    uint8_t prg16 = header[4];
    uint8_t chr8  = header[5];
    uint8_t mapper = ((header[6] >> 4) & 0x0F) | (((header[7] >> 4) & 0x0F) << 4);

    // NESLoader supporta i mapper 0/1/2/3/4 (NROM/MMC1/UNROM/CNROM/MMC3)
    bool supported = (mapper <= 4);

    if (!supported) {
        snprintf(romInfo, sizeof(romInfo), "MAPPER %u UNSUPPORTED - PRG %uK",
                 (unsigned)mapper, (unsigned)(prg16 * 16));
    } else if (chr8 == 0) {
        // chr8kCount==0 => CHR-RAM 8KB (vedi NESLoader::loadROM)
        snprintf(romInfo, sizeof(romInfo), "MAPPER %u - PRG %uK - CHR-RAM",
                 (unsigned)mapper, (unsigned)(prg16 * 16));
    } else {
        snprintf(romInfo, sizeof(romInfo), "MAPPER %u - PRG %uK - CHR %uK",
                 (unsigned)mapper, (unsigned)(prg16 * 16), (unsigned)(chr8 * 8));
    }
}

// ============================================================
// Disegno (livrea NES: nero, bianco, accenti rossi)
// ============================================================
void NESGameBrowser::draw() {
    if (!needsRedraw) return;
    drawBackground();
    drawLogo();
    drawGameImage();
    drawGameName();
    drawGameInfo();
    drawCounter();
    drawArrows();
    drawCredits();
    needsRedraw = false;
} 

void NESGameBrowser::drawBackground() {
    tft->fillScreen(0xDEFB);
}

void NESGameBrowser::drawLogo() {
    if (!hasLogo) {
        tft->setTextColor(TFT_RED);
        tft->setTextSize(2);
        tft->setTextDatum(TL_DATUM);
        tft->drawString("PiNES", 5, 5);
        return;
    }
    tft->setSwapBytes(true);
    tft->pushImage(5, 5, logoWidth, logoHeight, logo, 0x0000);
    tft->setSwapBytes(false);
}

void NESGameBrowser::drawGameImage() {
    const int imgWidth = 200;
    const int imgHeight = 150;
    const int imgX = (320 - imgWidth) / 2;
    const int imgY = 40;

    // Cancella area immagine
    tft->fillRect(imgX, imgY, imgWidth, imgHeight, 0xDEFB);
    if (totalGames == 0 || currentFile[0] == '\0') return;

#if NES_BROWSER_JPG
    String displayName = getDisplayName(currentFile);
    char imagePath[128];
    if (hasImageFile(currentFile)) {
        snprintf(imagePath, sizeof(imagePath), "%s/%s.jpg", browserPath, displayName.c_str());
    } else {
        snprintf(imagePath, sizeof(imagePath), "%s/noImage.jpg", browserPath);
    }
    if (!fileExists(imagePath)) {
        drawNoImagePlaceholder(imgX, imgY, imgWidth, imgHeight);
        return;
    }
    if (currentFS == FSType::FS_SD) {
        TJpgDec.drawSdJpg(imgX, imgY, imagePath);
    } else if (currentFS == FSType::FS_LITTLEFS) {
        TJpgDec.drawFsJpg(imgX, imgY, imagePath);
    }
#else
    drawNoImagePlaceholder(imgX, imgY, imgWidth, imgHeight);
#endif
}

// Placeholder quando manca il JPG dell'anteprima (o JPG disabilitati):
// cornice rossa + testo, al posto del riquadro ciano di C64GameBrowser
void NESGameBrowser::drawNoImagePlaceholder(int x, int y, int w, int h) {
    tft->drawRect(x, y, w, h, TFT_RED);
    tft->drawRect(x + 1, y + 1, w - 2, h - 2, TFT_RED);
    tft->setTextColor(TFT_DARKGREY);
    tft->setTextSize(2);
    tft->setTextDatum(MC_DATUM);
    tft->drawString("NO IMAGE", 160, y + h / 2);
}

void NESGameBrowser::drawGameName() {
    if (totalGames == 0 || currentFile[0] == '\0') return;
    String displayName = getDisplayName(currentFile);
    tft->setTextColor(TFT_BLACK);
    tft->setTextSize(2);
    tft->setTextDatum(TC_DATUM);
    tft->fillRect(0, 196, 320, 20, 0xDEFB);
    tft->drawString(displayName, 160, 198);
}

void NESGameBrowser::drawGameInfo() {
    // riga info header iNES (mapper/PRG/CHR), vedi readRomInfo()
    /*
    tft->fillRect(0, 218, 320, 12, 0xDEFB);
    if (romInfo[0] == '\0') return;
    tft->setTextColor(TFT_DARKGREY);
    tft->setTextSize(1);
    tft->setTextDatum(TC_DATUM);
    tft->drawString(romInfo, 160, 219);
    */
}

void NESGameBrowser::drawCounter() {
    // contatore "n / N" in alto a destra
    if (totalGames == 0) return;
    char buf[16];
    snprintf(buf, sizeof(buf), "%d / %d", currentIndex + 1, totalGames);
    tft->setTextSize(1);
    tft->setTextColor(TFT_DARKGREY);
    tft->setTextDatum(TR_DATUM);
    tft->fillRect(240, 5, 76, 10, 0xDEFB);
    tft->drawString(buf, 315, 6);
}

void NESGameBrowser::drawArrows() {
    if (totalGames <= 1) return;
    tft->setTextColor(TFT_RED);
    tft->setTextSize(4);
    tft->setTextDatum(MC_DATUM);
    tft->drawString("<", 20, 115);
    tft->drawString(">", 300, 115);
}

void NESGameBrowser::drawCredits() {
    tft->setTextSize(1);
    tft->setTextColor(TFT_DARKGREY);
    tft->setTextDatum(BR_DATUM);
    tft->drawString("@IV PiNES emulator", 315, 235);
}

void NESGameBrowser::showError(const char* msg) {
    tft->fillScreen(0xDEFB);
    tft->setTextColor(TFT_RED);
    tft->setTextSize(2);
    tft->setTextDatum(MC_DATUM);
    tft->drawString(msg, 160, 120);
}

void NESGameBrowser::setLogo(const unsigned short* logoData, int width, int height) {
    logo = logoData;
    logoWidth = width;
    logoHeight = height;
    hasLogo = (logo != nullptr);
}

#if NES_BROWSER_JPG
// callback TJpg_Decoder (identica a C64GameBrowser::tftOutput)
bool NESGameBrowser::tftOutput(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
    if (bitmap && staticTFT) {
        staticTFT->setSwapBytes(true);
        staticTFT->pushImage(x, y, w, h, bitmap);
        staticTFT->setSwapBytes(false);
    }
    return true;
}
#endif
