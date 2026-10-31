/*
 * PiNES.ino — Emulatore NES per Raspberry Pi Pico (RP2040), Arduino IDE
 * Copyright (C) 2026 Ivan Vettori — GPLv3
 *
 * ============================================================================
 * ARCHITETTURA (v7)
 *
 *  Core0:  CPU 6502 + APU + PPU (logica + render dei frame "mostrati").
 *          Non aspetta MAI Core1. E' "pilotato" dall'audio: quando il ring
 *          audio e' sopra AUDIO_TARGET_FILL si ferma finche' l'I2S non lo
 *          svuota (paceAudio) => velocita' di gioco = clock del DAC.
 *  Core1:  (a) IRQ DMA dell'I2S (MAX98357) che preleva dal ring audio;
 *          (b) loop1(): consuma le righe pronte di ppu.fb[] (indice palette
 *              0-63), converte in RGB565 (palette gia' byte-swappata) e
 *              spinge in DMA verso l'ILI9341 a blocchi di PUSH_LINES righe.
 *
 *  Sacrificare il framerate, mai la velocita':
 *   - se Core1 e' ancora occupato col frame precedente, il nuovo frame non
 *     viene disegnato (nessuna attesa su Core0);
 *   - frameskip ADATTIVO: se l'audio va in underrun si salta di piu', se
 *     Core0 ha tempo libero (idle in paceAudio) si salta di meno.
 *
 *  Il codice caldo (CPU, PPU, APU, loop frame, conversione Core1) e' in RAM
 *  (PINES_RAM) per non thrashare la cache XIP condivisa dai due core.
 *
 * ============================================================================
 * UI DI SELEZIONE ROM (v8) — pattern Pi64 (C64Menu/C64GameBrowser)
 *
 *  Stati Core0:  STATE_MENU -> STATE_BROWSER -> STATE_EMULATION
 *   - setup() mostra NESMenu (voce unica GAMES BROWSER);
 *   - alla conferma (A/START) si passa al NESGameBrowser su /roms:
 *     navigazione LEFT/RIGHT, anteprime JPG, info header iNES;
 *   - B/SELECT dal browser riporta al menu; A/START lancia la ROM;
 *   - startEmulation() esegue il setup che in v7 stava in setup()
 *     (loadROM + ppu/apu/cpu.begin) e sblocca Core1 (emuReady);
 *   - dal gioco si esce con la combo SELECT+START+A+B tenuta ~1 secondo:
 *     rp2040.restart() (stesso approccio del REBOOT_BUTTON di Pi64), quindi
 *     si riparte dal menu senza teardown manuale di audio/DMA/core1.
 *
 *  Core1 resta fermo in setup1() (attesa emuReady) per tutto il tempo della
 *  UI: la navigazione non tocca IRQ, ring audio o DMA del display.
 * ============================================================================
 */

#include <TFT_eSPI.h>
#include "hardware/vreg.h"
#include "hardware/clocks.h"
#include "hardware/timer.h"

#include "pines_ram.h"
#include "cpu.h"
#include "nes_ppu.h"
#include "nes_apu.h"
#include "NESLoader.h"
#include "NesJoystick.h"
#include "nes_joystick_config.h"
#include "NesAudioOut.h"
#include "nes_palette.h"      // NES_PALETTE_RGB565 (copiata in RAM, byte-swappata)

#include "NESMenu.h"
#include "NESGameBrowser.h"
#include "PiNESLogo.h"

// ============================================================================
// CONFIGURAZIONE
// ============================================================================
#define NES_STATS            1      // riga di statistiche ogni ~2s su Serial
#define NES_PROFILE          0      // 1 = tempi CPU/APU/PPU per frame (costa un po')

#define AUDIO_BCLK_GPIO      26     // LRCLK = GP27 (BCLK+1), imposto dalla libreria I2S
#define AUDIO_DIN_GPIO       28
#define NES_CPU_CLOCK_NTSC   1789773UL
#define DISPLAY_X_OFFSET     32     // (320-256)/2

