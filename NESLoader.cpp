/*
 * NESLoader.cpp — Copyright (C) 2026 Ivan Vettori — GPLv3
 * Basato sul pattern di SMSLoader (progetto SMS Emulator / PiMSX).
 * Vedi NESLoader.h per le note architetturali (finestra attiva fissa
 * 32KB PRG + 8KB CHR, cache intera ROM se piccola, altrimenti swap).
 */
#include "NESLoader.h"

NESLoader::NESLoader() {
    memset(_prgCache, 0, sizeof(_prgCache));
    memset(_chrCache, 0, sizeof(_chrCache));
    memset(_chrRAM, 0, sizeof(_chrRAM));
    memset(_prgRAM, 0, sizeof(_prgRAM));
    for (int i = 0; i < NES_PRG_SLOTS; i++) { _prgSlotBank[i] = 0xFFFF; _prgSlotWant[i] = 0; }
    for (int i = 0; i < NES_CHR_SLOTS; i++) { _chrSlotBank[i] = 0xFFFF; _chrSlotWant[i] = 0; }
    _romName[0] = '\0';
    _romPath[0] = '\0';
}

// ============================================================================
// Filesystem — identico al pattern SMSLoader (SD poi LittleFS, o viceversa)
// ============================================================================
bool NESLoader::initFilesystem(FSType preferred, int sdCsPin) {
    if (_fsReady) return true;
    _sdCsPin = sdCsPin;

    auto trySD = [&]() -> bool {
        Serial.println("[NESLoader] SD...");
        if (initSD()) { _currentFS = FSType::FS_SD; _fsReady = true; Serial.println("[NESLoader] SD OK"); return true; }
        Serial.println("[NESLoader] SD non disponibile");
        return false;
    };
    auto tryLFS = [&]() -> bool {
        Serial.println("[NESLoader] LittleFS...");
        if (LittleFS.begin()) { _currentFS = FSType::FS_LITTLEFS; _fsReady = true; Serial.println("[NESLoader] LittleFS OK"); return true; }
        Serial.println("[NESLoader] LittleFS non disponibile");
        return false;
    };

    if (preferred == FSType::FS_SD) { if (trySD()) return true; if (tryLFS()) return true; }
    else                            { if (tryLFS()) return true; if (trySD()) return true; }

    Serial.println("[NESLoader] ERRORE: nessun filesystem disponibile");
    return false;
}

void NESLoader::closeFilesystem() {
    closeROMFile();
    if (_currentFS == FSType::FS_SD) { SD.end(); Serial.println("[NESLoader] SD chiusa"); }
    _fsReady = false;
    _currentFS = FSType::FS_NONE;
}

bool NESLoader::initSD() {
    SPI1.setRX(SD_MISO);
    SPI1.setTX(SD_MOSI);
    SPI1.setSCK(SD_SCLK);
    SPI1.setCS(SD_CS);
    SPI1.begin();
    delay(100);
    return SD.begin(SD_CS, SPI1);
}

bool NESLoader::openROMFile() {
    if (_fileOpen) return true;
    if (!_fsReady) return false;
    if (_currentFS == FSType::FS_SD) _romFile = SD.open(_romPath, FILE_READ);
    else                             _romFile = LittleFS.open(_romPath, "r");
    _fileOpen = (bool)_romFile;
    if (!_fileOpen) Serial.printf("[NESLoader] ERRORE: impossibile aprire %s\n", _romPath);
    return _fileOpen;
}
void NESLoader::closeROMFile() { if (_fileOpen) { _romFile.close(); _fileOpen = false; } }

bool NESLoader::readFromFile(uint32_t offset, uint8_t* dst, uint32_t len) {
    if (!_fileOpen) { if (!openROMFile()) return false; }
    if (!_romFile.seek(offset)) { Serial.printf("[NESLoader] ERRORE seek %lu\n", offset); return false; }
    return _romFile.read(dst, len) == len;
}

