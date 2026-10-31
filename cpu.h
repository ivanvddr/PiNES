/*
 * Questo file fa parte del progetto PiNES.
 *
 * Copyright (C) 2026 Ivan Vettori
 *
 * Questo file si ispira e trae spunti dal progetto Fake6502 CPU emulator
 * degli autori Mike Chambers e David MHS Webster, e dal core CPU
 * originariamente scritto per il progetto Pi64 (C64).
 *
 * Puoi ridistribuirlo e/o modificarlo secondo i termini della GPLv3.
 *
 * Questo programma è distribuito nella speranza che sia utile,
 * ma SENZA ALCUNA GARANZIA; senza neppure la garanzia implicita
 * di COMMERCIABILITÀ o IDONEITÀ PER UN PARTICOLARE SCOPO.
 * Vedi la licenza GPLv3 per maggiori dettagli.
 *
 * ============================================================================
 * NOTE DI PORTING (Pi64 -> PiNES)
 * ============================================================================
 * Rispetto alla versione C64, questa CPU è stata rimossa da ogni dipendenza
 * hardware-specifica (PLA, VIC2, CIA1/2, SID). L'accesso a memoria e IO è
 * ora completamente parametrico tramite function pointer passati a begin().
 *
 * Mappa di memoria assunta (tipica NES, gestita da QUESTA classe solo per la
 * RAM interna; tutto il resto è delegato al chiamante):
 *
 *   0x0000-0x07FF  RAM interna 2KB (gestita internamente, fast path)
 *   0x0800-0x1FFF  mirror della RAM interna (gestito internamente)
 *   0x2000-0x3FFF  registri PPU, mirrorati ogni 8 byte -> ioRead/ioWrite
 *   0x4000-0x401F  registri APU / IO (incl. joypad $4016/$4017,
 *                  OAM DMA $4014) -> ioRead/ioWrite
 *   0x4020-0xFFFF  spazio cartuccia/mapper (PRG-ROM, PRG-RAM, vettori
 *                  reset/NMI/IRQ in 0xFFFA-0xFFFF) -> memRead/memWrite
 *
 *
 * IMPORTANTE: il 2A03 (CPU della NES) NON ha modalità BCD funzionante.
 * Il flag DECIMAL esiste nel registro di stato (SED/CLD funzionano), ma
 * ADC/SBC eseguono SEMPRE aritmetica binaria. Questo è stato corretto in
 * cpu.cpp rispetto alla versione C64 (dove il BCD era necessario).
 * ============================================================================
 */

#ifndef CPU_H
#define CPU_H

#include <stdint.h>
#include "pines_ram.h"

#define RAMSIZE 0x0800   // 2KB RAM interna NES
#define FLAG_CARRY      0x01
#define FLAG_ZERO       0x02
#define FLAG_INTERRUPT  0x04
#define FLAG_DECIMAL    0x08   // presente ma inerte sul 2A03 (vedi nota sopra)
#define FLAG_BREAK      0x10
#define FLAG_CONSTANT   0x20
#define FLAG_OVERFLOW   0x40
#define FLAG_SIGN       0x80
#define BASE_STACK      0x100

// Soglie della mappa di memoria (modificabili se il tuo mapper differisce)
#define IO_START   0x2000   // inizio registri PPU
#define IO_END     0x401F   // fine registri APU/IO ($4000-$401F incluso)

class cpu {
public:
  // ==========================================================================
  // FUNCTION POINTER per memoria e IO, forniti dal main .ino
  // ==========================================================================
  typedef uint8_t (*MemReadFn)(uint16_t addr);
  typedef void    (*MemWriteFn)(uint16_t addr, uint8_t val);
  typedef uint8_t (*IOReadFn)(uint16_t addr);
  typedef void    (*IOWriteFn)(uint16_t addr, uint8_t val);

  cpu();

  void begin(MemReadFn memRead, MemWriteFn memWrite,
             IOReadFn ioRead, IOWriteFn ioWrite);

  int execCPU(int tickcount = 1);
  void resetCPU();
  uint16_t getpc();
  uint8_t getop();

  inline uint8_t mem_read(uint16_t addr) __attribute__((always_inline)) {
      return mem_read_fast(addr);
  }

  inline void mem_write(uint16_t addr, uint8_t val) __attribute__((always_inline)) {
      mem_write_fast(addr, val);
  }

  // RAM interna 2KB esposta per debug/savestate
  uint8_t memory[RAMSIZE];

  // Richiede un NMI/IRQ al PROSSIMO controllo del loop di esecuzione.
  // Thread-safe per uso da ISR (vedi cpu.cpp): usa solo flag volatili,
  // l'effettivo nmi()/irq() viene eseguito dentro execCPU().
  void requestNMI();
  void requestIRQ(bool level);  // level=true mentre la linea IRQ è asserita