// Pacing audio: il ring viene tenuto attorno a questo livello (campioni).
// 1024 @32kHz = 32ms. Piu' alto = piu' tolleranza ai picchi, piu' latenza A/V.
#define AUDIO_TARGET_FILL    1024

// Frameskip adattivo (0 = mostra ogni frame, N = mostra 1 frame ogni N+1)
//#define ADAPTIVE_SKIP        1
#define ADAPTIVE_SKIP        0
#define SKIP_MIN             0
#define SKIP_MAX             4
//#define SKIP_START           2
#define SKIP_START           1
#define ADAPT_WINDOW_FRAMES  64
#define ADAPT_IDLE_UP_US     5000   // idle medio/frame sopra cui si prova a saltare meno

#define PUSH_LINES           8      // righe per ogni DMA verso il display (240 % PUSH_LINES == 0)

// ============================================================================
// OGGETTI GLOBALI
// ============================================================================
cpu         nesCPU;
NesPPU      ppu;
NesAPU      apu;
NESLoader   nesLoader;
NesAudioOut audioOut;
TFT_eSPI    tft = TFT_eSPI();

NesJoystick *joy1 = nullptr, *joy2 = nullptr;

static volatile bool emuReady = false;      // Core0 -> Core1: emulazione avviata (clock e TFT pronti)

// Palette RGB565 byte-swappata (formato che l'ILI9341 vuole sul bus) in RAM.
// Cosi' tft.setSwapBytes(false) e pushPixelsDMA non rifa' lo swap pixel per pixel.
static uint16_t palSwapped[64] __attribute__((aligned(4)));

// ============================================================================
// UI — stato applicazione, menu e browser (pattern Pi64)
// ============================================================================
enum AppState {
    STATE_MENU,        // NESMenu a schermo
    STATE_BROWSER,     // NESGameBrowser a schermo
    STATE_EMULATION    // emulazione in corso
};
AppState currentState = STATE_MENU;

NESMenu*        menu        = nullptr;
NESGameBrowser* gameBrowser = nullptr;
NesJoystick*    uiJoy       = nullptr;      // pad usato da menu e browser (porta 1)

// Combo di uscita dal gioco: SELECT+START+A+B tenuta ~1 secondo.
// Costo: una lettura pad (8 digitalRead) per frame, trascurabile rispetto
// ai ~16.6ms di budget. Il reboot evita qualsiasi teardown di audio/DMA.
#define EXIT_COMBO_MASK   (NES_BTN_A | NES_BTN_B | NES_BTN_SELECT | NES_BTN_START)
#define EXIT_COMBO_FRAMES 60               // ~1 secondo a 60 fps

// ============================================================================
// JOYPAD — $4016/$4017
// ============================================================================
static uint8_t joy1Shift = 0, joy2Shift = 0;
static bool joyStrobe = false;

// ============================================================================
// BUS GLUE
// ============================================================================
// PRG ($8000+) e PRG-RAM ($6000-$7FFF) in lettura non passano piu' di qui:
// la CPU le legge direttamente dalle finestre del loader (cpu::setFastMemory).
static uint8_t PINES_RAM nesMemRead(uint16_t addr)             { return nesLoader.cpuRead(addr); }
static void    PINES_RAM nesMemWrite(uint16_t addr, uint8_t v) { nesLoader.cpuWrite(addr, v); }

static uint8_t PINES_RAM nesIoRead(uint16_t addr) {
    if (addr <= 0x3FFF) return ppu.regRead(addr);
    if (addr == 0x4015) return apu.regRead(addr);
    if (addr == 0x4016) {
        uint8_t bit = joy1Shift & 1;
        if (!joyStrobe) joy1Shift >>= 1;
        return bit | 0x40;
    }
    if (addr == 0x4017) {
        uint8_t bit = joy2Shift & 1;
        if (!joyStrobe) joy2Shift >>= 1;
        return bit | 0x40;
    }
    return 0xFF;
}