void NESLoader::extractName(const char* path) {
    if (!path) { _romName[0] = '\0'; return; }
    const char* slash = strrchr(path, '/');
    if (!slash) slash = strrchr(path, '\\');
    const char* base = slash ? slash + 1 : path;
    strncpy(_romName, base, sizeof(_romName) - 1);
    _romName[sizeof(_romName) - 1] = '\0';
    char* dot = strrchr(_romName, '.');
    if (dot) *dot = '\0';
}

// ============================================================================
// readBankData: ponte fra "voglio il banco fisico N" e "dove sta davvero"
// (RAM cache intera oppure file). Vedi NESLoader.h.
// ============================================================================
bool NESLoader::readBankData(uint32_t offset, bool isChr, uint8_t* dst, uint32_t len) {
    if (_fullyCached) {
        uint32_t base = isChr ? _prgSize : 0;
        memcpy(dst, _romFullCache + base + offset, len);
        return true;
    }
    uint32_t fileOff = _headerOffset + (isChr ? _prgSize : 0) + offset;
    return readFromFile(fileOff, dst, len);
}

void NESLoader::syncPrgSlots() {
    for (int s = 0; s < NES_PRG_SLOTS; s++) {
        if (_prgSlotBank[s] == _prgSlotWant[s]) continue;
        uint16_t bank = _prgSlotWant[s];
        if (_prgBanks8k > 0) bank = bank % _prgBanks8k;   // mirroring se la ROM è più piccola del richiesto
        readBankData((uint32_t)bank * NES_PRG_SLOT_SIZE, false,
                     &_prgCache[s * NES_PRG_SLOT_SIZE], NES_PRG_SLOT_SIZE);
        _prgSlotBank[s] = _prgSlotWant[s];
    }
}
void NESLoader::syncChrSlots() {
    if (_chrIsRAM) return;
    for (int s = 0; s < NES_CHR_SLOTS; s++) {
        if (_chrSlotBank[s] == _chrSlotWant[s]) continue;
        uint16_t bank = _chrSlotWant[s];
        if (_chrBanks1k > 0) bank = bank % _chrBanks1k;
        readBankData((uint32_t)bank * NES_CHR_SLOT_SIZE, true,
                     &_chrCache[s * NES_CHR_SLOT_SIZE], NES_CHR_SLOT_SIZE);
        _chrSlotBank[s] = _chrSlotWant[s];
    }
}

// ============================================================================
// Interfaccia CPU
// ============================================================================
uint8_t NESLoader::cpuRead(uint16_t addr) {
    if (addr >= 0x8000) {
        uint16_t slot = (addr - 0x8000) / NES_PRG_SLOT_SIZE;
        uint16_t off  = addr & (NES_PRG_SLOT_SIZE - 1);
        return _prgCache[slot * NES_PRG_SLOT_SIZE + off];
    }
    if (addr >= 0x6000) return _prgRAM[addr - 0x6000];
    return 0xFF; // $4020-$5FFF: non mappato sui mapper supportati
}

void NESLoader::cpuWrite(uint16_t addr, uint8_t val) {
    if (addr >= 0x8000) {
        switch (_mapperId) {
            case NESMapperID::NROM:  handleNromWrite(addr, val); break;
            case NESMapperID::MMC1:  handleMmc1Write(addr, val); break;
            case NESMapperID::UNROM: handleUnromWrite(addr, val); break;
            case NESMapperID::CNROM: handleCnromWrite(addr, val); break;
            case NESMapperID::MMC3:  handleMmc3Write(addr, val); break;
            default: break;
        }
        return;
    }
    if (addr >= 0x6000) {
        if (_mapperId != NESMapperID::MMC1 || _mmc1PrgRamEnabled) _prgRAM[addr - 0x6000] = val;
    }
}

// ============================================================================
// Interfaccia PPU (CHR)
// ============================================================================
uint8_t NESLoader::chrRead(uint16_t addr) {
    addr &= 0x1FFF;
    if (_chrIsRAM) return _chrRAM[addr];
    uint16_t slot = addr / NES_CHR_SLOT_SIZE;
    uint16_t off  = addr & (NES_CHR_SLOT_SIZE - 1);
    return _chrCache[slot * NES_CHR_SLOT_SIZE + off];
}
void NESLoader::chrWrite(uint16_t addr, uint8_t val) {
    addr &= 0x1FFF;
    if (_chrIsRAM) _chrRAM[addr] = val;
    // CHR-ROM: scrittura ignorata (è ROM)
}

