/*
 * Questo file fa parte del progetto PiNES. Copyright (C) 2026 Ivan Vettori.
 * GPLv3. Vedi nes_apu.h.
 */
#include "nes_apu.h"
#include <string.h>

// Tabelle hardware in RAM (non const, vedi nes_apu.h)
uint8_t g_apuDuty[4][8] = {
    {0,1,0,0,0,0,0,0},
    {0,1,1,0,0,0,0,0},
    {0,1,1,1,1,0,0,0},
    {1,0,0,1,1,1,1,1}
};
uint8_t g_apuTriSeq[32] = {
    15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0,
    0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
};
static uint8_t s_lengthTable[32] = {
    10,254,20,2,40,4,80,6,160,8,60,10,14,12,26,14,
    12,16,24,18,48,20,96,22,192,24,72,26,16,28,32,30
};
static uint16_t s_noisePeriod[16] = {
    4,8,16,32,64,96,128,160,202,254,380,508,762,1016,2034,4068
};
static uint32_t s_frameEvents[4] = { 7457, 14913, 22371, 29829 };

int16_t NesAPU::pulseTable[31];
int16_t NesAPU::tndTable[203];

void NesAPU::begin(uint32_t cpuClockHz, uint32_t sampleRateHz) {
    cpuClock = cpuClockHz;
    sampleRate = sampleRateHz;
    cyclesPerSampleFixed = (uint32_t)(((uint64_t)cpuClock << 8) / sampleRate);

    // Mixer non lineare esatto del 2A03 (wiki nesdev), a 16 bit:
    //   pulse_out = 95.52  / (8128/(p1+p2) + 100)
    //   tnd_out   = 163.67 / (24329/(3*t + 2*n + dmc) + 100)
    // SCALE: il massimo teorico somma ~1.0; 24000 lascia margine per il HPF.
    const float SCALE = 24000.0f;
    pulseTable[0] = 0;
    for (int i = 1; i < 31; i++)
        pulseTable[i] = (int16_t)(95.52f / (8128.0f / i + 100.0f) * SCALE + 0.5f);
    tndTable[0] = 0;
    for (int i = 1; i < 203; i++)
        tndTable[i] = (int16_t)(163.67f / (24329.0f / i + 100.0f) * SCALE + 0.5f);

    reset();
}

void NesAPU::reset() {
    pulse1 = Pulse(); pulse2 = Pulse();
    pulse2.isChannel2 = true;
    triangle = Triangle();
    noise = Noise();
    sampleAccum = 0;
    cycleParity = false;
    frameSeqCounter = 0;
    frameStep = 0;
    nextFrameEvent = s_frameEvents[0];
    hpPrevX = hpPrevY = 0;
}

// ============================================================================
// Registri
// ============================================================================
void PINES_RAM NesAPU::regWrite(uint16_t addr, uint8_t val) {
    switch (addr) {
        case 0x4000:
            pulse1.duty = (val >> 6) & 3;
            pulse1.lengthHalt = val & 0x20;
            pulse1.constantVolume = val & 0x10;
            pulse1.volumeOrEnvPeriod = val & 0x0F;
            break;
        case 0x4001:
            pulse1.sweepEnabled = val & 0x80;
            pulse1.sweepPeriod = (val >> 4) & 7;
            pulse1.sweepNegate = val & 0x08;
            pulse1.sweepShift = val & 7;
            pulse1.sweepReload = true;
            break;
        case 0x4002:
            pulse1.timerPeriod = (pulse1.timerPeriod & 0x0700) | val;
            break;
        case 0x4003:
            pulse1.timerPeriod = (pulse1.timerPeriod & 0x00FF) | ((uint16_t)(val & 7) << 8);
            if (pulse1.enabled) pulse1.lengthCounter = s_lengthTable[(val >> 3) & 0x1F];
            pulse1.envelopeStart = true;
            pulse1.seqPos = 0;
            break;

        case 0x4004:
            pulse2.duty = (val >> 6) & 3;
            pulse2.lengthHalt = val & 0x20;
            pulse2.constantVolume = val & 0x10;
            pulse2.volumeOrEnvPeriod = val & 0x0F;
            break;
        case 0x4005:
            pulse2.sweepEnabled = val & 0x80;
            pulse2.sweepPeriod = (val >> 4) & 7;
            pulse2.sweepNegate = val & 0x08;
            pulse2.sweepShift = val & 7;
            pulse2.sweepReload = true;
            break;
        case 0x4006:
            pulse2.timerPeriod = (pulse2.timerPeriod & 0x0700) | val;
            break;
        case 0x4007:
            pulse2.timerPeriod = (pulse2.timerPeriod & 0x00FF) | ((uint16_t)(val & 7) << 8);
            if (pulse2.enabled) pulse2.lengthCounter = s_lengthTable[(val >> 3) & 0x1F];
            pulse2.envelopeStart = true;
            pulse2.seqPos = 0;
            break;

        case 0x4008:
            triangle.lengthHalt = val & 0x80;
            triangle.linearPeriod = val & 0x7F;
            break;
        case 0x400A:
            triangle.timerPeriod = (triangle.timerPeriod & 0x0700) | val;
            break;
        case 0x400B:
            triangle.timerPeriod = (triangle.timerPeriod & 0x00FF) | ((uint16_t)(val & 7) << 8);
            if (triangle.enabled) triangle.lengthCounter = s_lengthTable[(val >> 3) & 0x1F];
            triangle.linearReload = true;
            break;

        case 0x400C:
            noise.lengthHalt = val & 0x20;
            noise.constantVolume = val & 0x10;
            noise.volumeOrEnvPeriod = val & 0x0F;
            break;
        case 0x400E:
            noise.modeShort = val & 0x80;
            noise.timerPeriod = s_noisePeriod[val & 0x0F];
            break;
        case 0x400F:
            if (noise.enabled) noise.lengthCounter = s_lengthTable[(val >> 3) & 0x1F];
            noise.envelopeStart = true;
            break;

        case 0x4015:
            pulse1.enabled = val & 0x01;
            pulse2.enabled = val & 0x02;
            triangle.enabled = val & 0x04;
            noise.enabled = val & 0x08;
            if (!pulse1.enabled) pulse1.lengthCounter = 0;
            if (!pulse2.enabled) pulse2.lengthCounter = 0;
            if (!triangle.enabled) triangle.lengthCounter = 0;
            if (!noise.enabled) noise.lengthCounter = 0;
            break;

        case 0x4017:
            frameSeqCounter = 0;
            frameStep = 0;
            nextFrameEvent = s_frameEvents[0];
            if (val & 0x80) { quarterFrameTick(); halfFrameTick(); }  // modo 5-step: clock immediato
            break;

        default: break;
    }
}