static void PINES_RAM nesIoWrite(uint16_t addr, uint8_t val) {
    if (addr <= 0x3FFF) { ppu.regWrite(addr, val); return; }
    if (addr == 0x4014) {
        ppu.regWrite(addr, val);
        nesCPU.stealCycles(513);
        return;
    }
    if (addr == 0x4016) {
        bool newStrobe = val & 1;
        if (newStrobe) {
            joy1Shift = joy1 ? joy1->read() : 0;
            joy2Shift = joy2 ? joy2->read() : 0;
        }
        joyStrobe = newStrobe;
        return;
    }
    // FIX: prima mancava $4015 => i canali non venivano mai abilitati (silenzio).
    if ((addr >= 0x4000 && addr <= 0x4013) || addr == 0x4015 || addr == 0x4017) {
        apu.regWrite(addr, val);
        return;
    }
}

static uint8_t PINES_RAM nesCpuPageRead(uint16_t addr) { return nesCPU.mem_read(addr); }
static void    PINES_RAM nesChrWrite(uint16_t addr, uint8_t v) { nesLoader.chrWrite(addr, v); }

static uint8_t PINES_RAM nesMirrorModeFn() {
    switch (nesLoader.getMirroring()) {
        case NESMirrorMode::VERTICAL:           return 1;
        case NESMirrorMode::SINGLE_SCREEN_LOW:  return 2;
        case NESMirrorMode::SINGLE_SCREEN_HIGH: return 3;
        default:                                return 0;
    }
}

static void PINES_RAM audioPushSampleWrapper(int16_t s) { audioOut.pushSample(s); }

// ============================================================================
// STATO DI PACING / FRAMESKIP / STATISTICHE (solo Core0)
// ============================================================================
static bool     paceEnabled   = true;       // si autodisabilita se l'audio non scorre
static uint32_t idleAccumUs   = 0;          // tempo passato in attesa del DAC (= tempo libero)
static uint8_t  skipLevel     = SKIP_START;
static uint8_t  skipCtr       = 0;
static uint32_t frameNo       = 0;          // contatore frame per la finestra adattiva (non azzerato dalle stats)

struct Stats {
    uint32_t frames = 0, shown = 0, droppedBusy = 0;
    uint32_t windowStartUs = 0;
    uint32_t idleStartAccum = 0;
    uint32_t lastUnderruns = 0;
#if NES_PROFILE
    uint32_t cpuUs = 0, apuUs = 0, ppuUs = 0;
#endif
} st;

#if NES_PROFILE
  #define PROF_START(v)   uint32_t v = time_us_32()
  #define PROF_ADD(a, v)  a += time_us_32() - (v)
#else
  #define PROF_START(v)
  #define PROF_ADD(a, v)
#endif

// Attende che l'I2S svuoti il ring. Se in 50ms non scorre nulla l'audio e'
// considerato morto: si passa al pacing a tempo (vedi runFrame).
static inline void PINES_RAM paceAudio() {
    if (!paceEnabled) return;
    if (audioOut.ringUsed() <= AUDIO_TARGET_FILL) return;
    uint32_t t0 = time_us_32();
    while (audioOut.ringUsed() > AUDIO_TARGET_FILL) {
        if (time_us_32() - t0 > 50000) {
            paceEnabled = false;
            Serial.println(F("[PiNES] audio non scorre: pacing a tempo"));
            break;
        }
    }
    idleAccumUs += time_us_32() - t0;
}

static void PINES_RAM frameStart() {
    // --- frameskip: mostra 1 frame ogni (skipLevel+1) ---
    bool want;
    if (skipLevel == 0) want = true;
    else if (++skipCtr > skipLevel) { skipCtr = 0; want = true; }
    else want = false;

    bool drawn = ppu.startFrame(want);
    st.frames++;
    if (drawn) st.shown++;
    else if (want) st.droppedBusy++;            // voluto ma Core1 era occupato: saltato SENZA attese

#if ADAPTIVE_SKIP
    if ((++frameNo % ADAPT_WINDOW_FRAMES) == 0) {
        uint32_t under = audioOut.underruns();
        uint32_t idleAvg = (idleAccumUs - st.idleStartAccum) / ADAPT_WINDOW_FRAMES;
        if (under != st.lastUnderruns) {                       // non ce la facciamo: meno frame mostrati
            if (skipLevel < SKIP_MAX) skipLevel++;
        } else if (paceEnabled && idleAvg > ADAPT_IDLE_UP_US) { // tempo libero: piu' frame mostrati
            if (skipLevel > SKIP_MIN) skipLevel--;
        }
        st.lastUnderruns = under;
        st.idleStartAccum = idleAccumUs;
    }
#endif
}

