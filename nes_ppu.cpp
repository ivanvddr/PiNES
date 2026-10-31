/*
 * nes_ppu.cpp — PPU NES ottimizzata per RP2040 — v6
 * Copyright (C) 2026 Ivan Vettori — GPLv3
 * Vedi nes_ppu.h per l'architettura (handoff senza attese verso Core1).
 *
 * Ottimizzazioni rispetto alla v5:
 *  - Niente micros() nel percorso caldo (profiling a livello di frame nel .ino).
 *  - Pattern CHR letti da puntatore (chrData[]) invece di 2 chiamate a
 *    funzione + switch per ogni tile (66 chiamate/riga prima).
 *  - fineY e switch di mirroring sollevati fuori dal loop dei 33 tile
 *    (tabella ntBase[4] per riga).
 *  - Composizione con LUT a 4 entry per tile: niente piu' 'px ? pc[px] : backdrop'.
 *  - bgPaletteIdx eliminato (veniva scritto ma mai letto); bgPixel scritto
 *    solo se gli sprite sono attivi.
 *  - kSpread in RAM (non in flash/XIP).
 *  - Frame non disegnati: nessun bg/sprite render, solo flag (vedi sprite0HitFast).
 *  - Fix: clipping dei primi 8 pixel (showLeft8) ora vale anche quando
 *    fineX > 0 (prima copriva solo il tile 0).
 *  - Fix: sprite overflow ora scatta al 9o sprite sulla riga (prima all'8o).
 *  - Fix: NMI abilitato in $2000 con vblank gia' attivo ora genera NMI.
 */
#include "nes_ppu.h"

static uint32_t kSpread[256];
static bool kSpreadInit = false;
static void initSpread() {
    if (kSpreadInit) return;
    for (int i = 0; i < 256; i++) {
        uint32_t r = 0;
        for (int b = 0; b < 8; b++) if (i & (1 << b)) r |= (1u << (b * 4));
        kSpread[i] = r;
    }
    kSpreadInit = true;
}

// ============================================================================
void NesPPU::begin(const uint8_t* chrBase, ChrWriteFn cw, CpuPageReadFn cpr, MirrorModeFn mmf) {
    chrData = chrBase;
    chrWrite = cw;
    cpuPageRead = cpr;
    mirrorModeFn = mmf;
    initSpread();
    reset();
}

void NesPPU::reset() {
    memset(nametableRAM, 0, sizeof(nametableRAM));
    memset(paletteRAM, 0, sizeof(paletteRAM));
    memset(oam, 0, sizeof(oam));
    memset(fb, 0, sizeof(fb));
    memset(bgPixel, 0, sizeof(bgPixel));
    memset(bgIndexCache, 0, sizeof(bgIndexCache));
    memset(spriteIndexCache, 0, sizeof(spriteIndexCache));

    linesDone = 0;
    frameActive = false;
    renderPixels = false;
    sprCount = 0;

    ctrl = mask = status = oamAddr = 0;
    v = t = 0; fineX = 0; writeToggle = false; nmiLate = false;
    ppuDataReadBuffer = 0;
    scanline = 0;
    oddFrame = false;
    updatePaletteCaches();
    refreshMirror();
}

void NesPPU::updatePaletteCaches() {
    for (int g = 0; g < 4; g++)
        for (int p = 0; p < 4; p++) {
            bgIndexCache[g][p]     = paletteRAM[g * 4 + p] & 0x3F;
            spriteIndexCache[g][p] = paletteRAM[0x10 + g * 4 + p] & 0x3F;
        }
}

void PINES_RAM NesPPU::updatePaletteCacheSingle(uint8_t idx) {
    idx &= 0x1F;
    uint8_t color = paletteRAM[idx] & 0x3F;
    if (idx < 0x10) bgIndexCache[idx >> 2][idx & 3] = color;
    else            spriteIndexCache[(idx - 0x10) >> 2][idx & 3] = color;
}

// ============================================================================
// Mirroring / VRAM
// ============================================================================
void PINES_RAM NesPPU::refreshMirror() {
    uint8_t mode = mirrorModeFn ? mirrorModeFn() : 0;
    switch (mode) {
        case 1:  ntBase[0] = 0x000; ntBase[1] = 0x400; ntBase[2] = 0x000; ntBase[3] = 0x400; break; // verticale
        case 2:  ntBase[0] = ntBase[1] = ntBase[2] = ntBase[3] = 0x000; break;                     // single low
        case 3:  ntBase[0] = ntBase[1] = ntBase[2] = ntBase[3] = 0x400; break;                     // single high
        default: ntBase[0] = 0x000; ntBase[1] = 0x000; ntBase[2] = 0x400; ntBase[3] = 0x400; break; // orizzontale
    }
}

