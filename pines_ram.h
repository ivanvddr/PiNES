#pragma once
/*
 * pines_ram.h — macro comuni PiNES (GPLv3, Ivan Vettori)
 *
 * PINES_RAM : mette la funzione in SRAM (sezione .time_critical, copiata da
 *             flash a RAM all'avvio). Sul RP2040 il codice in flash passa da
 *             una cache XIP da 16KB condivisa dai due core: un emulatore
 *             (CPU + PPU + APU) la fa "thrashare" di continuo. Il codice caldo
 *             in RAM esegue a 1 ciclo/istruzione e non disturba l'altro core.
 * PINES_BARRIER : barriera di memoria fra i due core (ARMv6-M "dmb").
 */
#if defined(ARDUINO_ARCH_RP2040) || defined(PICO_BUILD)
  #define PINES_RAM __attribute__((section(".time_critical.pines")))
  #define PINES_BARRIER() __asm volatile ("dmb" ::: "memory")
#else
  #define PINES_RAM
  #define PINES_BARRIER() __sync_synchronize()
#endif