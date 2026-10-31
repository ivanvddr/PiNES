/*
 * nes_ppu.h — PPU NES per RP2040 (PiNES) — v6
 * Copyright (C) 2026 Ivan Vettori — GPLv3
 *
 * v6: handoff Core0 -> Core1 SENZA ATTESE.
 *  - Un solo frame buffer a 8 bit (indice palette NES 0-63): 256x240 = 60KB.
 *  - Core0 scrive le righe in fb[] e pubblica il progresso con linesDone.
 *    Core1 consuma le righe gia' pronte (conversione RGB565 + DMA) mentre
 *    Core0 sta ancora emulando il resto del frame.
 *  - Core0 NON aspetta mai Core1: a inizio frame, se Core1 e' ancora occupato
 *    col frame precedente (frameActive == true) il nuovo frame viene
 *    semplicemente NON disegnato (startFrame() ritorna false). Si sacrifica
 *    il framerate, mai la velocita' d'emulazione.
 *  - Frame non disegnati (frameskip o Core1 occupato): costo PPU ~ zero. Si
 *    calcolano solo i flag che la CPU puo' leggere (sprite-0 hit, overflow)
 *    con un percorso minimale che valuta SOLO i pixel necessari.
 */
#ifndef NES_PPU_H
#define NES_PPU_H
#include <stdint.h>
#include <string.h>
#include "pines_ram.h"

class NesPPU {
public:
    typedef void    (*ChrWriteFn)(uint16_t addr, uint8_t val);
    typedef uint8_t (*CpuPageReadFn)(uint16_t addr);
    typedef uint8_t (*MirrorModeFn)();

    static const int WIDTH  = 256;
    static const int HEIGHT = 240;

    // chrBase: puntatore alla finestra CHR 8KB del loader (ROM o RAM, mappata
    // 1:1 su $0000-$1FFF e a indirizzo costante). La PPU legge i pattern
    // direttamente da qui, senza chiamate a funzione.
    void begin(const uint8_t* chrBase, ChrWriteFn chrWrite,
               CpuPageReadFn cpuPageRead, MirrorModeFn mirrorModeFn);
    void reset();

    uint8_t regRead(uint16_t addr);
    void    regWrite(uint16_t addr, uint8_t val);

    // Una scanline. Ritorna true se va inviato NMI ora.
    bool stepScanline();

    // Da chiamare a scanline==0, prima di stepScanline(). Ritorna true se il
    // frame verra' disegnato (richiesto E Core1 libero).
    bool startFrame(bool wantDisplay);

    // --- Handoff verso Core1 ---------------------------------------------
    uint8_t fb[HEIGHT][WIDTH] __attribute__((aligned(4)));
    volatile uint16_t linesDone   = 0;      // righe complete e leggibili (scritto da Core0)
    volatile bool     frameActive = false;  // true da startFrame() fino a fine push (azzerato da Core1)

    uint16_t getCurrentScanline() const { return scanline; }
    bool     frameBeingRendered() const { return renderPixels; }

private:
    const uint8_t* chrData = nullptr;
    ChrWriteFn     chrWrite = nullptr;
    CpuPageReadFn  cpuPageRead = nullptr;
    MirrorModeFn   mirrorModeFn = nullptr;

    uint8_t nametableRAM[0x800];
    uint8_t paletteRAM[32];
    uint8_t oam[256];

    uint8_t bgIndexCache[4][4];      // indici colore NES (0-63) gia' risolti
    uint8_t spriteIndexCache[4][4];

    bool renderPixels = false;

    uint8_t  ctrl = 0, mask = 0, status = 0, oamAddr = 0;
    uint16_t v = 0, t = 0;
    uint8_t  fineX = 0;
    bool     writeToggle = false;
    bool     nmiLate = false;        // NMI abilitato in $2000 mentre il vblank e' gia' attivo
    uint8_t  ppuDataReadBuffer = 0;

    uint16_t scanline = 0;
    bool     oddFrame = false;

    uint16_t ntBase[4];              // offset nametable (0 / 0x400) per quadrante, aggiornato a ogni riga
    uint8_t  bgPixel[WIDTH];         // valore 0-3 del pixel di sfondo (solo se sprite attivi)

    // Valutazione sprite della riga corrente
    uint8_t sprIdx[8];
    uint8_t sprRow[8];
    uint8_t sprCount = 0;

    void updatePaletteCaches();
    void updatePaletteCacheSingle(uint8_t idx);

    uint8_t  vramRead(uint16_t addr);
    void     vramWrite(uint16_t addr, uint8_t val);
    uint16_t mirrorNametable(uint16_t addr);
    uint8_t  mirrorPalette(uint8_t addr);
    void     refreshMirror();

    void evalSprites();
    uint32_t spritePatternRow(uint8_t tile, uint8_t attr, uint8_t row, uint8_t h);
    uint8_t  bgPixelAt(int sx);
    void renderBackgroundLine(uint8_t* out);
    void renderSprites(uint8_t* out);
    void sprite0HitFast();
    void doOamDMA(uint8_t page);

    static inline void incrementX(uint16_t &v) {
        if ((v & 0x001F) == 31) { v &= ~0x001F; v ^= 0x0400; } else v++;
    }
    static void incrementY(uint16_t &v);
};

#endif