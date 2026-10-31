#pragma once
/*
 * NESLoader.h — Loader ROM .nes per PiNES (RP2040)
 *
 * Copyright (C) 2026 Ivan Vettori — GPLv3
 * Basato sul pattern di SMSLoader (progetto SMS Emulator / PiMSX).
 *
 * ============================================================
 * Differenza di fondo rispetto a SMSLoader
 * ============================================================
 * La SMSLoader cache-a 3 slot fissi da 16KB (= tutto lo spazio CPU
 * indirizzabile per la ROM). Sul NES la "finestra" indirizzabile per
 * la ROM è più grande (32KB CPU + 8KB CHR per la PPU) e il numero/
 * dimensione dei banchi cambia parecchio da mapper a mapper (16KB su
 * MMC1/UNROM, 8KB su MMC3, nessuno su NROM...).
 *
 * Per non scrivere una cache diversa per ogni mapper, uso UNA SOLA
 * finestra attiva fissa, alla granularità più fine richiesta da
 * QUALSIASI mapper supportato:
 *
 *   PRG: 4 slot × 8KB  = 32KB   ($8000-$FFFF, granularità 8KB)
 *   CHR: 8 slot × 1KB  = 8KB    ($0000-$1FFF lato PPU, granularità 1KB)
 *
 * Ogni mapper, quando riceve una scrittura nei suoi registri, si
 * limita a dire "lo slot N ora deve contenere il banco fisico X" —
 * la ricarica effettiva (syncPrgSlots/syncChrSlots) ricarica SOLO
 * gli slot il cui banco è cambiato, esattamente come loadBank() in
 * SMSLoader evita reload ridondanti.
 *
 * ============================================================
 * Strategia di storage (stessa identica idea di SMSLoader)
 * ============================================================
 *   ROM piccola (PRG+CHR ≤ NES_FULLCACHE_THRESHOLD, vedi sotto):
 *     tutta la ROM viene letta UNA VOLTA in un buffer malloc'ato
 *     (_romFullCache) e da quel momento "ricaricare un banco" è un
 *     semplice memcpy, zero accessi SD/flash durante l'emulazione.
 *   ROM grande:
 *     resta su SD/LittleFS, i banchi vengono letti al volo a ogni
 *     bank-switch (readFromFile, stesso schema di SMSLoader).
 *
 * NOTA IMPORTANTE sul mapper MMC3: alcuni giochi cambiano banco CHR
 * più volte all'interno di un singolo frame (es. per lo split status
 * bar / parallasse). Se la ROM è in modalità "swap da SD", questi
 * cambi continui possono introdurre latenza/jitter percepibile (una
 * letture da SD da 1KB può costare 1-5ms, contro i ~64µs disponibili
 * per scanline). Su LittleFS (flash interna) il problema è molto
 * minore. Consiglio: per giochi MMC3 pesanti in bank-switch, preferire
 * LittleFS o assicurarsi che la ROM rientri nella soglia di
 * full-cache. Verificalo sul tuo hardware reale con i giochi che ti
 * interessano.
 *
 * ============================================================
 * Uso da PiNES.ino
 * ============================================================
 *   uint8_t nesMemRead(uint16_t addr)  { return loader.cpuRead(addr); }
 *   void    nesMemWrite(uint16_t a, uint8_t v) { loader.cpuWrite(a, v); }
 *   uint8_t nesChrRead(uint16_t addr)  { return loader.chrRead(addr); }
 *   void    nesChrWrite(uint16_t a, uint8_t v) { loader.chrWrite(a, v); }
 *   // una volta per scanline visibile, DOPO ppu.stepScanline():
 *   if (loader.notifyScanlineIRQ()) cpu.requestIRQ(true);
 *   else if (!loader.irqStillPending()) cpu.requestIRQ(false);
 */

#include <Arduino.h>
#include <SD.h>
#include <LittleFS.h>
#include <stdint.h>
#include <string.h>

enum class FSType : uint8_t { FS_NONE, FS_SD, FS_LITTLEFS };

#ifndef SD_CS
  #define SD_CS    13
  #define SD_MISO  12
  #define SD_MOSI  11
  #define SD_SCLK  10
#endif

enum class NESMapperID : uint8_t {
    NROM  = 0,
    MMC1  = 1,
    UNROM = 2,
    CNROM = 3,
    MMC3  = 4,
    UNSUPPORTED = 0xFF
};

enum class NESMirrorMode : uint8_t {
    HORIZONTAL,
    VERTICAL,
    SINGLE_SCREEN_LOW,
    SINGLE_SCREEN_HIGH,
    FOUR_SCREEN     // richiede 2KB extra di nametable RAM, vedi nota in NESLoader.cpp
};