// ============================================================================
// Mapper 0 — NROM (nessun registro)
// ============================================================================
void NESLoader::handleNromWrite(uint16_t, uint8_t) { /* no-op */ }

// ============================================================================
// Mapper 2 — UNROM/UOROM: 16KB switchable @ $8000, 16KB fisso (ultimo) @ $C000
// ============================================================================
void NESLoader::handleUnromWrite(uint16_t, uint8_t val) {
    _simpleBank = val;
    uint8_t banks16k = _prgBanks8k / 2; if (banks16k == 0) banks16k = 1;
    uint8_t mask = 1; while (mask < banks16k) mask <<= 1; mask -= 1;
    uint8_t bank = val & mask;
    _prgSlotWant[0] = bank * 2;
    _prgSlotWant[1] = bank * 2 + 1;
    _prgSlotWant[2] = (banks16k - 1) * 2;
    _prgSlotWant[3] = (banks16k - 1) * 2 + 1;
    syncPrgSlots();
}

// ============================================================================
// Mapper 3 — CNROM: PRG fisso, CHR switchable a banchi da 8KB
// ============================================================================
void NESLoader::handleCnromWrite(uint16_t, uint8_t val) {
    _simpleBank = val;
    uint8_t banks8kChr = _chrBanks1k / 8; if (banks8kChr == 0) banks8kChr = 1;
    uint8_t mask = 1; while (mask < banks8kChr) mask <<= 1; mask -= 1;
    uint8_t bank = val & mask;
    for (int s = 0; s < 8; s++) _chrSlotWant[s] = bank * 8 + s;
    syncChrSlots();
}

// ============================================================================
// Mapper 1 — MMC1
// ============================================================================
void NESLoader::handleMmc1Write(uint16_t addr, uint8_t val) {
    if (val & 0x80) {
        // Reset shift register; l'hardware reale forza anche la modalità
        // PRG a "16KB, fissa l'ultimo banco" (bits 2-3 del control = 3).
        _mmc1ShiftReg = 0;
        _mmc1ShiftCount = 0;
        _mmc1Control |= 0x0C;
        recalcMmc1Slots();
        return;
    }

    _mmc1ShiftReg = (_mmc1ShiftReg >> 1) | ((val & 1) << 4);
    _mmc1ShiftCount++;
    if (_mmc1ShiftCount < 5) return;

    uint8_t data = _mmc1ShiftReg & 0x1F;
    _mmc1ShiftReg = 0;
    _mmc1ShiftCount = 0;

    uint8_t which = (addr >> 13) & 3;   // 0=$8000 control, 1=$A000 chr0, 2=$C000 chr1, 3=$E000 prg
    switch (which) {
        case 0: _mmc1Control  = data; break;
        case 1: _mmc1ChrBank0 = data; break;
        case 2: _mmc1ChrBank1 = data; break;
        case 3: _mmc1PrgBank  = data & 0x0F; _mmc1PrgRamEnabled = !(data & 0x10); break;
    }
    recalcMmc1Slots();
}

