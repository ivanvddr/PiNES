#pragma once
/*
 * NesAudioOut — uscita audio I2S (MAX98357A) per PiNES (RP2040)
 * Copyright (C) 2026 Ivan Vettori — GPLv3
 *
 * Pin (libreria I2S di arduino-pico: LRCLK = BCLK+1):
 *     BCLK = GP26,  LRCLK = GP27,  DIN = GP28
 *
 * Flusso:
 *   NesAPU::clock() --pushSample()--> ring SPSC (Core0, sincrono con la CPU
 *   emulata) --onTransmit() in IRQ DMA su Core1--> I2S (PIO+DMA).
 *
 * L'audio e' il MASTER CLOCK dell'emulazione: il loop di Core0 si ferma
 * (paceAudio() nel .ino) quando il ring e' sopra AUDIO_TARGET_FILL. Cosi' la
 * velocita' di gioco e' esattamente quella del DAC, senza deriva e senza
 * dipendere dalla velocita' del display.
 *
 * Formato campioni: int16 mono con segno (il MAX98357 con SD flottante fa
 * (L+R)/2, quindi duplico il campione su L e R).
 */
#include <Arduino.h>
#include <I2S.h>
#include <stdint.h>
#include "pines_ram.h"

static constexpr uint32_t NES_AUDIO_SAMPLE_RATE = 32000;

static constexpr int NES_AUDIO_RING_BITS    = 12;                    // 4096 campioni (~128ms)
static constexpr int NES_AUDIO_RING_SIZE    = 1 << NES_AUDIO_RING_BITS;
static constexpr int NES_AUDIO_RING_MASK    = NES_AUDIO_RING_SIZE - 1;
static constexpr int NES_AUDIO_RING_PREFILL = 384;                   // ~12ms prima di partire

// Buffer DMA della libreria I2S: 4 x 64 frame = 256 frame = 8ms di latenza fissa
static constexpr int NES_I2S_BUFFERS      = 4;
static constexpr int NES_I2S_BUFFER_WORDS = 64;

static constexpr uint16_t NES_AUDIO_GAIN_UNITY = 256;

class NesAudioOut {
public:
    NesAudioOut() : _i2s(OUTPUT) { reset(); }

    void reset() {
        _writeIdx = 0; _readIdx = 0;
        _underruns = 0; _overruns = 0;
        _started = false; _last = 0;
    }

    // DA CHIAMARE DA CORE1 (setup1), dopo che Core0 ha impostato il clock di
    // sistema: la IRQ DMA della libreria si registra sul core che chiama begin().
    bool begin(uint8_t bclkPin, uint8_t dataPin);

    // Core0 (produttore). Chiamata dal callback di NesAPU::clock().
    inline void pushSample(int16_t sample) {
        int32_t s = ((int32_t)sample * (int32_t)_gainQ8) >> 8;
        if (s >  32767) s =  32767;
        if (s < -32768) s = -32768;
        uint16_t w = _writeIdx;
        uint16_t next = (w + 1) & NES_AUDIO_RING_MASK;
        if (next == _readIdx) { _overruns++; return; }   // ring pieno: scarta
        _ring[w] = (int16_t)s;
        PINES_BARRIER();                                  // dato visibile prima dell'indice
        _writeIdx = next;
        if (!_started && ringUsed() >= NES_AUDIO_RING_PREFILL) _started = true;
    }

    void setGainQ8(uint16_t g) { _gainQ8 = g; }
    void setVolume(float v01) {
        if (v01 < 0.f) v01 = 0.f;
        if (v01 > 1.f) v01 = 1.f;
        _gainQ8 = (uint16_t)(v01 * v01 * (NES_AUDIO_GAIN_UNITY * 2) + 0.5f);
    }

    inline int ringUsed() const { return (_writeIdx - _readIdx) & NES_AUDIO_RING_MASK; }
    uint32_t underruns() const { return _underruns; }
    uint32_t overruns()  const { return _overruns; }
    bool     running()   const { return _started; }

private:
    I2S _i2s;
    int16_t _ring[NES_AUDIO_RING_SIZE];
    volatile uint16_t _writeIdx = 0, _readIdx = 0;
    volatile uint32_t _underruns = 0, _overruns = 0;
    volatile bool _started = false;
    int16_t _last = 0;
    uint16_t _gainQ8 = NES_AUDIO_GAIN_UNITY;

    static NesAudioOut* s_instance;
    static void isrTrampoline();
    void onTransmit();
};