#if NES_STATS
static void statsPrint() {
    uint32_t now = time_us_32();
    uint32_t win = now - st.windowStartUs;
    float s = win / 1e6f;
    Serial.printf("[PiNES] emu %.1f fps | mostrati %.1f fps | skip=%u busyDrop=%lu | idle %.0f%% | ring %d | underrun %lu overrun %lu\n",
                  st.frames / s, st.shown / s, skipLevel, (unsigned long)st.droppedBusy,
                  100.0f * idleAccumUs / win, audioOut.ringUsed(),
                  (unsigned long)audioOut.underruns(), (unsigned long)audioOut.overruns());
#if NES_PROFILE
    Serial.printf("        cpu %.1f%%  apu %.1f%%  ppu %.1f%%\n",
                  100.0f * st.cpuUs / win, 100.0f * st.apuUs / win, 100.0f * st.ppuUs / win);
    st.cpuUs = st.apuUs = st.ppuUs = 0;
#endif
    st.frames = st.shown = st.droppedBusy = 0;
    idleAccumUs = 0; st.idleStartAccum = 0;
    st.windowStartUs = now;
}
#endif

// ============================================================================
// CORE 0: un intero frame NES (262 scanline) per chiamata di loop()
// (evita l'overhead del wrapper Arduino loop()/__loop() a ogni scanline)
// ============================================================================
static void PINES_RAM runFrame() {
    static int frac = 0;           // 113.667 cicli/riga = 113,114,114 ripetuto
    static int cycleDebt = 0;      // cicli di troppo/di meno eseguiti nella riga precedente
    const uint32_t tFrame0 = time_us_32();

    frameStart();

    for (int line = 0; line < 262; line++) {
        paceAudio();

        frac += 2;
        int cycles = 113;
        if (frac >= 3) { frac -= 3; cycles = 114; }

        // L'ultima istruzione sfora sempre il budget (e OAM DMA ne ruba 513):
        // si contano i cicli REALI da getTotalCycles() (include NMI/IRQ/DMA) e
        // si compensa sulla riga dopo => CPU, APU e PPU restano agganciate.
        int target = cycles + cycleDebt;
        PROF_START(t0);
        int64_t c0 = nesCPU.getTotalCycles();
        nesCPU.execCPU(target);
        int executed = (int)(nesCPU.getTotalCycles() - c0);
        cycleDebt = target - executed;
        PROF_ADD(st.cpuUs, t0);

        PROF_START(t1);
        apu.clock(executed, audioPushSampleWrapper);
        PROF_ADD(st.apuUs, t1);

        PROF_START(t2);
        bool nmi = ppu.stepScanline();
        PROF_ADD(st.ppuUs, t2);
        if (nmi) nesCPU.requestNMI();

        if (line < NesPPU::HEIGHT && nesLoader.notifyScanlineIRQ()) nesCPU.requestIRQ(true);
        if (!nesLoader.irqStillPending()) nesCPU.requestIRQ(false);
    }

    // Senza audio funzionante: limita a ~60.1 fps a tempo.
    if (!paceEnabled) {
        while (time_us_32() - tFrame0 < 16639) { /* spin */ }
    }

#if NES_STATS
    if (st.frames >= 120) statsPrint();
#endif
}

// ============================================================================
// UI — transizioni di stato (pattern Pi64: menu -> browser -> emulazione)
// ============================================================================