void NESLoader::recalcMmc1Slots() {
    // Mirroring (bits 0-1 del control)
    switch (_mmc1Control & 0x03) {
        case 0: _mirroring = NESMirrorMode::SINGLE_SCREEN_LOW; break;
        case 1: _mirroring = NESMirrorMode::SINGLE_SCREEN_HIGH; break;
        case 2: _mirroring = NESMirrorMode::VERTICAL; break;
        case 3: _mirroring = NESMirrorMode::HORIZONTAL; break;
    }

    uint8_t prgMode = (_mmc1Control >> 2) & 3;
    uint8_t banks16k = _prgBanks8k / 2; if (banks16k == 0) banks16k = 1;

    if (prgMode <= 1) {
        // 32KB mode: bit0 del registro prg ignorato
        uint8_t bank32k = (_mmc1PrgBank >> 1);
        uint8_t base = (bank32k * 4) % (banks16k * 2 > 0 ? banks16k * 2 : 1);
        for (int s = 0; s < 4; s++) _prgSlotWant[s] = base + s;
    } else if (prgMode == 2) {
        // primo 16KB fisso a banco 0, ultimo switchable
        _prgSlotWant[0] = 0; _prgSlotWant[1] = 1;
        _prgSlotWant[2] = _mmc1PrgBank * 2;     _prgSlotWant[3] = _mmc1PrgBank * 2 + 1;
    } else {
        // primo 16KB switchable, ultimo fisso (modalità più comune)
        _prgSlotWant[0] = _mmc1PrgBank * 2;     _prgSlotWant[1] = _mmc1PrgBank * 2 + 1;
        _prgSlotWant[2] = (banks16k - 1) * 2;   _prgSlotWant[3] = (banks16k - 1) * 2 + 1;
    }
    syncPrgSlots();

    if (!_chrIsRAM) {
        bool chr4kMode = (_mmc1Control >> 4) & 1;
        if (chr4kMode) {
            for (int i = 0; i < 4; i++) _chrSlotWant[i]     = _mmc1ChrBank0 * 4 + i;
            for (int i = 0; i < 4; i++) _chrSlotWant[4 + i] = _mmc1ChrBank1 * 4 + i;
        } else {
            uint8_t bank8k = _mmc1ChrBank0 >> 1;
            for (int i = 0; i < 8; i++) _chrSlotWant[i] = bank8k * 8 + i;
        }
        syncChrSlots();
    }
}

// ============================================================================
// Mapper 4 — MMC3
// ============================================================================
void NESLoader::handleMmc3Write(uint16_t addr, uint8_t val) {
    if (addr <= 0x9FFF) {
        if ((addr & 1) == 0) { _mmc3BankSelect = val; _mmc3PrgMode = val & 0x40; _mmc3ChrMode = val & 0x80; }
        else                 { _mmc3Regs[_mmc3BankSelect & 7] = val; recalcMmc3Slots(); }
    } else if (addr <= 0xBFFF) {
        if ((addr & 1) == 0) {
            _mmc3Mirroring = val & 1; // 0=vertical, 1=horizontal
            _mirroring = _mmc3Mirroring ? NESMirrorMode::HORIZONTAL : NESMirrorMode::VERTICAL;
        }
        // $A001 (PRG-RAM protect/enable): non implementato, vedi nota in header
    } else if (addr <= 0xDFFF) {
        if ((addr & 1) == 0) _mmc3IrqLatch = val;
        else                 _mmc3IrqReloadFlag = true;
    } else {
        if ((addr & 1) == 0) { _mmc3IrqEnable = false; _mmc3IrqPending = false; }
        else                 { _mmc3IrqEnable = true; }
    }
    if (addr <= 0x9FFF) recalcMmc3Slots();
}

void NESLoader::recalcMmc3Slots() {
    uint8_t R6 = _mmc3Regs[6], R7 = _mmc3Regs[7];
    uint8_t lastBank  = (_prgBanks8k > 0) ? (_prgBanks8k - 1) : 0;
    uint8_t secondLast = (_prgBanks8k > 1) ? (_prgBanks8k - 2) : 0;

    if (!_mmc3PrgMode) {
        _prgSlotWant[0] = R6; _prgSlotWant[1] = R7;
        _prgSlotWant[2] = secondLast; _prgSlotWant[3] = lastBank;
    } else {
        _prgSlotWant[0] = secondLast; _prgSlotWant[1] = R7;
        _prgSlotWant[2] = R6;         _prgSlotWant[3] = lastBank;
    }
    syncPrgSlots();

    if (!_chrIsRAM) {
        uint8_t R0 = _mmc3Regs[0] & 0xFE, R1 = _mmc3Regs[1] & 0xFE;
        uint8_t R2 = _mmc3Regs[2], R3 = _mmc3Regs[3], R4 = _mmc3Regs[4], R5 = _mmc3Regs[5];
        if (!_mmc3ChrMode) {
            _chrSlotWant[0]=R0; _chrSlotWant[1]=R0+1; _chrSlotWant[2]=R1; _chrSlotWant[3]=R1+1;
            _chrSlotWant[4]=R2; _chrSlotWant[5]=R3;   _chrSlotWant[6]=R4; _chrSlotWant[7]=R5;
        } else {
            _chrSlotWant[0]=R2; _chrSlotWant[1]=R3;   _chrSlotWant[2]=R4; _chrSlotWant[3]=R5;
            _chrSlotWant[4]=R0; _chrSlotWant[5]=R0+1; _chrSlotWant[6]=R1; _chrSlotWant[7]=R1+1;
        }
        syncChrSlots();
    }
}