  // accesso cicli totali eseguiti dall'ultimo resetCPU() (utile per audio sync)
  inline int64_t getTotalCycles() const { return totalCycles; }

  // Da chiamare quando un dispositivo esterno "ruba" cicli bus alla CPU
  // (es. OAM DMA $4014: 513/514 cicli; DMC DMA: 4 cicli). Implementata in
  // cpu.cpp perché deve toccare clockgoal6502, che è file-scope lì.
  void stealCycles(int n);

  // Lettura diretta di PRG ($8000-$FFFF) e PRG-RAM ($6000-$7FFF): evita una
  // chiamata a function pointer + divisione per ogni fetch/lettura.
  // DA CHIAMARE prima di resetCPU(). I puntatori devono restare validi.
  void setFastMemory(const uint8_t* prgWindow32k, const uint8_t* prgRam8k) {
      _prgWin = prgWindow32k; _prgRam = prgRam8k;
  }

private:
  MemReadFn  _memRead;
  MemWriteFn _memWrite;
  IOReadFn   _ioRead;
  IOWriteFn  _ioWrite;

  const uint8_t* _prgWin = nullptr;
  const uint8_t* _prgRam = nullptr;

  volatile bool _nmiPending;
  volatile bool _irqLine;

  int64_t totalCycles;

  // ========================================================================
  // Dispatch table con function pointer
  // ========================================================================
  typedef void (cpu::*OpcodeHandler)();
  static OpcodeHandler opcodeTable[256];
  static bool opcodeTableInitialized;
  static void initOpcodeTable();

  // Stack operations
  inline void push16(uint16_t pushval) __attribute__((always_inline));
  inline void push8(uint8_t pushval) __attribute__((always_inline));
  inline uint16_t pull16() __attribute__((always_inline));
  inline uint8_t pull8() __attribute__((always_inline));

  // CPU registers
  uint16_t pc;
  uint8_t sp, a, x, y, cpustatus;

  void nmi();
  void irq();

  // ========================================================================
  // ISTRUZIONI (identiche al set Pi64, con fix cicli/decimal — vedi cpu.cpp)
  // ========================================================================

  // LDA
  void op_lda_imm(); void op_lda_zp(); void op_lda_zpx(); void op_lda_abs();
  void op_lda_absx(); void op_lda_absy(); void op_lda_indx(); void op_lda_indy();
  // LDX
  void op_ldx_imm(); void op_ldx_zp(); void op_ldx_zpy(); void op_ldx_abs(); void op_ldx_absy();
  // LDY
  void op_ldy_imm(); void op_ldy_zp(); void op_ldy_zpx(); void op_ldy_abs(); void op_ldy_absx();
  // STA
  void op_sta_zp(); void op_sta_zpx(); void op_sta_abs(); void op_sta_absx();
  void op_sta_absy(); void op_sta_indx(); void op_sta_indy();
  // STX/STY
  void op_stx_zp(); void op_stx_zpy(); void op_stx_abs();
  void op_sty_zp(); void op_sty_zpx(); void op_sty_abs();
  // ADC
  void op_adc_imm(); void op_adc_zp(); void op_adc_zpx(); void op_adc_abs();
  void op_adc_absx(); void op_adc_absy(); void op_adc_indx(); void op_adc_indy();
  // SBC
  void op_sbc_imm(); void op_sbc_zp(); void op_sbc_zpx(); void op_sbc_abs();
  void op_sbc_absx(); void op_sbc_absy(); void op_sbc_indx(); void op_sbc_indy();
  // AND
  void op_and_imm(); void op_and_zp(); void op_and_zpx(); void op_and_abs();
  void op_and_absx(); void op_and_absy(); void op_and_indx(); void op_and_indy();
  // ORA
  void op_ora_imm(); void op_ora_zp(); void op_ora_zpx(); void op_ora_abs();
  void op_ora_absx(); void op_ora_absy(); void op_ora_indx(); void op_ora_indy();
  // EOR
  void op_eor_imm(); void op_eor_zp(); void op_eor_zpx(); void op_eor_abs();
  void op_eor_absx(); void op_eor_absy(); void op_eor_indx(); void op_eor_indy();
  // CMP
  void op_cmp_imm(); void op_cmp_zp(); void op_cmp_zpx(); void op_cmp_abs();
  void op_cmp_absx(); void op_cmp_absy(); void op_cmp_indx(); void op_cmp_indy();
  // CPX/CPY
  void op_cpx_imm(); void op_cpx_zp(); void op_cpx_abs();
  void op_cpy_imm(); void op_cpy_zp(); void op_cpy_abs();
  // BIT
  void op_bit_zp(); void op_bit_abs();
  // ASL
  void op_asl_acc(); void op_asl_zp(); void op_asl_zpx(); void op_asl_abs(); void op_asl_absx();
  // LSR
  void op_lsr_acc(); void op_lsr_zp(); void op_lsr_zpx(); void op_lsr_abs(); void op_lsr_absx();
  // ROL
  void op_rol_acc(); void op_rol_zp(); void op_rol_zpx(); void op_rol_abs(); void op_rol_absx();
  // ROR
  void op_ror_acc(); void op_ror_zp(); void op_ror_zpx(); void op_ror_abs(); void op_ror_absx();
  // INC/DEC
  void op_inc_zp(); void op_inc_zpx(); void op_inc_abs(); void op_inc_absx();
  void op_dec_zp(); void op_dec_zpx(); void op_dec_abs(); void op_dec_absx();
  void op_inx(); void op_iny(); void op_dex(); void op_dey();
  // Branch
  void op_bpl(); void op_bmi(); void op_bvc(); void op_bvs();
  void op_bcc(); void op_bcs(); void op_bne(); void op_beq();
  // Jump/Call
  void op_jmp_abs(); void op_jmp_ind(); void op_jsr(); void op_rts(); void op_rti(); void op_brk();
  // Stack
  void op_pha(); void op_pla(); void op_php(); void op_plp();
  // Transfer
  void op_tax(); void op_txa(); void op_tay(); void op_tya(); void op_tsx(); void op_txs();
  // Flags
  void op_clc(); void op_sec(); void op_cli(); void op_sei();
  void op_clv(); void op_cld(); void op_sed(); void op_nop();
  // NOP varianti (illegal, con/senza operando)
  void op_nop_imm(); void op_nop_zp(); void op_nop_zpx(); void op_nop_abs(); void op_nop_absx();