// Riporta il TFT in uno stato noto al cambio modalita' grafica (browser ->
// emulatore). Versione ridotta della cleanupTFT() di Pi64: il browser disegna
// in blocking (nessun pushPixelsDMA), quindi basta attendere eventuali
// trasferimenti pendenti; niente abort dei canali DMA (restano validi per
// l'emulazione).
static void cleanupTFT() {
    Serial.println(F("[PiNES] Cleanup TFT..."));
    tft.endWrite();
    uint32_t t0 = millis();
    while (tft.dmaBusy() && (millis() - t0 < 200)) delay(1);
    tft.fillScreen(TFT_BLACK);
    delay(50);                       // aspetta che fillScreen finisca
    tft.setTextDatum(TL_DATUM);
    Serial.println(F("[PiNES] TFT pulito"));
}

// STATE_MENU
void handleMenu() {
    NESMenuAction action = menu->update();
    switch (action) {
        case NES_ACTION_START_GAME:
            Serial.println(F("[PiNES] Browser giochi in avvio..."));
            currentState = STATE_BROWSER;
            break;
        case NES_ACTION_NONE:
        default:
            // Nessuna azione
            break;
    }
    delay(10);        // polling UI a 100Hz (come handleGameBrowser su Pi64)
}

// Setup dell'emulazione: e' il codice che in v7 stava in setup() dopo
// l'(ora rimossa) loadROM() hardcoded. Chiamato alla conferma della ROM.
void startEmulation(const char* romPath) {
    cleanupTFT();

    if (!nesLoader.loadROM(romPath)) {
        Serial.println(F("[PiNES] Errore caricamento ROM!"));
        tft.fillScreen(TFT_BLACK);
        tft.setTextColor(TFT_RED, TFT_BLACK);
        tft.setTextSize(2);
        tft.setTextDatum(MC_DATUM);
        tft.drawString("LOAD ERROR!", 160, 120);
        delay(3000);
        currentState = STATE_MENU;
        menu->draw();
        return;
    }

    ppu.begin(nesLoader.chrBase(), nesChrWrite, nesCpuPageRead, nesMirrorModeFn);
    apu.begin(NES_CPU_CLOCK_NTSC, NES_AUDIO_SAMPLE_RATE);
    nesCPU.begin(nesMemRead, nesMemWrite, nesIoRead, nesIoWrite);
    nesCPU.setFastMemory(nesLoader.prgWindow(), nesLoader.prgRamPtr());   // PRG/PRG-RAM senza chiamate
    nesCPU.resetCPU();

    // Da qui in poi serve solo l'emulatore: il menu non tornera' mai
    // (l'uscita dal gioco e' il reboot, vedi checkExitCombo).
    delete menu;
    menu = nullptr;

    st.windowStartUs = time_us_32();
    currentState = STATE_EMULATION;

    PINES_BARRIER();
    emuReady = true;                  // sblocca setup1(): I2S + IRQ DMA su Core1
}