bool NESLoader::notifyScanlineIRQ() {
    // Approssimazione "una volta per scanline" — vedi nota in NESLoader.h
    if (_mmc3IrqCounter == 0 || _mmc3IrqReloadFlag) {
        _mmc3IrqCounter = _mmc3IrqLatch;
        _mmc3IrqReloadFlag = false;
    } else {
        _mmc3IrqCounter--;
    }
    if (_mmc3IrqCounter == 0 && _mmc3IrqEnable) {
        _mmc3IrqPending = true;
        return true;
    }
    return false;
}

// ============================================================================
// loadROM — parsing header iNES + setup iniziale mapper
// ============================================================================
bool NESLoader::loadROM(const char* path) {
    if (!path || path[0] == '\0') return false;
    if (!_fsReady && !initFilesystem()) {
        Serial.println("[NESLoader] ERRORE: filesystem non disponibile");
        return false;
    }
    if (_loaded) unloadROM();

    strncpy(_romPath, path, sizeof(_romPath) - 1);
    _romPath[sizeof(_romPath) - 1] = '\0';
    extractName(path);

    Serial.printf("\n[NESLoader] Caricamento: %s\n", path);
    if (!openROMFile()) return false;

    uint8_t header[16];
    if (!readFromFile(0, header, 16)) { Serial.println("[NESLoader] ERRORE lettura header"); closeROMFile(); return false; }
    if (header[0] != 'N' || header[1] != 'E' || header[2] != 'S' || header[3] != 0x1A) {
        Serial.println("[NESLoader] ERRORE: non è un file iNES valido");
        closeROMFile(); return false;
    }

    // NOTA: (flags7 bit2-3 == 10) non viene riconosciuto/parsato
    // nei suoi campi estesi: i giochi con PRG/CHR molto grandi codificati
    // solo in NES2.0 potrebbero leggere dimensioni errate. Per i mapper
    // 0/1/2/3/4 con ROM "normali" l'header iNES 1.0 è quasi sempre
    // sufficiente.
    uint8_t prg16kCount = header[4];
    uint8_t chr8kCount  = header[5];
    uint8_t flags6 = header[6];
    uint8_t flags7 = header[7];

    _hasTrainer = flags6 & 0x04;
    _hasBattery = flags6 & 0x02;
    bool fourScreen = flags6 & 0x08;
    uint8_t mapperNum = ((flags6 >> 4) & 0x0F) | (((flags7 >> 4) & 0x0F) << 4);

    _mirroring = fourScreen ? NESMirrorMode::FOUR_SCREEN
                            : ((flags6 & 1) ? NESMirrorMode::VERTICAL : NESMirrorMode::HORIZONTAL);

    switch (mapperNum) {
        case 0: _mapperId = NESMapperID::NROM;  break;
        case 1: _mapperId = NESMapperID::MMC1;  break;
        case 2: _mapperId = NESMapperID::UNROM; break;
        case 3: _mapperId = NESMapperID::CNROM; break;
        case 4: _mapperId = NESMapperID::MMC3;  break;
        default:
            Serial.printf("[NESLoader] ERRORE: mapper %d non supportato\n", mapperNum);
            _mapperId = NESMapperID::UNSUPPORTED;
            closeROMFile();
            return false;
    }

    _prgSize = (uint32_t)prg16kCount * 0x4000;
    _chrSize = (uint32_t)chr8kCount  * 0x2000;
    _prgBanks8k = prg16kCount * 2;
    _chrBanks1k = chr8kCount  * 8;
    _chrIsRAM = (chr8kCount == 0);
    _headerOffset = 16 + (_hasTrainer ? 512 : 0);

    if (_prgSize == 0) {
        Serial.println("[NESLoader] ERRORE: PRG-ROM vuota");
        closeROMFile(); return false;
    }

    // ── Decidi cache intera vs swap ────────────────
    uint32_t totalRomData = _prgSize + (_chrIsRAM ? 0 : _chrSize);
    if (_romFullCache) { free(_romFullCache); _romFullCache = nullptr; }
    if (totalRomData <= NES_FULLCACHE_THRESHOLD) {
        _romFullCache = (uint8_t*)malloc(totalRomData);
        if (_romFullCache) {
            if (!readFromFile(_headerOffset, _romFullCache, totalRomData)) {
                Serial.println("[NESLoader] ERRORE lettura ROM in cache, fallback su swap da file");
                free(_romFullCache); _romFullCache = nullptr; _fullyCached = false;
            } else {
                _fullyCached = true;
                closeROMFile();
                Serial.printf("[NESLoader] ROM interamente in RAM (%lu KB)\n", totalRomData / 1024);
            }
        } else {
            Serial.println("[NESLoader] malloc fallito per cache intera, uso swap da file");
            _fullyCached = false;
        }
    } else {
        _fullyCached = false;
        Serial.printf("[NESLoader] ROM grande (%lu KB): modalità swap da %s\n",
                      totalRomData / 1024, _currentFS == FSType::FS_SD ? "SD" : "LittleFS");
    }

    // ── Stato iniziale mapper ──────────────────────
    for (int i = 0; i < NES_PRG_SLOTS; i++) _prgSlotBank[i] = 0xFFFF;
    for (int i = 0; i < NES_CHR_SLOTS; i++) _chrSlotBank[i] = 0xFFFF;
    memset(_chrRAM, 0, sizeof(_chrRAM));
    memset(_prgRAM, 0, sizeof(_prgRAM));

    uint8_t banks16k = _prgBanks8k / 2; if (banks16k == 0) banks16k = 1;

    switch (_mapperId) {
        case NESMapperID::NROM:
            if (banks16k == 1) { _prgSlotWant[0]=0; _prgSlotWant[1]=1; _prgSlotWant[2]=0; _prgSlotWant[3]=1; }
            else                { for (int s=0;s<4;s++) _prgSlotWant[s]=s; }
            syncPrgSlots();
            if (!_chrIsRAM) { for (int s=0;s<8;s++) _chrSlotWant[s]=s; syncChrSlots(); }
            break;

        case NESMapperID::UNROM:
            _prgSlotWant[0]=0; _prgSlotWant[1]=1;
            _prgSlotWant[2]=(banks16k-1)*2; _prgSlotWant[3]=(banks16k-1)*2+1;
            syncPrgSlots();
            if (!_chrIsRAM) { for (int s=0;s<8;s++) _chrSlotWant[s]=s; syncChrSlots(); }
            break;

        case NESMapperID::CNROM:
            if (banks16k == 1) { _prgSlotWant[0]=0; _prgSlotWant[1]=1; _prgSlotWant[2]=0; _prgSlotWant[3]=1; }
            else                { for (int s=0;s<4;s++) _prgSlotWant[s]=s; }
            syncPrgSlots();
            for (int s=0;s<8;s++) _chrSlotWant[s]=s;
            syncChrSlots();
            break;

        case NESMapperID::MMC1:
            _mmc1Control = 0x0C; _mmc1PrgBank = 0; _mmc1ChrBank0 = 0; _mmc1ChrBank1 = 0;
            _mmc1ShiftReg = 0; _mmc1ShiftCount = 0; _mmc1PrgRamEnabled = true;
            recalcMmc1Slots();
            break;

        case NESMapperID::MMC3:
            memset(_mmc3Regs, 0, sizeof(_mmc3Regs));
            _mmc3BankSelect = 0; _mmc3PrgMode = false; _mmc3ChrMode = false;
            _mmc3IrqLatch = 0; _mmc3IrqCounter = 0; _mmc3IrqEnable = false;
            _mmc3IrqReloadFlag = false; _mmc3IrqPending = false;
            recalcMmc3Slots();
            break;

        default: break;
    }

    _loaded = true;
    Serial.printf("[NESLoader] ROM: %-32s  PRG:%lu KB  CHR:%s  Mapper:%d  Mirror:%d\n",
                  _romName, _prgSize/1024,
                  _chrIsRAM ? "RAM 8KB" : (String(_chrSize/1024)+"KB").c_str(),
                  mapperNum, (int)_mirroring);
    return true;
}