uint16_t PINES_RAM NesPPU::mirrorNametable(uint16_t addr) {
    refreshMirror();
    return ntBase[(addr >> 10) & 3] + (addr & 0x03FF);
}

uint8_t PINES_RAM NesPPU::mirrorPalette(uint8_t addr) {
    addr &= 0x1F;
    if ((addr & 0x13) == 0x10) addr &= 0x0F;
    return addr;
}

uint8_t PINES_RAM NesPPU::vramRead(uint16_t addr) {
    addr &= 0x3FFF;
    if (addr < 0x2000) return chrData ? chrData[addr] : 0;
    if (addr < 0x3F00) return nametableRAM[mirrorNametable(addr)];
    return paletteRAM[mirrorPalette((uint8_t)addr)];
}

void PINES_RAM NesPPU::vramWrite(uint16_t addr, uint8_t val) {
    addr &= 0x3FFF;
    if (addr < 0x2000) {
        if (chrWrite) chrWrite(addr, val);
    } else if (addr < 0x3F00) {
        nametableRAM[mirrorNametable(addr)] = val;
    } else {
        uint8_t pi = mirrorPalette((uint8_t)addr);
        paletteRAM[pi] = val;
        updatePaletteCacheSingle(pi);
    }
}

// ============================================================================
// Registri
// ============================================================================
uint8_t PINES_RAM NesPPU::regRead(uint16_t addr) {
    switch (addr & 7) {
        case 2: { uint8_t r = status; status &= ~0x80; writeToggle = false; nmiLate = false; return r; }
        case 4: return oam[oamAddr];
        case 7: {
            uint8_t result;
            uint16_t a = v & 0x3FFF;
            if (a >= 0x3F00) { result = vramRead(a); ppuDataReadBuffer = vramRead(a - 0x1000); }
            else             { result = ppuDataReadBuffer; ppuDataReadBuffer = vramRead(a); }
            v += (ctrl & 0x04) ? 32 : 1;
            return result;
        }
        default: return 0;
    }
}

void PINES_RAM NesPPU::regWrite(uint16_t addr, uint8_t val) {
    if (addr == 0x4014) { doOamDMA(val); return; }
    switch (addr & 7) {
        case 0: {
            uint8_t old = ctrl;
            ctrl = val;
            t = (t & 0xF3FF) | ((uint16_t)(val & 0x03) << 10);
            if (!(old & 0x80) && (val & 0x80) && (status & 0x80)) nmiLate = true;
            break;
        }
        case 1: mask = val; break;
        case 3: oamAddr = val; break;
        case 4: oam[oamAddr++] = val; break;
        case 5:
            if (!writeToggle) { fineX = val & 7; t = (t & 0xFFE0) | (val >> 3); }
            else { t = (t & 0x8FFF) | ((uint16_t)(val & 7) << 12); t = (t & 0xFC1F) | ((uint16_t)(val >> 3) << 5); }
            writeToggle = !writeToggle; break;
        case 6:
            if (!writeToggle) t = (t & 0x00FF) | ((uint16_t)(val & 0x3F) << 8);
            else { t = (t & 0xFF00) | val; v = t; }
            writeToggle = !writeToggle; break;
        case 7: vramWrite(v & 0x3FFF, val); v += (ctrl & 0x04) ? 32 : 1; break;
        default: break;
    }
}

void PINES_RAM NesPPU::doOamDMA(uint8_t page) {
    uint16_t base = (uint16_t)page << 8;
    for (int i = 0; i < 256; i++) oam[(oamAddr + i) & 0xFF] = cpuPageRead ? cpuPageRead(base + i) : 0;
}

void PINES_RAM NesPPU::incrementY(uint16_t &v) {
    if ((v & 0x7000) != 0x7000) { v += 0x1000; return; }
    v &= ~0x7000;
    uint16_t y = (v & 0x03E0) >> 5;
    if (y == 29) { y = 0; v ^= 0x0800; } else if (y == 31) y = 0; else y++;
    v = (v & ~0x03E0) | (y << 5);
}