// STATE_BROWSER (pattern handleGameBrowser di Pi64)
void handleBrowser() {
    // Crea browser solo una volta
    if (!gameBrowser) {
        // USA LOADER per init filesystem (SD prima, LittleFS come fallback):
        // initFilesystem e' idempotente, quindi e' sicuro anche al ritorno
        // dal menu dopo un BACK (il filesystem resta aperto).
        if (!nesLoader.initFilesystem(FSType::FS_SD)) {
            Serial.println(F("[PiNES] Nessun filesystem disponibile!"));
            tft.fillScreen(TFT_RED);
            tft.setTextColor(TFT_WHITE, TFT_RED);
            tft.setTextSize(2);
            tft.setTextDatum(MC_DATUM);
            tft.drawString("NO FILESYSTEM!", 160, 120);
            delay(3000);
            currentState = STATE_MENU;
            menu->draw();
            return;
        }

        gameBrowser = new NESGameBrowser(&tft, &nesLoader);
        gameBrowser->setLogo(logoPiNES_small, PINES_LOGO_SMALL_W, PINES_LOGO_SMALL_H);
        gameBrowser->setPath("/games");
        gameBrowser->setFileExt(".nes");

        if (!gameBrowser->begin(nesLoader.getCurrentFilesystem())) {
            Serial.println(F("[PiNES] Init browser fallita!"));
            delay(1500);               // lascia leggere la schermata d'errore del browser
            delete gameBrowser;
            gameBrowser = nullptr;
            currentState = STATE_MENU;
            menu->draw();
            return;
        }

        // Diversamente da Pi64 (che qui liberava il menu), il menu resta
        // vivo: serve al BACK dal browser verso il menu. Costa ~60 byte di
        // heap e viene liberato in startEmulation().
        Serial.printf("[PiNES] Browser attivo (%s)\n",
                      nesLoader.getCurrentFilesystem() == FSType::FS_SD ? "SD" : "LittleFS");
    }

    // Navigazione con il pad della UI (bit attivi-alti di NesJoystick)
    NESBrowserEvent ev = gameBrowser->update(uiJoy->read());

    if (ev == NESBrowserEvent::SELECTED) {
        Serial.println(F("[PiNES] ROM selezionata!"));
        // Il browser salva il path nel loader e lo ritorna
        String romPath = gameBrowser->getRomSelected();

        // Libera il browser PRIMA del loadROM: la lista ordinata e' heap
        // (~21KB nel caso peggiore) che la cache-ROM di NESLoader puo'
        // volere indietro per la malloc del full-cache.
        delete gameBrowser;
        gameBrowser = nullptr;

        startEmulation(romPath.c_str());
        return;
    }

    if (ev == NESBrowserEvent::BACK) {
        Serial.println(F("[PiNES] Ritorno al menu"));
        delete gameBrowser;
        gameBrowser = nullptr;
        currentState = STATE_MENU;
        menu->draw();
        return;
    }

    delay(10);        // polling UI a 100Hz (come handleGameBrowser su Pi64)
}

// Combo di uscita dal gioco (vedi EXIT_COMBO_*): un check per frame.
static void checkExitCombo() {
    static uint16_t heldFrames = 0;
    uint8_t state = joy1 ? joy1->read() : 0;
    if ((state & EXIT_COMBO_MASK) == EXIT_COMBO_MASK) {
        if (++heldFrames >= EXIT_COMBO_FRAMES) {
            Serial.println(F("[PiNES] Combo SELECT+START+A+B: riavvio..."));
            delay(100);                // lascia uscire la riga di log
            rp2040.restart();          // reboot pulito -> si riparte dal menu
        }
    } else {
        heldFrames = 0;
    }
}

// STATE_EMULATION
void handleEmulation() {
    checkExitCombo();
    runFrame();
}

// ============================================================================
// SETUP (Core0): clock, periferiche e MENU (l'emulazione parte solo dopo la
// selezione della ROM nel browser — vedi startEmulation)
// ============================================================================
void setup() {
    vreg_set_voltage(VREG_VOLTAGE_1_20);
    set_sys_clock_khz(276000, true);   // overclock — verifica stabilita' sul tuo esemplare
    clock_configure(
      clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
      276000000, 276000000);

    Serial.begin(115200);

    initNesJoysticks(&joy1, &joy2);

    tft.init();
    tft.setRotation(1);
    tft.fillScreen(TFT_BLACK);
    tft.initDMA();
    tft.setSwapBytes(false);           // la palette e' gia' byte-swappata

    // Joystick per la UI: porta 1 (convenzione NES), fallback porta 2
    uiJoy = joy1 ? joy1 : joy2;
    if (uiJoy == nullptr) {
        Serial.println(F("[PiNES] ERRORE: nessun joystick per la UI"));
        tft.setTextColor(TFT_RED, TFT_BLACK);
        tft.setTextSize(2);
        tft.setTextDatum(MC_DATUM);
        tft.drawString("JOYSTICK ERROR!", 160, 120);
        while (true) { delay(1000); }  // come Pi64: senza pad la UI e' inutilizzabile
    }

    for (int i = 0; i < 64; i++) palSwapped[i] = __builtin_bswap16(NES_PALETTE_RGB565[i]);

    audioOut.reset();                  // l'I2S parte in setup1() (IRQ su Core1)

    // ── UI di selezione ROM (pattern Pi64) ─────────────────────
    menu = new NESMenu(&tft, uiJoy);
    menu->setLogo(logoPiNES, PINES_LOGO_W, PINES_LOGO_H);
    menu->begin();

    st.windowStartUs = time_us_32();
    Serial.println(F("[PiNES] setup() completato"));
    // NOTA: emuReady resta false finché non parte l'emulazione: Core1 resta
    // in attesa in setup1() mentre Core0 gestisce menu e browser.
}