void NESLoader::unloadROM() {
    closeROMFile();
    if (_romFullCache) { free(_romFullCache); _romFullCache = nullptr; }
    _fullyCached = false;
    _loaded = false;
    _romName[0] = '\0';
    _romPath[0] = '\0';
}

// ============================================================================
// Debug / utility (pattern identico a SMSLoader)
// ============================================================================
void NESLoader::debugStatus() const {
    if (!_loaded) { Serial.println("[NESLoader] Nessuna ROM caricata"); return; }
    Serial.println("\n=== NESLoader Status ===");
    Serial.printf("ROM     : %s\n", _romName);
    Serial.printf("PRG     : %lu KB (%d banchi da 8KB)\n", _prgSize/1024, _prgBanks8k);
    Serial.printf("CHR     : %s\n", _chrIsRAM ? "RAM 8KB" : (String(_chrSize/1024)+" KB").c_str());
    Serial.printf("Mapper  : %d\n", (int)_mapperId);
    Serial.printf("Mirror  : %d\n", (int)_mirroring);
    Serial.printf("Storage : %s\n", _fullyCached ? "RAM (full cache)" : "swap da file");
    Serial.println("=========================\n");
}

bool NESLoader::fileExists(const char* path) {
    if (_currentFS == FSType::FS_SD) return SD.exists(path);
    if (_currentFS == FSType::FS_LITTLEFS) return LittleFS.exists(path);
    return false;
}
void NESLoader::setSelectedROM(const char* path) {
    if (!path) return;
    strncpy(_romPath, path, sizeof(_romPath) - 1);
    _romPath[sizeof(_romPath) - 1] = '\0';
}
File NESLoader::openFile(const char* path, const char* mode) {
    if (_currentFS == FSType::FS_SD) return SD.open(path, mode);
    if (_currentFS == FSType::FS_LITTLEFS) return LittleFS.open(path, mode);
    return File();
}