// ── Costanti finestra attiva (vedi spiegazione sopra) ─────
static constexpr uint32_t NES_PRG_SLOT_SIZE   = 0x2000;   // 8KB
static constexpr uint8_t  NES_PRG_SLOTS       = 4;        // 4*8KB = 32KB ($8000-$FFFF)
static constexpr uint32_t NES_PRG_WINDOW_SIZE = NES_PRG_SLOT_SIZE * NES_PRG_SLOTS;

static constexpr uint32_t NES_CHR_SLOT_SIZE   = 0x0400;   // 1KB
static constexpr uint8_t  NES_CHR_SLOTS       = 8;        // 8*1KB = 8KB ($0000-$1FFF)
static constexpr uint32_t NES_CHR_WINDOW_SIZE = NES_CHR_SLOT_SIZE * NES_CHR_SLOTS;

static constexpr uint32_t NES_PRG_RAM_SIZE    = 0x2000;   // 8KB @ $6000-$7FFF
static constexpr uint32_t NES_CHR_RAM_SIZE    = 0x2000;   // 8KB (se CHR-RAM)

// Soglia sotto la quale la ROM intera viene cache-ata in RAM (malloc).
//
// framebuffer RGB565 singolo da 256x240 (~120KB, vedi NesPPU::framebuffer)
// il budget residuo su un RP2040 da 264KB è molto stretto. Stima
// approssimativa:
//
//   framebuffer PPU                    ~120 KB
//   finestra attiva NESLoader (fissa)   ~56 KB  (32 PRG + 8 CHR-ROM + 8 CHR-RAM + 8 PRG-RAM)
//   RAM interna CPU + nametable/OAM      ~4 KB
//   ring/buffer audio                    ~5 KB
//   stack ×2 core + TFT_eSPI + varie    ~20 KB
//   ───────────────────────────────────────────
//   totale fisso                       ~205 KB   →   ~59 KB liberi
//
// Tenuto un margine di sicurezza, la soglia è impostata a 48KB: copre
// comunque la maggior parte dei giochi NROM/CNROM/UNROM piccoli (es.
// Donkey Kong, Balloon Fight, molti CNROM ≤40KB). Se sul binario
// reale risulta più RAM libera di quanto stimato, si puó alzare.
//static constexpr uint32_t NES_FULLCACHE_THRESHOLD = 48 * 1024;
static constexpr uint32_t NES_FULLCACHE_THRESHOLD = 96 * 1024;

class NESLoader {
public:
    NESLoader();
    ~NESLoader() { unloadROM(); }

    // ── Filesystem (identico a SMSLoader: SD poi LittleFS) ──
    bool     initFilesystem(FSType preferred = FSType::FS_SD, int sdCsPin = SD_CS);
    FSType   currentFS() const { return _currentFS; }
    bool     fsReady() const { return _fsReady; }
    void     closeFilesystem();

    // ── Caricamento ROM ───────────────────────────
    bool     loadROM(const char* path);
    void     unloadROM();
    bool     isLoaded() const { return _loaded; }

    // ── Interfaccia CPU (per cpu.begin) ───────────
    uint8_t  cpuRead(uint16_t addr);
    void     cpuWrite(uint16_t addr, uint8_t value);

    // ── Interfaccia PPU (per pattern table CHR) ───
    uint8_t  chrRead(uint16_t addr);
    void     chrWrite(uint16_t addr, uint8_t value);

    NESMirrorMode getMirroring() const { return _mirroring; }

    // ── MMC3 IRQ: chiamare una volta per scanline VISIBILE dopo
    // ppu.stepScanline(). Ritorna true nell'istante in cui l'IRQ va
    // asserito. NOTA APPROSSIMAZIONE: il vero MMC3 conta i fronti di
    // salita di A12 (collegato al fetch pattern table), qui si conta
    // una volta per scanline; funziona per la stragrande maggioranza dei
    // giochi (incl. split status bar di SMB3) ma non è ciclo-esatta.
    bool     notifyScanlineIRQ();
    bool     irqStillPending() const { return _mmc3IrqPending; }

    // ── Info / debug ───────────────────────────────
    uint32_t    prgSize() const { return _prgSize; }
    uint32_t    chrSize() const { return _chrSize; }
    NESMapperID mapperID() const { return _mapperId; }
    bool        hasBattery() const { return _hasBattery; }
    const char* romName() const { return _romName; }
    void        debugStatus() const;
    void        listROMs(const char* folder = "/roms");
    bool        fileExists(const char* path);
    void        setSelectedROM(const char* path);
    const char* getSelectedROM() const { return _romPath; }
    FSType      getCurrentFilesystem() const { return _currentFS; }
    File        openFile(const char* path, const char* mode);