uint8_t PINES_RAM NesAPU::regRead(uint16_t addr) {
    if (addr == 0x4015) {
        uint8_t r = 0;
        if (pulse1.lengthCounter > 0)   r |= 0x01;
        if (pulse2.lengthCounter > 0)   r |= 0x02;
        if (triangle.lengthCounter > 0) r |= 0x04;
        if (noise.lengthCounter > 0)    r |= 0x08;
        return r;
    }
    return 0;
}

void PINES_RAM NesAPU::quarterFrameTick() {
    pulse1.tickEnvelope(); pulse2.tickEnvelope();
    triangle.tickLinear();
    noise.tickEnvelope();
}
void PINES_RAM NesAPU::halfFrameTick() {
    pulse1.tickLength(); pulse1.tickSweep();
    pulse2.tickLength(); pulse2.tickSweep();
    triangle.tickLength();
    noise.tickLength();
}

// ============================================================================
// Mixer + passa-alto DC (~20Hz @32kHz): y = x - x[-1] + 0.996*y[-1]
// ============================================================================
inline int16_t NesAPU::mixHP() {
    int32_t x = pulseTable[pulse1.output() + pulse2.output()]
              + tndTable[3 * triangle.output() + 2 * noise.output()];
    int32_t y = x - hpPrevX + ((hpPrevY * 32636) >> 15);
    hpPrevX = x;
    hpPrevY = y;
    if (y >  32767) y =  32767;
    if (y < -32768) y = -32768;
    return (int16_t)y;
}

// ============================================================================
// Clock a blocchi
// ============================================================================
void PINES_RAM NesAPU::clock(int cpuCycles, PushSampleFn pushSample) {
    while (cpuCycles > 0) {
        // cicli CPU al prossimo campione (>= 1, perche' sampleAccum < cyclesPerSampleFixed)
        uint32_t toSample = (cyclesPerSampleFixed - sampleAccum + 255) >> 8;
        uint32_t toFrame  = nextFrameEvent - frameSeqCounter;       // >= 1
        uint32_t n = (uint32_t)cpuCycles;
        if (n > toSample) n = toSample;
        if (n > toFrame)  n = toFrame;

        // Pulse/Noise: un tick ogni 2 cicli CPU. Conteggio esatto dei fronti
        // di cycleParity che cadono in questo blocco.
        uint32_t apuTicks = cycleParity ? (n >> 1) : ((n + 1) >> 1);
        cycleParity ^= (n & 1);

        if (triangle.enabled) triangle.advance(n);
        if (apuTicks) {
            if (pulse1.enabled) pulse1.advance(apuTicks);
            if (pulse2.enabled) pulse2.advance(apuTicks);
            if (noise.enabled)  noise.advance(apuTicks);
        }

        sampleAccum     += n << 8;
        frameSeqCounter += n;

        if (frameSeqCounter == nextFrameEvent) {
            switch (frameStep) {
                case 0: quarterFrameTick(); break;
                case 1: quarterFrameTick(); halfFrameTick(); break;
                case 2: quarterFrameTick(); break;
                default: quarterFrameTick(); halfFrameTick(); frameSeqCounter = 0; break;
            }
            frameStep = (frameStep + 1) & 3;
            nextFrameEvent = s_frameEvents[frameStep];
        }

        if (sampleAccum >= cyclesPerSampleFixed) {
            sampleAccum -= cyclesPerSampleFixed;
            if (pushSample) pushSample(mixHP());
        }
        cpuCycles -= (int)n;
    }
}