void NESLoader::listROMs(const char* folder) {
    if (!_fsReady) { Serial.println("[NESLoader] Filesystem non pronto"); return; }
    Serial.printf("ROM in %s:\n==========================================\n", folder);

    auto isROM = [](const char* name) -> bool {
        size_t len = strlen(name);
        if (len < 5) return false;
        const char* ext = name + len - 4;
        return strcasecmp(ext, ".nes") == 0;
    };

    if (_currentFS == FSType::FS_SD) {
        File root = SD.open(folder);
        if (!root) { Serial.println("  ERRORE: cartella non trovata"); return; }
        File entry = root.openNextFile();
        while (entry) {
            if (!entry.isDirectory() && isROM(entry.name()))
                Serial.printf("  %-40s  %6lu KB\n", entry.name(), (uint32_t)entry.size()/1024);
            entry.close();
            entry = root.openNextFile();
        }
        root.close();
    } else {
        Dir dir = LittleFS.openDir(folder);
        while (dir.next()) {
            if (isROM(dir.fileName().c_str())) {
                File f = dir.openFile("r");
                if (f) { Serial.printf("  %-40s  %6lu KB\n", dir.fileName().c_str(), (uint32_t)f.size()/1024); f.close(); }
            }
        }
    }
    Serial.println("==========================================");
}