    const uint8_t* prgWindow() const { return _prgCache; }
    const uint8_t* prgRamPtr() const { return _prgRAM; }
    const uint8_t* chrBase()   const { return _chrIsRAM ? _chrRAM : _chrCache; }  // valido dopo loadROM()

private:
    // ── Finestra attiva PRG (32KB) ────────────────
    uint8_t  _prgCache[NES_PRG_WINDOW_SIZE];
    uint16_t _prgSlotBank[NES_PRG_SLOTS];      // banco fisico (unità 8KB) attualmente caricato
    uint16_t _prgSlotWant[NES_PRG_SLOTS];      // banco fisico desiderato (ricalcolato dai mapper handler)

    // ── Finestra attiva CHR (8KB), se CHR-ROM ─────
    uint8_t  _chrCache[NES_CHR_WINDOW_SIZE];
    uint16_t _chrSlotBank[NES_CHR_SLOTS];
    uint16_t _chrSlotWant[NES_CHR_SLOTS];

    bool     _chrIsRAM = false;
    uint8_t  _chrRAM[NES_CHR_RAM_SIZE];

    uint8_t  _prgRAM[NES_PRG_RAM_SIZE];

    // ── Cache intera ROM in RAM (se piccola) ──────
    uint8_t* _romFullCache = nullptr;   // PRG seguito da CHR, contiguo
    bool     _fullyCached = false;

    // ── Info ROM (da header iNES) ──────────────────
    uint32_t    _prgSize = 0, _chrSize = 0;
    uint8_t     _prgBanks8k = 0;   // numero di banchi da 8KB nel PRG totale
    uint8_t     _chrBanks1k = 0;   // numero di banchi da 1KB nel CHR totale
    NESMapperID _mapperId = NESMapperID::NROM;
    NESMirrorMode _mirroring = NESMirrorMode::HORIZONTAL;
    bool        _hasBattery = false;
    bool        _hasTrainer = false;
    uint32_t    _headerOffset = 0;   // 16 (+512 se trainer) byte di offset prima del PRG nel file
    char        _romName[64];
    char        _romPath[160];
    bool        _loaded = false;

    // ── Stato specifico mapper ────────────────────
    // MMC1
    uint8_t  _mmc1ShiftReg = 0, _mmc1ShiftCount = 0;
    uint8_t  _mmc1Control = 0x0C;   // default: PRG 16KB mode, banco fisso ultimo
    uint8_t  _mmc1ChrBank0 = 0, _mmc1ChrBank1 = 0;
    uint8_t  _mmc1PrgBank = 0;
    bool     _mmc1PrgRamEnabled = true;

    // UNROM/CNROM
    uint8_t  _simpleBank = 0;       // unico registro bank-switch (mapper 2/3)

    // MMC3
    uint8_t  _mmc3BankSelect = 0;
    uint8_t  _mmc3Regs[8] = {0};
    bool     _mmc3PrgMode = false;  // bit6 di $8000
    bool     _mmc3ChrMode = false;  // bit7 di $8000
    bool     _mmc3Mirroring = false;
    uint8_t  _mmc3IrqLatch = 0, _mmc3IrqCounter = 0;
    bool     _mmc3IrqEnable = false, _mmc3IrqReloadFlag = false;
    bool     _mmc3IrqPending = false;

    // ── Filesystem ─────────────────────────────────
    FSType _currentFS = FSType::FS_NONE;
    bool   _fsReady = false;
    int    _sdCsPin = SD_CS;
    File   _romFile;
    bool   _fileOpen = false;
    bool   initSD();
    bool   openROMFile();
    void   closeROMFile();

    // ── Helpers ────────────────────────────────────
    bool     readBankData(uint32_t prgOrChrOffset, bool isChr, uint8_t* dst, uint32_t len);
    bool     readFromFile(uint32_t fileOffset, uint8_t* dst, uint32_t len);
    void     syncPrgSlots();
    void     syncChrSlots();
    void     extractName(const char* path);

    // Dispatch scrittura mapper-specifica (chiamata da cpuWrite per $8000-$FFFF)
    void     handleNromWrite(uint16_t addr, uint8_t val);   // no-op, ROM
    void     handleMmc1Write(uint16_t addr, uint8_t val);
    void     handleUnromWrite(uint16_t addr, uint8_t val);
    void     handleCnromWrite(uint16_t addr, uint8_t val);
    void     handleMmc3Write(uint16_t addr, uint8_t val);

    void     recalcMmc1Slots();
    void     recalcMmc3Slots();
};