  // Illegal "combo" opcodes
  void op_slo_zp(); void op_slo_zpx(); void op_slo_abs(); void op_slo_absx();
  void op_slo_absy(); void op_slo_indx(); void op_slo_indy();
  void op_rla_zp(); void op_rla_zpx(); void op_rla_abs(); void op_rla_absx();
  void op_rla_absy(); void op_rla_indx(); void op_rla_indy();
  void op_sre_zp(); void op_sre_zpx(); void op_sre_abs(); void op_sre_absx();
  void op_sre_absy(); void op_sre_indx(); void op_sre_indy();
  void op_rra_zp(); void op_rra_zpx(); void op_rra_abs(); void op_rra_absx();
  void op_rra_absy(); void op_rra_indx(); void op_rra_indy();
  void op_lax_zp(); void op_lax_zpy(); void op_lax_abs(); void op_lax_absy();
  void op_lax_indx(); void op_lax_indy();
  void op_sax_zp(); void op_sax_zpy(); void op_sax_abs(); void op_sax_indx();
  void op_dcp_zp(); void op_dcp_zpx(); void op_dcp_abs(); void op_dcp_absx();
  void op_dcp_absy(); void op_dcp_indx(); void op_dcp_indy();
  void op_isc_zp(); void op_isc_zpx(); void op_isc_abs(); void op_isc_absx();
  void op_isc_absy(); void op_isc_indx(); void op_isc_indy();
  void op_anc_imm(); void op_arr_imm(); void op_alr_imm(); void op_axs_imm();
  void op_shx_absy(); void op_shy_absx(); void op_las_absy(); void op_tas_absy();
  void op_ahx_absy(); void op_ahx_indy(); void op_xaa_imm(); void op_lxa_imm();
  void op_jam();   // halt reale (le JAM bloccano davvero il bus sul 6502 vero)

  // ========================================================================
  // mem_read_fast / mem_write_fast: fast path RAM interna + dispatch IO/cart
  // ========================================================================
  inline uint8_t mem_read_fast(uint16_t addr) {
      if (addr >= 0x8000) return _prgWin[addr & 0x7FFF];            // il caso piu' frequente
      if (addr < 0x2000)  return memory[addr & (RAMSIZE - 1)];
      if (addr >= 0x6000) return _prgRam[addr & 0x1FFF];
      if (addr <= IO_END) return _ioRead ? _ioRead(addr) : 0xFF;
      return _memRead ? _memRead(addr) : 0xFF;                       // $4020-$5FFF
  }

  inline void mem_write_fast(uint16_t addr, uint8_t value) {
      if (addr < 0x2000) {
          memory[addr & (RAMSIZE - 1)] = value;
          return;
      }
      if (addr <= IO_END) {
          if (_ioWrite) _ioWrite(addr, value);
          return;
      }
      if (_memWrite) _memWrite(addr, value);
  }
};

#endif