// ============================================================================
// Background: scrive l'indice colore NES (0-63) in out[256]
// ============================================================================
void PINES_RAM NesPPU::renderBackgroundLine(uint8_t* out) {
    const bool spritesOn = (mask & 0x10) != 0;
    const uint8_t backdrop = bgIndexCache[0][0];

    if (!(mask & 0x08)) {
        memset(out, backdrop, WIDTH);
        if (spritesOn) memset(bgPixel, 0, WIDTH);
        return;
    }

    const uint8_t* chr = chrData;
    const uint16_t patBase = (ctrl & 0x10) ? 0x1000 : 0x0000;
    const uint8_t  fy = (v >> 12) & 7;                // costante per tutta la riga
    const uint8_t  fx = fineX;

    uint32_t combined[33];
    uint8_t  palSel[33];

    uint16_t rv = v;
    for (int tile = 0; tile < 33; tile++) {
        uint16_t ntPage = ntBase[(rv >> 10) & 3];
        uint8_t  tileIndex = nametableRAM[ntPage + (rv & 0x03FF)];
        uint8_t  attr = nametableRAM[ntPage + 0x3C0 + ((rv >> 4) & 0x38) + ((rv >> 2) & 0x07)];
        palSel[tile] = (attr >> (((rv >> 4) & 4) | (rv & 2))) & 3;

        uint16_t pa = patBase + (uint16_t)tileIndex * 16 + fy;
        combined[tile] = kSpread[chr[pa]] | (kSpread[chr[pa + 8]] << 1);
        incrementX(rv);
    }

    // Tile 1..31: sempre interamente dentro [0,255] (8-fx .. 247-fx+7)
    for (int tile = 1; tile < 32; tile++) {
        const uint8_t* pc = bgIndexCache[palSel[tile]];
        const uint8_t lut[4] = { backdrop, pc[1], pc[2], pc[3] };
        const uint32_t c = combined[tile];
        uint8_t* o = out + tile * 8 - fx;
        o[0] = lut[(c >> 28) & 3]; o[1] = lut[(c >> 24) & 3];
        o[2] = lut[(c >> 20) & 3]; o[3] = lut[(c >> 16) & 3];
        o[4] = lut[(c >> 12) & 3]; o[5] = lut[(c >>  8) & 3];
        o[6] = lut[(c >>  4) & 3]; o[7] = lut[ c        & 3];
        if (spritesOn) {
            uint8_t* b = bgPixel + tile * 8 - fx;
            b[0] = (c >> 28) & 3; b[1] = (c >> 24) & 3; b[2] = (c >> 20) & 3; b[3] = (c >> 16) & 3;
            b[4] = (c >> 12) & 3; b[5] = (c >>  8) & 3; b[6] = (c >>  4) & 3; b[7] = c & 3;
        }
    }

    // Tile di bordo 0 e 32 (possono uscire da [0,255] per via di fineX)
    for (int e = 0; e < 2; e++) {
        const int tile = e ? 32 : 0;
        const uint8_t* pc = bgIndexCache[palSel[tile]];
        const uint32_t c = combined[tile];
        const int baseX = tile * 8 - fx;
        for (int i = 0; i < 8; i++) {
            int x = baseX + i;
            if (x < 0 || x >= WIDTH) continue;
            uint8_t px = (c >> (28 - 4 * i)) & 3;
            out[x] = px ? pc[px] : backdrop;
            if (spritesOn) bgPixel[x] = px;
        }
    }

    // Clipping dei primi 8 pixel (bit 1 di PPUMASK)
    if (!(mask & 0x02)) {
        for (int x = 0; x < 8; x++) {
            out[x] = backdrop;
            if (spritesOn) bgPixel[x] = 0;
        }
    }
}

// ============================================================================
// Sprite
// ============================================================================
// Valuta quali sprite cadono sulla riga corrente (max 8) e il flag overflow.
void PINES_RAM NesPPU::evalSprites() {
    sprCount = 0;
    if (!(mask & 0x18)) return;
    const uint32_t h = (ctrl & 0x20) ? 16 : 8;
    const int sl = (int)scanline - 1;
    for (int i = 0; i < 64; i++) {
        uint32_t row = (uint32_t)(sl - (int)oam[i * 4]);   // negativo => enorme => scartato
        if (row >= h) continue;
        if (sprCount < 8) { sprIdx[sprCount] = (uint8_t)i; sprRow[sprCount] = (uint8_t)row; sprCount++; }
        else { status |= 0x20; break; }                     // 9o sprite: overflow
    }
}

uint32_t PINES_RAM NesPPU::spritePatternRow(uint8_t tile, uint8_t attr, uint8_t row, uint8_t h) {
    if (attr & 0x80) row = h - 1 - row;                     // flip verticale
    uint16_t base; uint8_t tn;
    if (h == 16) {
        base = (tile & 1) ? 0x1000 : 0x0000;
        tn = (tile & 0xFE) + (row >= 8 ? 1 : 0);
        row &= 7;
    } else {
        base = (ctrl & 0x08) ? 0x1000 : 0x0000;
        tn = tile;
    }
    uint16_t pa = base + (uint16_t)tn * 16 + row;
    return kSpread[chrData[pa]] | (kSpread[chrData[pa + 8]] << 1);
}

