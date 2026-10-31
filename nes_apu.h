/*
 * Questo file fa parte del progetto PiNES. Copyright (C) 2026 Ivan Vettori.
 * GPLv3.
 *
 * APU NES — v2 "a blocchi" per RP2040
 *
 * Implementati: Pulse 1/2 (sweep, envelope), Triangle (linear counter),
 * Noise (envelope, LFSR 15 bit), length counter, frame sequencer 4-step.
 * NON implementati: DMC, frame IRQ, 5-step reale (trattato come 4-step ma
 * con il clock immediato di half+quarter frame alla scrittura di $4017).
 *
 * Differenze rispetto alla v1:
 *  - clock() non gira piu' ciclo-per-ciclo (113 iterazioni a scanline):
 *    avanza a BLOCCHI fino al prossimo evento (campione di uscita oppure
 *    gradino del frame sequencer). I timer dei canali vengono avanzati in
 *    forma chiusa (advance(n)), non con n decrementi. Costo ~ 30x minore.
 *  - Mixer NON lineare esatto (formule del 2A03) precalcolato in tabella.
 *  - Uscita int16 con segno, con filtro passa-alto (~20Hz) che elimina
 *    l'offset DC: serve per l'I2S (la NES e' unipolare, l'I2S e' con segno).
 */
#ifndef NES_APU_H
#define NES_APU_H
#include <stdint.h>
#include "pines_ram.h"

// Tabelle hardware, definite in nes_apu.cpp NON const => vivono in RAM
// (evitano accessi alla flash/cache XIP dal loop caldo).
extern uint8_t g_apuDuty[4][8];
extern uint8_t g_apuTriSeq[32];

class NesAPU {
public:
    // Callback: campione PCM int16 con segno (centrato su 0).
    typedef void (*PushSampleFn)(int16_t sample);

    void begin(uint32_t cpuClockHz = 1789773, uint32_t sampleRateHz = 32000);
    void reset();

    void regWrite(uint16_t addr, uint8_t val);  // $4000-$4013, $4015, $4017
    uint8_t regRead(uint16_t addr);             // $4015

    // Avanza di cpuCycles cicli CPU, emettendo i campioni maturati.
    void clock(int cpuCycles, PushSampleFn pushSample);

private:
    uint32_t cpuClock = 1789773, sampleRate = 32000;
    uint32_t cyclesPerSampleFixed = 0;   // cicli CPU per campione, *256
    uint32_t sampleAccum = 0;
    bool     cycleParity = false;        // divide per 2 il clock di pulse/noise
    uint32_t frameSeqCounter = 0;
    uint8_t  frameStep = 0;              // 0..3
    uint32_t nextFrameEvent = 7457;

    int32_t hpPrevX = 0, hpPrevY = 0;    // filtro passa-alto DC

    static int16_t pulseTable[31];
    static int16_t tndTable[203];

    struct Pulse {
        bool enabled = false;
        bool lengthHalt = false, constantVolume = false, envelopeStart = false;
        bool sweepEnabled = false, sweepNegate = false, sweepReload = false;
        bool isChannel2 = false;
        uint8_t duty = 0, seqPos = 0, volumeOrEnvPeriod = 0;
        uint8_t envelopeCounter = 0, envelopeDecay = 0, lengthCounter = 0;
        uint8_t sweepPeriod = 0, sweepShift = 0, sweepCounter = 0;
        uint16_t timerPeriod = 0;
        int16_t  timerCounter = 0;       // invariante: sempre >= 0

        inline uint8_t output() const {
            if (lengthCounter == 0) return 0;
            if (timerPeriod < 8 || timerPeriod > 0x7FF) return 0;
            if (!g_apuDuty[duty][seqPos]) return 0;
            return constantVolume ? volumeOrEnvPeriod : envelopeDecay;
        }
        // Equivalente a 'ticks' ripetizioni di:
        //   if (--timerCounter < 0) { timerCounter = timerPeriod; seqPos++; }
        inline void advance(uint32_t ticks) {
            uint32_t first = (uint32_t)timerCounter + 1;
            if (ticks < first) { timerCounter -= (int16_t)ticks; return; }
            ticks -= first;
            uint32_t p1 = (uint32_t)timerPeriod + 1, steps = 1;
            if (ticks >= p1) { steps += ticks / p1; ticks %= p1; }
            timerCounter = (int16_t)(timerPeriod - ticks);
            seqPos = (seqPos + steps) & 7;
        }
        inline void tickEnvelope() {
            if (envelopeStart) {
                envelopeStart = false; envelopeDecay = 15; envelopeCounter = volumeOrEnvPeriod;
            } else if (envelopeCounter > 0) {
                envelopeCounter--;
            } else {
                envelopeCounter = volumeOrEnvPeriod;
                if (envelopeDecay > 0) envelopeDecay--;
                else if (lengthHalt) envelopeDecay = 15;
            }
        }
        inline void tickSweep() {
            if (sweepReload) { sweepCounter = sweepPeriod; sweepReload = false; return; }
            if (sweepCounter > 0) { sweepCounter--; return; }
            sweepCounter = sweepPeriod;
            if (!sweepEnabled || sweepShift == 0) return;
            int16_t delta = timerPeriod >> sweepShift;
            int32_t np = timerPeriod + (sweepNegate ? -delta - (isChannel2 ? 0 : 1) : delta);
            if (np >= 8 && np <= 0x7FF) timerPeriod = (uint16_t)np;
        }
        inline void tickLength() { if (!lengthHalt && lengthCounter > 0) lengthCounter--; }
    } pulse1, pulse2;