// ============================================================================
// LOOP (Core0): macchina a stati della UI
// ============================================================================
void loop() {
    switch (currentState) {
        case STATE_MENU:
            handleMenu();
            break;
        case STATE_BROWSER:
            handleBrowser();
            break;
        case STATE_EMULATION:
            handleEmulation();
            break;
    }
}

void setFrameSkip(uint8_t n) { skipLevel = n; }

// ============================================================================
// CORE 1: I2S (IRQ) + display
// ============================================================================
void setup1() {
    // Core1 parte PRIMA che setup() giri (vedi main.cpp di arduino-pico): si
    // aspetta che Core0 abbia fissato il clock di sistema (il divisore PIO
    // dell'I2S dipende da clk_sys), inizializzato il TFT e avviato l'emulazione
    // tramite il browser (startEmulation -> emuReady).
    while (!emuReady) { tight_loop_contents(); }
    PINES_BARRIER();
    audioOut.begin(AUDIO_BCLK_GPIO, AUDIO_DIN_GPIO);   // la IRQ DMA si registra su QUESTO core
}

// idx (8 bit, 4 pixel per parola) -> RGB565 swappato (2 pixel per parola)
static void PINES_RAM convertIdxToRgb(const uint8_t* src, uint32_t* dst, int nPixels) {
    const uint32_t* s = (const uint32_t*)src;
    const uint16_t* pal = palSwapped;
    for (int i = 0; i < nPixels / 4; i++) {
        uint32_t w = s[i];
        dst[2 * i]     = pal[w & 0x3F]         | ((uint32_t)pal[(w >> 8)  & 0x3F] << 16);
        dst[2 * i + 1] = pal[(w >> 16) & 0x3F] | ((uint32_t)pal[(w >> 24) & 0x3F] << 16);
    }
}

static uint16_t rgbBuf[2][PUSH_LINES * NesPPU::WIDTH] __attribute__((aligned(4)));

void PINES_RAM loop1() {
    static bool     active = false;
    static uint8_t  sel = 0;
    static uint16_t nextLine = 0;

    if (!active) {
        if (!ppu.frameActive) { busy_wait_us_32(100); return; }   // niente da fare (non martello il bus)
        PINES_BARRIER();
        tft.startWrite();
        tft.setAddrWindow(DISPLAY_X_OFFSET, 0, NesPPU::WIDTH, NesPPU::HEIGHT);
        active = true;
        nextLine = 0;
    }

    uint16_t done = ppu.linesDone;
    PINES_BARRIER();                                   // leggi i pixel solo DOPO aver letto il contatore
    if (done < nextLine + PUSH_LINES) { busy_wait_us_32(20); return; }   // blocco non ancora completo

    convertIdxToRgb(&ppu.fb[nextLine][0], (uint32_t*)rgbBuf[sel], PUSH_LINES * NesPPU::WIDTH);

    while (tft.dmaBusy()) { /* il blocco precedente sta ancora uscendo */ }
    tft.pushPixelsDMA(rgbBuf[sel], PUSH_LINES * NesPPU::WIDTH);   // non bloccante

    sel ^= 1;
    nextLine += PUSH_LINES;

    if (nextLine >= NesPPU::HEIGHT) {
        while (tft.dmaBusy()) { }
        tft.endWrite();
        active = false;
        PINES_BARRIER();
        ppu.frameActive = false;                       // Core0 puo' avviare un nuovo frame disegnato
    }
}