void PINES_RAM NesPPU::renderSprites(uint8_t* out) {
    if (!(mask & 0x10) || sprCount == 0) return;
    const uint8_t h = (ctrl & 0x20) ? 16 : 8;
    const bool showLeft8 = (mask & 0x04) != 0;

    for (int si = sprCount - 1; si >= 0; si--) {            // dal meno al piu' prioritario
        const uint8_t i = sprIdx[si];
        const uint8_t tile = oam[i * 4 + 1];
        const uint8_t attr = oam[i * 4 + 2];
        const int sprX = oam[i * 4 + 3];
        const uint32_t comb = spritePatternRow(tile, attr, sprRow[si], h);
        const uint8_t* pal = spriteIndexCache[attr & 3];
        const bool flipH = attr & 0x40, behind = attr & 0x20;

        for (int px = 0; px < 8; px++) {
            int sx = sprX + px;
            if (sx >= WIDTH) break;
            if (!showLeft8 && sx < 8) continue;
            uint8_t p = flipH ? (uint8_t)((comb >> (px * 4)) & 3)
                              : (uint8_t)((comb >> ((7 - px) * 4)) & 3);
            if (!p) continue;
            const uint8_t bg = bgPixel[sx];
            if (i == 0 && bg && sx < 255) status |= 0x40;   // sprite-0 hit
            if (behind && bg) continue;
            out[sx] = pal[p];
        }
    }
}

// Pixel di sfondo (0-3) alla coordinata schermo sx per la riga corrente,
// calcolato da zero. Usato solo dal percorso "frame non disegnato".
uint8_t PINES_RAM NesPPU::bgPixelAt(int sx) {
    if (!(mask & 0x08)) return 0;
    if (!(mask & 0x02) && sx < 8) return 0;
    int bx = sx + fineX;
    int tile = bx >> 3, col = bx & 7;
    uint16_t cx = (v & 0x1F) + tile;
    uint16_t vv = (v & ~0x1F) | (cx & 0x1F);
    if (cx & 0x20) vv ^= 0x0400;
    uint8_t tileIndex = nametableRAM[ntBase[(vv >> 10) & 3] + (vv & 0x03FF)];
    uint16_t pa = ((ctrl & 0x10) ? 0x1000 : 0x0000) + (uint16_t)tileIndex * 16 + ((v >> 12) & 7);
    uint8_t p0 = chrData[pa], p1 = chrData[pa + 8];
    return ((p0 >> (7 - col)) & 1) | (((p1 >> (7 - col)) & 1) << 1);
}

// Solo sprite-0 hit, solo se sprite 0 e' sulla riga, solo i suoi 8 pixel.
void PINES_RAM NesPPU::sprite0HitFast() {
    if ((status & 0x40) || (mask & 0x18) != 0x18) return;
    if (sprCount == 0 || sprIdx[0] != 0) return;
    const uint8_t h = (ctrl & 0x20) ? 16 : 8;
    const uint8_t attr = oam[2];
    const int sprX = oam[3];
    const uint32_t comb = spritePatternRow(oam[1], attr, sprRow[0], h);
    const bool flipH = attr & 0x40, showLeft8 = (mask & 0x04) != 0;
    for (int px = 0; px < 8; px++) {
        int sx = sprX + px;
        if (sx >= 255) break;
        if (!showLeft8 && sx < 8) continue;
        uint8_t p = flipH ? (uint8_t)((comb >> (px * 4)) & 3)
                          : (uint8_t)((comb >> ((7 - px) * 4)) & 3);
        if (p && bgPixelAt(sx)) { status |= 0x40; return; }
    }
}

// ============================================================================
bool NesPPU::startFrame(bool wantDisplay) {
    if (wantDisplay && !frameActive) {
        linesDone = 0;
        PINES_BARRIER();
        frameActive = true;          // da qui Core1 puo' iniziare a consumare righe
        renderPixels = true;
        return true;
    }
    renderPixels = false;            // frameskip, oppure Core1 ancora occupato: NON si aspetta
    return false;
}

bool PINES_RAM NesPPU::stepScanline() {
    bool nmi = false;

    if (scanline < HEIGHT) {
        refreshMirror();
        evalSprites();
        if (renderPixels) {
            uint8_t* out = fb[scanline];
            renderBackgroundLine(out);
            renderSprites(out);
            PINES_BARRIER();                     // pixel visibili prima del contatore
            linesDone = scanline + 1;
        } else {
            sprite0HitFast();
        }
        if (mask & 0x18) {
            incrementY(v);
            v = (v & 0xFBE0) | (t & 0x041F);
        }
    } else if (scanline == HEIGHT + 1) {
        status |= 0x80;
        if (ctrl & 0x80) nmi = true;
    } else if (scanline == 261) {
        status &= ~0xE0;
        if (mask & 0x18) v = t;
        oddFrame = !oddFrame;
    }

    if (nmiLate) { nmi = true; nmiLate = false; }

    if (++scanline > 261) scanline = 0;
    return nmi;
}