    struct Triangle {
        bool enabled = false, lengthHalt = false, linearReload = false;
        uint8_t seqPos = 0, linearPeriod = 0, linearCounter = 0, lengthCounter = 0;
        uint16_t timerPeriod = 0;
        int16_t  timerCounter = 0;

        inline uint8_t output() const {
            if (lengthCounter == 0 || linearCounter == 0) return 0;
            return g_apuTriSeq[seqPos];
        }
        // 'ticks' = cicli CPU (il triangle gira a clock CPU pieno).
        // lengthCounter/linearCounter cambiano solo sui confini di blocco,
        // quindi il gate e' costante dentro advance().
        inline void advance(uint32_t ticks) {
            uint32_t first = (uint32_t)timerCounter + 1;
            if (ticks < first) { timerCounter -= (int16_t)ticks; return; }
            ticks -= first;
            uint32_t p1 = (uint32_t)timerPeriod + 1, steps = 1;
            if (ticks >= p1) { steps += ticks / p1; ticks %= p1; }
            timerCounter = (int16_t)(timerPeriod - ticks);
            if (lengthCounter > 0 && linearCounter > 0) seqPos = (seqPos + steps) & 31;
        }
        inline void tickLinear() {
            if (linearReload) linearCounter = linearPeriod;
            else if (linearCounter > 0) linearCounter--;
            if (!lengthHalt) linearReload = false;
        }
        inline void tickLength() { if (!lengthHalt && lengthCounter > 0) lengthCounter--; }
    } triangle;

    struct Noise {
        bool enabled = false, modeShort = false, lengthHalt = false;
        bool constantVolume = false, envelopeStart = false;
        uint8_t volumeOrEnvPeriod = 0, envelopeCounter = 0, envelopeDecay = 0, lengthCounter = 0;
        uint16_t timerPeriod = 0;
        int16_t  timerCounter = 0;
        uint16_t shiftReg = 1;

        inline uint8_t output() const {
            if (lengthCounter == 0) return 0;
            if (shiftReg & 1) return 0;
            return constantVolume ? volumeOrEnvPeriod : envelopeDecay;
        }
        inline void advance(uint32_t ticks) {
            uint32_t first = (uint32_t)timerCounter + 1;
            if (ticks < first) { timerCounter -= (int16_t)ticks; return; }
            ticks -= first;
            uint32_t p1 = (uint32_t)timerPeriod + 1, steps = 1;
            if (ticks >= p1) { steps += ticks / p1; ticks %= p1; }
            timerCounter = (int16_t)(timerPeriod - ticks);
            // l'LFSR va avanzato davvero 'steps' volte (con period>=4: max pochi passi per blocco)
            uint16_t sr = shiftReg;
            const int tap = modeShort ? 6 : 1;
            while (steps--) {
                uint16_t fb = (sr & 1) ^ ((sr >> tap) & 1);
                sr = (sr >> 1) | (fb << 14);
            }
            shiftReg = sr;
        }
        inline void tickEnvelope() {
            if (envelopeStart) {
                envelopeStart = false; envelopeDecay = 15; envelopeCounter = volumeOrEnvPeriod;
            } else if (envelopeCounter > 0) {
                envelopeCounter--;
            } else {
                envelopeCounter = volumeOrEnvPeriod;
                if (envelopeDecay > 0) envelopeDecay--;
                else if (lengthHalt) envelopeDecay = 15;
            }
        }
        inline void tickLength() { if (!lengthHalt && lengthCounter > 0) lengthCounter--; }
    } noise;

    void quarterFrameTick();
    void halfFrameTick();
    inline int16_t mixHP();
};

#endif