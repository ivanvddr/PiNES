/*
 * NesAudioOut.cpp — Copyright (C) 2026 Ivan Vettori — GPLv3
 * Vedi NesAudioOut.h.
 */
#include "NesAudioOut.h"

NesAudioOut* NesAudioOut::s_instance = nullptr;

bool NesAudioOut::begin(uint8_t bclkPin, uint8_t dataPin) {
    s_instance = this;
    _i2s.setBCLK(bclkPin);          // LRCLK = bclkPin + 1 (GP27)
    _i2s.setDATA(dataPin);
    _i2s.setBitsPerSample(16);
    _i2s.setBuffers(NES_I2S_BUFFERS, NES_I2S_BUFFER_WORDS, 0);
    _i2s.onTransmit(isrTrampoline);
    bool ok = _i2s.begin(NES_AUDIO_SAMPLE_RATE);
    Serial.printf("[NesAudioOut] I2S BCLK=GP%u LRCLK=GP%u DIN=GP%u %lu Hz -> %s\n",
                  bclkPin, bclkPin + 1, dataPin,
                  (unsigned long)NES_AUDIO_SAMPLE_RATE, ok ? "OK" : "ERRORE");
    return ok;
}

void PINES_RAM NesAudioOut::isrTrampoline() {
    if (s_instance) s_instance->onTransmit();
}

// IRQ DMA (Core1): riempie TUTTO lo spazio libero dei buffer I2S. Se il ring
// e' vuoto (underrun) ripete l'ultimo campione con un breve fade a zero:
// niente click e niente buchi di silenzio secco.
void PINES_RAM NesAudioOut::onTransmit() {
    if (!_started) return;                 // la libreria emette silenzio da sola

    int words = _i2s.availableForWrite() >> 2;   // byte -> parole da 32 bit (1 frame L+R)
    uint32_t tmp[64];

    while (words > 0) {
        int n = words > 64 ? 64 : words;
        uint16_t r = _readIdx;
        const uint16_t w = _writeIdx;
        int16_t last = _last;
        uint32_t under = 0;

        for (int i = 0; i < n; i++) {
            int16_t s;
            if (r != w) {
                s = _ring[r];
                r = (r + 1) & NES_AUDIO_RING_MASK;
                last = s;
            } else {
                last = (int16_t)(((int32_t)last * 240) >> 8);   // fade
                s = last;
                under++;
            }
            uint32_t u = (uint16_t)s;
            tmp[i] = (u << 16) | u;        // L = R
        }
        size_t wrote = _i2s.write((const uint8_t*)tmp, (size_t)n * 4) >> 2;   // non bloccante
        // chiesto solo quanto availableForWrite() dichiarava libero,
        // quindi wrote == n. Se per qualche motivo fosse minore perdiamo
        // pochi campioni: accettabile, e non rientriamo piu' nel ciclo.
        _readIdx = r;
        _last = last;
        _underruns += under;
        if (wrote < (size_t)n) break;
        words -= n;
    }
}