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
 */

#include "cpu.h"
#include <string.h>
#include "pines_ram.h"

// ---------------------------------------------------------------------------
// flag modifier macros
// ---------------------------------------------------------------------------
#define setcarry() cpustatus |= FLAG_CARRY
#define clearcarry() cpustatus &= (~FLAG_CARRY)
#define setzero() cpustatus |= FLAG_ZERO
#define clearzero() cpustatus &= (~FLAG_ZERO)
#define setinterrupt() cpustatus |= FLAG_INTERRUPT
#define clearinterrupt() cpustatus &= (~FLAG_INTERRUPT)
#define setdecimal() cpustatus |= FLAG_DECIMAL
#define cleardecimal() cpustatus &= (~FLAG_DECIMAL)
#define setoverflow() cpustatus |= FLAG_OVERFLOW
#define clearoverflow() cpustatus &= (~FLAG_OVERFLOW)
#define setsign() cpustatus |= FLAG_SIGN
#define clearsign() cpustatus &= (~FLAG_SIGN)
#define setbreak() cpustatus |= FLAG_BREAK
#define clearbreak() cpustatus &= (~FLAG_BREAK)

#define zerocalc(n)      { if ((n) & 0x00FF) clearzero(); else setzero(); }
#define signcalc(n)      { if ((n) & 0x0080) setsign();   else clearsign(); }
#define carrycalc(n)     { if ((n) & 0xFF00) setcarry();  else clearcarry(); }
#define overflowcalc(n, m, o)                                  \
    { if (((n) ^ (uint16_t)(m)) & ((n) ^ (o)) & 0x0080) setoverflow(); \
      else clearoverflow(); }

// Penalità di pagina: da chiamare con l'indirizzo BASE (pre-indicizzazione)
// e l'indirizzo EFFETTIVO (post-indicizzazione). +1 ciclo se la pagina alta
// cambia. Va usata solo nelle istruzioni di LETTURA indicizzate.
//
// Accumula la penalità
// in extraCyclesAccum, sommata UNA SOLA VOLTA (insieme al costo base
// dell'istruzione) nel loop principale di execCPU(), cosí sia
// clockgoal6502 (budget del loop) sia realtickcount/totalCycles (valore
// restituito al chiamante, usato per sincronizzare APU e IRQ MMC3)
// restano coerenti fra loro.
#define PAGE_CROSS_PENALTY(base, eff) \
    { if (((base) ^ (eff)) & 0xFF00) extraCyclesAccum++; }

// ---------------------------------------------------------------------------
// stato globale di esecuzione (un solo core CPU per processo, come Pi64)
// ---------------------------------------------------------------------------
static int32_t clockgoal6502 = 0;
// penalità (page-cross e branch preso/attraversamento pagina)
// accumulate durante l'esecuzione dell'istruzione corrente, azzerata ad
// ogni fetch e sommata al costo base una sola volta in execCPU().
static int32_t extraCyclesAccum = 0;
static uint8_t opcode;

cpu::OpcodeHandler cpu::opcodeTable[256];
bool cpu::opcodeTableInitialized = false;

// Tabella cicli BASE (senza penalità di pagina, aggiunta a runtime dove serve)
static uint8_t ticktable[256] = {
    /*        0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F */
    /* 0 */   7, 6, 2, 8, 3, 3, 5, 5, 3, 2, 2, 2, 4, 4, 6, 6,
    /* 1 */   2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
    /* 2 */   6, 6, 2, 8, 3, 3, 5, 5, 4, 2, 2, 2, 4, 4, 6, 6,
    /* 3 */   2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
    /* 4 */   6, 6, 2, 8, 3, 3, 5, 5, 3, 2, 2, 2, 3, 4, 6, 6,
    /* 5 */   2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
    /* 6 */   6, 6, 2, 8, 3, 3, 5, 5, 4, 2, 2, 2, 5, 4, 6, 6,
    /* 7 */   2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
    /* 8 */   2, 6, 2, 6, 3, 3, 3, 3, 2, 2, 2, 2, 4, 4, 4, 4,
    /* 9 */   2, 6, 2, 6, 4, 4, 4, 4, 2, 5, 2, 5, 5, 5, 5, 5,
    /* A */   2, 6, 2, 6, 3, 3, 3, 3, 2, 2, 2, 2, 4, 4, 4, 4,
    /* B */   2, 5, 2, 5, 4, 4, 4, 4, 2, 4, 2, 4, 4, 4, 4, 4,
    /* C */   2, 6, 2, 8, 3, 3, 5, 5, 2, 2, 2, 2, 4, 4, 6, 6,
    /* D */   2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
    /* E */   2, 6, 2, 8, 3, 3, 5, 5, 2, 2, 2, 2, 4, 4, 6, 6,
    /* F */   2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7
};

// ============================================================================
// COSTRUTTORE / begin()
// ============================================================================
cpu::cpu()
  : _memRead(nullptr), _memWrite(nullptr), _ioRead(nullptr), _ioWrite(nullptr),
    _nmiPending(false), _irqLine(false), totalCycles(0)
{
}

void cpu::begin(MemReadFn memRead, MemWriteFn memWrite,
                IOReadFn ioRead, IOWriteFn ioWrite) {
    _memRead  = memRead;
    _memWrite = memWrite;
    _ioRead   = ioRead;
    _ioWrite  = ioWrite;

    if (!opcodeTableInitialized) {
        initOpcodeTable();
        opcodeTableInitialized = true;
    }
}

void PINES_RAM cpu::requestNMI() {
    _nmiPending = true;
}

void PINES_RAM cpu::requestIRQ(bool level) {
    _irqLine = level;
}

void PINES_RAM cpu::stealCycles(int n) {
    // Usato da OAM DMA ($4014, 513/514 cicli) e DMC DMA (4 cicli):
    // il bus è occupato dal device esterno, la CPU resta "ferma" per n
    // cicli. Aggiorniamo sia il budget dell'istruzione in corso (se
    // execCPU è in esecuzione, cosa garantita: ioWrite è sempre chiamata
    // da dentro mem_write_fast, a sua volta chiamata da dentro un opcode
    // in esecuzione dentro execCPU) sia il contatore totale di cicli.
    clockgoal6502 -= n;
    totalCycles += n;
}

// ============================================================================
// INIZIALIZZAZIONE TABELLA DISPATCH (256 ENTRY)
// ============================================================================
void cpu::initOpcodeTable() {
    for (int i = 0; i < 256; i++) opcodeTable[i] = &cpu::op_nop;

    opcodeTable[0xA9] = &cpu::op_lda_imm;  opcodeTable[0xA5] = &cpu::op_lda_zp;
    opcodeTable[0xB5] = &cpu::op_lda_zpx;  opcodeTable[0xAD] = &cpu::op_lda_abs;
    opcodeTable[0xBD] = &cpu::op_lda_absx; opcodeTable[0xB9] = &cpu::op_lda_absy;
    opcodeTable[0xA1] = &cpu::op_lda_indx; opcodeTable[0xB1] = &cpu::op_lda_indy;

    opcodeTable[0xA2] = &cpu::op_ldx_imm;  opcodeTable[0xA6] = &cpu::op_ldx_zp;
    opcodeTable[0xB6] = &cpu::op_ldx_zpy;  opcodeTable[0xAE] = &cpu::op_ldx_abs;
    opcodeTable[0xBE] = &cpu::op_ldx_absy;

    opcodeTable[0xA0] = &cpu::op_ldy_imm;  opcodeTable[0xA4] = &cpu::op_ldy_zp;
    opcodeTable[0xB4] = &cpu::op_ldy_zpx;  opcodeTable[0xAC] = &cpu::op_ldy_abs;
    opcodeTable[0xBC] = &cpu::op_ldy_absx;

    opcodeTable[0x85] = &cpu::op_sta_zp;   opcodeTable[0x95] = &cpu::op_sta_zpx;
    opcodeTable[0x8D] = &cpu::op_sta_abs;  opcodeTable[0x9D] = &cpu::op_sta_absx;
    opcodeTable[0x99] = &cpu::op_sta_absy; opcodeTable[0x81] = &cpu::op_sta_indx;
    opcodeTable[0x91] = &cpu::op_sta_indy;

    opcodeTable[0x86] = &cpu::op_stx_zp;   opcodeTable[0x96] = &cpu::op_stx_zpy;
    opcodeTable[0x8E] = &cpu::op_stx_abs;

    opcodeTable[0x84] = &cpu::op_sty_zp;   opcodeTable[0x94] = &cpu::op_sty_zpx;
    opcodeTable[0x8C] = &cpu::op_sty_abs;

    opcodeTable[0x69] = &cpu::op_adc_imm;  opcodeTable[0x65] = &cpu::op_adc_zp;
    opcodeTable[0x75] = &cpu::op_adc_zpx;  opcodeTable[0x6D] = &cpu::op_adc_abs;
    opcodeTable[0x7D] = &cpu::op_adc_absx; opcodeTable[0x79] = &cpu::op_adc_absy;
    opcodeTable[0x61] = &cpu::op_adc_indx; opcodeTable[0x71] = &cpu::op_adc_indy;

    opcodeTable[0xE9] = &cpu::op_sbc_imm;  opcodeTable[0xE5] = &cpu::op_sbc_zp;
    opcodeTable[0xF5] = &cpu::op_sbc_zpx;  opcodeTable[0xED] = &cpu::op_sbc_abs;
    opcodeTable[0xFD] = &cpu::op_sbc_absx; opcodeTable[0xF9] = &cpu::op_sbc_absy;
    opcodeTable[0xE1] = &cpu::op_sbc_indx; opcodeTable[0xF1] = &cpu::op_sbc_indy;
    opcodeTable[0xEB] = &cpu::op_sbc_imm;  // SBC immediate illegale (alias)

    opcodeTable[0x29] = &cpu::op_and_imm;  opcodeTable[0x25] = &cpu::op_and_zp;
    opcodeTable[0x35] = &cpu::op_and_zpx;  opcodeTable[0x2D] = &cpu::op_and_abs;
    opcodeTable[0x3D] = &cpu::op_and_absx; opcodeTable[0x39] = &cpu::op_and_absy;
    opcodeTable[0x21] = &cpu::op_and_indx; opcodeTable[0x31] = &cpu::op_and_indy;

    opcodeTable[0x09] = &cpu::op_ora_imm;  opcodeTable[0x05] = &cpu::op_ora_zp;
    opcodeTable[0x15] = &cpu::op_ora_zpx;  opcodeTable[0x0D] = &cpu::op_ora_abs;
    opcodeTable[0x1D] = &cpu::op_ora_absx; opcodeTable[0x19] = &cpu::op_ora_absy;
    opcodeTable[0x01] = &cpu::op_ora_indx; opcodeTable[0x11] = &cpu::op_ora_indy;

    opcodeTable[0x49] = &cpu::op_eor_imm;  opcodeTable[0x45] = &cpu::op_eor_zp;
    opcodeTable[0x55] = &cpu::op_eor_zpx;  opcodeTable[0x4D] = &cpu::op_eor_abs;
    opcodeTable[0x5D] = &cpu::op_eor_absx; opcodeTable[0x59] = &cpu::op_eor_absy;
    opcodeTable[0x41] = &cpu::op_eor_indx; opcodeTable[0x51] = &cpu::op_eor_indy;

    opcodeTable[0xC9] = &cpu::op_cmp_imm;  opcodeTable[0xC5] = &cpu::op_cmp_zp;
    opcodeTable[0xD5] = &cpu::op_cmp_zpx;  opcodeTable[0xCD] = &cpu::op_cmp_abs;
    opcodeTable[0xDD] = &cpu::op_cmp_absx; opcodeTable[0xD9] = &cpu::op_cmp_absy;
    opcodeTable[0xC1] = &cpu::op_cmp_indx; opcodeTable[0xD1] = &cpu::op_cmp_indy;

    opcodeTable[0xE0] = &cpu::op_cpx_imm;  opcodeTable[0xE4] = &cpu::op_cpx_zp;
    opcodeTable[0xEC] = &cpu::op_cpx_abs;
    opcodeTable[0xC0] = &cpu::op_cpy_imm;  opcodeTable[0xC4] = &cpu::op_cpy_zp;
    opcodeTable[0xCC] = &cpu::op_cpy_abs;

    opcodeTable[0x24] = &cpu::op_bit_zp;   opcodeTable[0x2C] = &cpu::op_bit_abs;

    opcodeTable[0x0A] = &cpu::op_asl_acc;  opcodeTable[0x06] = &cpu::op_asl_zp;
    opcodeTable[0x16] = &cpu::op_asl_zpx;  opcodeTable[0x0E] = &cpu::op_asl_abs;
    opcodeTable[0x1E] = &cpu::op_asl_absx;

    opcodeTable[0x4A] = &cpu::op_lsr_acc;  opcodeTable[0x46] = &cpu::op_lsr_zp;
    opcodeTable[0x56] = &cpu::op_lsr_zpx;  opcodeTable[0x4E] = &cpu::op_lsr_abs;
    opcodeTable[0x5E] = &cpu::op_lsr_absx;

    opcodeTable[0x2A] = &cpu::op_rol_acc;  opcodeTable[0x26] = &cpu::op_rol_zp;
    opcodeTable[0x36] = &cpu::op_rol_zpx;  opcodeTable[0x2E] = &cpu::op_rol_abs;
    opcodeTable[0x3E] = &cpu::op_rol_absx;

    opcodeTable[0x6A] = &cpu::op_ror_acc;  opcodeTable[0x66] = &cpu::op_ror_zp;
    opcodeTable[0x76] = &cpu::op_ror_zpx;  opcodeTable[0x6E] = &cpu::op_ror_abs;
    opcodeTable[0x7E] = &cpu::op_ror_absx;

    opcodeTable[0xE6] = &cpu::op_inc_zp;   opcodeTable[0xF6] = &cpu::op_inc_zpx;
    opcodeTable[0xEE] = &cpu::op_inc_abs;  opcodeTable[0xFE] = &cpu::op_inc_absx;
    opcodeTable[0xE8] = &cpu::op_inx;      opcodeTable[0xC8] = &cpu::op_iny;

    opcodeTable[0xC6] = &cpu::op_dec_zp;   opcodeTable[0xD6] = &cpu::op_dec_zpx;
    opcodeTable[0xCE] = &cpu::op_dec_abs;  opcodeTable[0xDE] = &cpu::op_dec_absx;
    opcodeTable[0xCA] = &cpu::op_dex;      opcodeTable[0x88] = &cpu::op_dey;

    opcodeTable[0x10] = &cpu::op_bpl;      opcodeTable[0x30] = &cpu::op_bmi;
    opcodeTable[0x50] = &cpu::op_bvc;      opcodeTable[0x70] = &cpu::op_bvs;
    opcodeTable[0x90] = &cpu::op_bcc;      opcodeTable[0xB0] = &cpu::op_bcs;
    opcodeTable[0xD0] = &cpu::op_bne;      opcodeTable[0xF0] = &cpu::op_beq;

    opcodeTable[0x4C] = &cpu::op_jmp_abs;  opcodeTable[0x6C] = &cpu::op_jmp_ind;
    opcodeTable[0x20] = &cpu::op_jsr;      opcodeTable[0x60] = &cpu::op_rts;
    opcodeTable[0x40] = &cpu::op_rti;      opcodeTable[0x00] = &cpu::op_brk;

    opcodeTable[0x48] = &cpu::op_pha;      opcodeTable[0x68] = &cpu::op_pla;
    opcodeTable[0x08] = &cpu::op_php;      opcodeTable[0x28] = &cpu::op_plp;

    opcodeTable[0xAA] = &cpu::op_tax;      opcodeTable[0x8A] = &cpu::op_txa;
    opcodeTable[0xA8] = &cpu::op_tay;      opcodeTable[0x98] = &cpu::op_tya;
    opcodeTable[0xBA] = &cpu::op_tsx;      opcodeTable[0x9A] = &cpu::op_txs;

    opcodeTable[0x18] = &cpu::op_clc;      opcodeTable[0x38] = &cpu::op_sec;
    opcodeTable[0x58] = &cpu::op_cli;      opcodeTable[0x78] = &cpu::op_sei;
    opcodeTable[0xB8] = &cpu::op_clv;      opcodeTable[0xD8] = &cpu::op_cld;
    opcodeTable[0xF8] = &cpu::op_sed;

    // ===== ILLEGAL OPCODES =====
    opcodeTable[0x07] = &cpu::op_slo_zp;   opcodeTable[0x17] = &cpu::op_slo_zpx;
    opcodeTable[0x0F] = &cpu::op_slo_abs;  opcodeTable[0x1F] = &cpu::op_slo_absx;
    opcodeTable[0x1B] = &cpu::op_slo_absy; opcodeTable[0x03] = &cpu::op_slo_indx;
    opcodeTable[0x13] = &cpu::op_slo_indy;

    opcodeTable[0x27] = &cpu::op_rla_zp;   opcodeTable[0x37] = &cpu::op_rla_zpx;
    opcodeTable[0x2F] = &cpu::op_rla_abs;  opcodeTable[0x3F] = &cpu::op_rla_absx;
    opcodeTable[0x3B] = &cpu::op_rla_absy; opcodeTable[0x23] = &cpu::op_rla_indx;
    opcodeTable[0x33] = &cpu::op_rla_indy;

    opcodeTable[0x47] = &cpu::op_sre_zp;   opcodeTable[0x57] = &cpu::op_sre_zpx;
    opcodeTable[0x4F] = &cpu::op_sre_abs;  opcodeTable[0x5F] = &cpu::op_sre_absx;
    opcodeTable[0x5B] = &cpu::op_sre_absy; opcodeTable[0x43] = &cpu::op_sre_indx;
    opcodeTable[0x53] = &cpu::op_sre_indy;

    opcodeTable[0x67] = &cpu::op_rra_zp;   opcodeTable[0x77] = &cpu::op_rra_zpx;
    opcodeTable[0x6F] = &cpu::op_rra_abs;  opcodeTable[0x7F] = &cpu::op_rra_absx;
    opcodeTable[0x7B] = &cpu::op_rra_absy; opcodeTable[0x63] = &cpu::op_rra_indx;
    opcodeTable[0x73] = &cpu::op_rra_indy;

    opcodeTable[0xA7] = &cpu::op_lax_zp;   opcodeTable[0xB7] = &cpu::op_lax_zpy;
    opcodeTable[0xAF] = &cpu::op_lax_abs;  opcodeTable[0xBF] = &cpu::op_lax_absy;
    opcodeTable[0xA3] = &cpu::op_lax_indx; opcodeTable[0xB3] = &cpu::op_lax_indy;

    opcodeTable[0x87] = &cpu::op_sax_zp;   opcodeTable[0x97] = &cpu::op_sax_zpy;
    opcodeTable[0x8F] = &cpu::op_sax_abs;  opcodeTable[0x83] = &cpu::op_sax_indx;

    opcodeTable[0xC7] = &cpu::op_dcp_zp;   opcodeTable[0xD7] = &cpu::op_dcp_zpx;
    opcodeTable[0xCF] = &cpu::op_dcp_abs;  opcodeTable[0xDF] = &cpu::op_dcp_absx;
    opcodeTable[0xDB] = &cpu::op_dcp_absy; opcodeTable[0xC3] = &cpu::op_dcp_indx;
    opcodeTable[0xD3] = &cpu::op_dcp_indy;

    opcodeTable[0xE7] = &cpu::op_isc_zp;   opcodeTable[0xF7] = &cpu::op_isc_zpx;
    opcodeTable[0xEF] = &cpu::op_isc_abs;  opcodeTable[0xFF] = &cpu::op_isc_absx;
    opcodeTable[0xFB] = &cpu::op_isc_absy; opcodeTable[0xE3] = &cpu::op_isc_indx;
    opcodeTable[0xF3] = &cpu::op_isc_indy;

    opcodeTable[0x0B] = &cpu::op_anc_imm;  opcodeTable[0x2B] = &cpu::op_anc_imm;
    opcodeTable[0x6B] = &cpu::op_arr_imm;  opcodeTable[0x4B] = &cpu::op_alr_imm;
    opcodeTable[0x9E] = &cpu::op_shx_absy; opcodeTable[0x9C] = &cpu::op_shy_absx;
    opcodeTable[0xBB] = &cpu::op_las_absy; opcodeTable[0x9B] = &cpu::op_tas_absy;
    opcodeTable[0x9F] = &cpu::op_ahx_absy; opcodeTable[0x93] = &cpu::op_ahx_indy;
    opcodeTable[0x8B] = &cpu::op_xaa_imm;  opcodeTable[0xCB] = &cpu::op_axs_imm;
    opcodeTable[0xAB] = &cpu::op_lxa_imm;

    // NOP impliciti illegali (1 byte)
    opcodeTable[0x1A] = &cpu::op_nop;  opcodeTable[0x3A] = &cpu::op_nop;
    opcodeTable[0x5A] = &cpu::op_nop;  opcodeTable[0x7A] = &cpu::op_nop;
    opcodeTable[0xDA] = &cpu::op_nop;  opcodeTable[0xFA] = &cpu::op_nop;

    // NOP con operando (leggono memoria/IO -> rilevante per side-effect su
    // registri PPU/APU mappati in IO! NON sostituire con NOP "puri".)
    opcodeTable[0x80] = &cpu::op_nop_imm; opcodeTable[0x82] = &cpu::op_nop_imm;
    opcodeTable[0x89] = &cpu::op_nop_imm; opcodeTable[0xC2] = &cpu::op_nop_imm;
    opcodeTable[0xE2] = &cpu::op_nop_imm;
    opcodeTable[0x04] = &cpu::op_nop_zp;  opcodeTable[0x44] = &cpu::op_nop_zp;
    opcodeTable[0x64] = &cpu::op_nop_zp;
    opcodeTable[0x14] = &cpu::op_nop_zpx; opcodeTable[0x34] = &cpu::op_nop_zpx;
    opcodeTable[0x54] = &cpu::op_nop_zpx; opcodeTable[0x74] = &cpu::op_nop_zpx;
    opcodeTable[0xD4] = &cpu::op_nop_zpx; opcodeTable[0xF4] = &cpu::op_nop_zpx;
    opcodeTable[0x0C] = &cpu::op_nop_abs;
    opcodeTable[0x1C] = &cpu::op_nop_absx; opcodeTable[0x3C] = &cpu::op_nop_absx;
    opcodeTable[0x5C] = &cpu::op_nop_absx; opcodeTable[0x7C] = &cpu::op_nop_absx;
    opcodeTable[0xDC] = &cpu::op_nop_absx; opcodeTable[0xFC] = &cpu::op_nop_absx;

    // JAM: blocco reale del bus (vedi op_jam)
    static const uint8_t jamOpcodes[12] = {
        0x02,0x12,0x22,0x32,0x42,0x52,0x62,0x72,0x92,0xB2,0xD2,0xF2
    };
    for (uint8_t op : jamOpcodes) opcodeTable[op] = &cpu::op_jam;
}

// ============================================================================
// RESET
// ============================================================================
void cpu::resetCPU() {
    if (!opcodeTableInitialized) {
        initOpcodeTable();
        opcodeTableInitialized = true;
    }
    memset(memory, 0, sizeof(memory));

    a = 0; x = 0; y = 0;
    sp = 0xFD;  // valore "canonico" dopo reset su 6502 reale (FF - 3 push fittizi)
    cpustatus = FLAG_CONSTANT | FLAG_INTERRUPT;

    // Vettore di reset: 0xFFFC/0xFFFD, letto tramite memRead (mapper/PRG-ROM)
    uint8_t lo = mem_read(0xFFFC);
    uint8_t hi = mem_read(0xFFFD);
    pc = (uint16_t)lo | ((uint16_t)hi << 8);

    _nmiPending = false;
    _irqLine = false;
    totalCycles = 0;
}

uint16_t cpu::getpc() { return pc; }
uint8_t  cpu::getop() { return opcode; }

// ============================================================================
// STACK OPERATIONS
// ============================================================================
inline void cpu::push16(uint16_t pushval) {
    mem_write(BASE_STACK + sp, (pushval >> 8) & 0xFF);
    mem_write(BASE_STACK + ((sp - 1) & 0xFF), pushval & 0xFF);
    sp -= 2;
}
inline void cpu::push8(uint8_t pushval) {
    mem_write(BASE_STACK + sp--, pushval);
}
inline uint16_t cpu::pull16() {
    uint16_t temp16 = mem_read(BASE_STACK + ((sp + 1) & 0xFF)) |
                       ((uint16_t)mem_read(BASE_STACK + ((sp + 2) & 0xFF)) << 8);
    sp += 2;
    return temp16;
}
inline uint8_t cpu::pull8() {
    return mem_read(BASE_STACK + (uint8_t)(++sp));
}

// ============================================================================
// INTERRUPT HANDLERS (ora con costo di 7 cicli correttamente contabilizzato)
// ============================================================================
void PINES_RAM cpu::nmi() {
    push16(pc);
    push8((cpustatus & ~FLAG_BREAK) | FLAG_CONSTANT);
    setinterrupt();
    pc = (uint16_t)mem_read(0xFFFA) | ((uint16_t)mem_read(0xFFFB) << 8);
    clockgoal6502 -= 7;
    totalCycles += 7;
}

void PINES_RAM cpu::irq() {
    if (cpustatus & FLAG_INTERRUPT) return;  // mascherato: non servito
    push16(pc);
    push8((cpustatus & ~FLAG_BREAK) | FLAG_CONSTANT);
    setinterrupt();
    pc = (uint16_t)mem_read(0xFFFE) | ((uint16_t)mem_read(0xFFFF) << 8);
    clockgoal6502 -= 7;
    totalCycles += 7;
}

// ============================================================================
// MAIN CPU EXECUTION LOOP
// ============================================================================
int PINES_RAM cpu::execCPU(int tickcount) {
    clockgoal6502 = tickcount;
    int realtickcount = 0;

    while (clockgoal6502 > 0) {
        // Controllo NMI/IRQ ad OGNI istruzione (vedi nota fix #5 in testa al file)
        if (_nmiPending) {
            _nmiPending = false;
            nmi();
            realtickcount += 7;
        }
        if (_irqLine && !(cpustatus & FLAG_INTERRUPT)) {
            irq();
            realtickcount += 7;
        }

        // azzerata prima del fetch, accumula eventuali penalità
        // page-cross/branch maturate DURANTE l'esecuzione dell'istruzione.
        extraCyclesAccum = 0;

        opcode = mem_read(pc++);
        cpustatus |= FLAG_CONSTANT;
        (this->*opcodeTable[opcode])();

        // costo base + penalità, sommato in un colpo solo sia al
        // budget del loop (clockgoal6502) sia al valore restituito al
        // chiamante (realtickcount/totalCycles) — prima la penalità
        // finiva SOLO nel primo, causando un sottoconteggio sistematico
        // dei cicli riportati ad ogni page-cross/branch preso.
        int cycles = ticktable[opcode] + extraCyclesAccum;
        clockgoal6502 -= cycles;
        realtickcount += cycles;
        totalCycles += cycles;
    }

    return realtickcount;
}

// ============================================================================
// LDA
// ============================================================================
void PINES_RAM cpu::op_lda_imm() { a = mem_read(pc++); zerocalc(a); signcalc(a); }
void PINES_RAM cpu::op_lda_zp()  { uint16_t addr = mem_read(pc++); a = mem_read(addr); zerocalc(a); signcalc(a); }
void PINES_RAM cpu::op_lda_zpx() { uint16_t addr = (mem_read(pc++) + x) & 0xFF; a = mem_read(addr); zerocalc(a); signcalc(a); }
void PINES_RAM cpu::op_lda_abs() { uint16_t addr = mem_read(pc) | (mem_read(pc+1)<<8); pc+=2; a = mem_read(addr); zerocalc(a); signcalc(a); }
void PINES_RAM cpu::op_lda_absx() {
    uint16_t base = mem_read(pc) | (mem_read(pc+1)<<8); pc+=2;
    uint16_t addr = base + x;
    PAGE_CROSS_PENALTY(base, addr);
    a = mem_read(addr); zerocalc(a); signcalc(a);
}
void PINES_RAM cpu::op_lda_absy() {
    uint16_t base = mem_read(pc) | (mem_read(pc+1)<<8); pc+=2;
    uint16_t addr = base + y;
    PAGE_CROSS_PENALTY(base, addr);
    a = mem_read(addr); zerocalc(a); signcalc(a);
}
void PINES_RAM cpu::op_lda_indx() {
    uint16_t zp = (mem_read(pc++) + x) & 0xFF;
    uint16_t addr = mem_read(zp) | (mem_read((zp+1)&0xFF)<<8);
    a = mem_read(addr); zerocalc(a); signcalc(a);
}
void PINES_RAM cpu::op_lda_indy() {
    uint16_t zp = mem_read(pc++);
    uint16_t base = mem_read(zp) | (mem_read((zp+1)&0xFF)<<8);
    uint16_t addr = base + y;
    PAGE_CROSS_PENALTY(base, addr);
    a = mem_read(addr); zerocalc(a); signcalc(a);
}

// ============================================================================
// LDX
// ============================================================================
void PINES_RAM cpu::op_ldx_imm() { x = mem_read(pc++); zerocalc(x); signcalc(x); }
void PINES_RAM cpu::op_ldx_zp()  { uint16_t addr = mem_read(pc++); x = mem_read(addr); zerocalc(x); signcalc(x); }
void PINES_RAM cpu::op_ldx_zpy() { uint16_t addr = (mem_read(pc++) + y) & 0xFF; x = mem_read(addr); zerocalc(x); signcalc(x); }
void PINES_RAM cpu::op_ldx_abs() { uint16_t addr = mem_read(pc) | (mem_read(pc+1)<<8); pc+=2; x = mem_read(addr); zerocalc(x); signcalc(x); }
void PINES_RAM cpu::op_ldx_absy() {
    uint16_t base = mem_read(pc) | (mem_read(pc+1)<<8); pc+=2;
    uint16_t addr = base + y;
    PAGE_CROSS_PENALTY(base, addr);
    x = mem_read(addr); zerocalc(x); signcalc(x);
}

// ============================================================================
// LDY
// ============================================================================
void PINES_RAM cpu::op_ldy_imm() { y = mem_read(pc++); zerocalc(y); signcalc(y); }
void PINES_RAM cpu::op_ldy_zp()  { uint16_t addr = mem_read(pc++); y = mem_read(addr); zerocalc(y); signcalc(y); }
void PINES_RAM cpu::op_ldy_zpx() { uint16_t addr = (mem_read(pc++) + x) & 0xFF; y = mem_read(addr); zerocalc(y); signcalc(y); }
void PINES_RAM cpu::op_ldy_abs() { uint16_t addr = mem_read(pc) | (mem_read(pc+1)<<8); pc+=2; y = mem_read(addr); zerocalc(y); signcalc(y); }
void PINES_RAM cpu::op_ldy_absx() {
    uint16_t base = mem_read(pc) | (mem_read(pc+1)<<8); pc+=2;
    uint16_t addr = base + x;
    PAGE_CROSS_PENALTY(base, addr);
    y = mem_read(addr); zerocalc(y); signcalc(y);
}

// ============================================================================
// STA / STX / STY  (nessuna penalità di pagina: costo sempre fisso)
// ============================================================================
void PINES_RAM cpu::op_sta_zp()   { uint16_t addr = mem_read(pc++); mem_write(addr, a); }
void PINES_RAM cpu::op_sta_zpx()  { uint16_t addr = (mem_read(pc++) + x) & 0xFF; mem_write(addr, a); }
void PINES_RAM cpu::op_sta_abs()  { uint16_t addr = mem_read(pc) | (mem_read(pc+1)<<8); pc+=2; mem_write(addr, a); }
void PINES_RAM cpu::op_sta_absx() { uint16_t addr = (mem_read(pc) | (mem_read(pc+1)<<8)) + x; pc+=2; mem_write(addr, a); }
void PINES_RAM cpu::op_sta_absy() { uint16_t addr = (mem_read(pc) | (mem_read(pc+1)<<8)) + y; pc+=2; mem_write(addr, a); }
void PINES_RAM cpu::op_sta_indx() {
    uint16_t zp = (mem_read(pc++) + x) & 0xFF;
    uint16_t addr = mem_read(zp) | (mem_read((zp+1)&0xFF)<<8);
    mem_write(addr, a);
}
void PINES_RAM cpu::op_sta_indy() {
    uint16_t zp = mem_read(pc++);
    uint16_t addr = (mem_read(zp) | (mem_read((zp+1)&0xFF)<<8)) + y;
    mem_write(addr, a);
}

void PINES_RAM cpu::op_stx_zp()  { uint16_t addr = mem_read(pc++); mem_write(addr, x); }
void PINES_RAM cpu::op_stx_zpy() { uint16_t addr = (mem_read(pc++) + y) & 0xFF; mem_write(addr, x); }
void PINES_RAM cpu::op_stx_abs() { uint16_t addr = mem_read(pc) | (mem_read(pc+1)<<8); pc+=2; mem_write(addr, x); }

void PINES_RAM cpu::op_sty_zp()  { uint16_t addr = mem_read(pc++); mem_write(addr, y); }
void PINES_RAM cpu::op_sty_zpx() { uint16_t addr = (mem_read(pc++) + x) & 0xFF; mem_write(addr, y); }
void PINES_RAM cpu::op_sty_abs() { uint16_t addr = mem_read(pc) | (mem_read(pc+1)<<8); pc+=2; mem_write(addr, y); }

// ============================================================================
// ADC / SBC — SOLO BINARIO (2A03: nessun BCD funzionante, vedi fix #1)
// ============================================================================
static inline __attribute__((always_inline)) void do_adc(uint8_t &a, uint8_t &cpustatus, uint8_t value) {
    uint16_t result = (uint16_t)a + value + (uint16_t)(cpustatus & FLAG_CARRY);
    carrycalc(result);
    zerocalc(result);
    overflowcalc(result, a, value);
    signcalc(result);
    a = (uint8_t)(result & 0xFF);
}
// SBC è aritmeticamente ADC(a, ~value, carry) — il calcolo del
// carry qui sotto tratta già correttamente (cpustatus&FLAG_CARRY)?0:1
// come "borrow" invertito, quindi anche l'overflow va calcolato su
// ~value, non su value diretto (altrimenti il flag V risulta sbagliato:
// verificato con A=0x80, M=0x01, no borrow — overflow reale, la vecchia
// formula restituiva V=0 invece di V=1).
static inline __attribute__((always_inline)) void do_sbc(uint8_t &a, uint8_t &cpustatus, uint8_t value) {
    uint16_t result = (uint16_t)a - value - ((cpustatus & FLAG_CARRY) ? 0 : 1);
    if (result < 0x100) setcarry(); else clearcarry();
    zerocalc(result);
    signcalc(result);
    overflowcalc(result, a, (uint8_t)(~value));
    a = (uint8_t)(result & 0xFF);
}

void PINES_RAM cpu::op_adc_imm()  { uint8_t v = mem_read(pc++); do_adc(a, cpustatus, v); }
void PINES_RAM cpu::op_adc_zp()   { uint16_t addr = mem_read(pc++); do_adc(a, cpustatus, mem_read(addr)); }
void PINES_RAM cpu::op_adc_zpx()  { uint16_t addr = (mem_read(pc++) + x) & 0xFF; do_adc(a, cpustatus, mem_read(addr)); }
void PINES_RAM cpu::op_adc_abs()  { uint16_t addr = mem_read(pc) | (mem_read(pc+1)<<8); pc+=2; do_adc(a, cpustatus, mem_read(addr)); }
void PINES_RAM cpu::op_adc_absx() {
    uint16_t base = mem_read(pc) | (mem_read(pc+1)<<8); pc+=2;
    uint16_t addr = base + x;
    PAGE_CROSS_PENALTY(base, addr);
    do_adc(a, cpustatus, mem_read(addr));
}
void PINES_RAM cpu::op_adc_absy() {
    uint16_t base = mem_read(pc) | (mem_read(pc+1)<<8); pc+=2;
    uint16_t addr = base + y;
    PAGE_CROSS_PENALTY(base, addr);
    do_adc(a, cpustatus, mem_read(addr));
}
void PINES_RAM cpu::op_adc_indx() {
    uint16_t zp = (mem_read(pc++) + x) & 0xFF;
    uint16_t addr = mem_read(zp) | (mem_read((zp+1)&0xFF)<<8);
    do_adc(a, cpustatus, mem_read(addr));
}
void PINES_RAM cpu::op_adc_indy() {
    uint16_t zp = mem_read(pc++);
    uint16_t base = mem_read(zp) | (mem_read((zp+1)&0xFF)<<8);
    uint16_t addr = base + y;
    PAGE_CROSS_PENALTY(base, addr);
    do_adc(a, cpustatus, mem_read(addr));
}

void PINES_RAM cpu::op_sbc_imm()  { uint8_t v = mem_read(pc++); do_sbc(a, cpustatus, v); }
void PINES_RAM cpu::op_sbc_zp()   { uint16_t addr = mem_read(pc++); do_sbc(a, cpustatus, mem_read(addr)); }
void PINES_RAM cpu::op_sbc_zpx()  { uint16_t addr = (mem_read(pc++) + x) & 0xFF; do_sbc(a, cpustatus, mem_read(addr)); }
void PINES_RAM cpu::op_sbc_abs()  { uint16_t addr = mem_read(pc) | (mem_read(pc+1)<<8); pc+=2; do_sbc(a, cpustatus, mem_read(addr)); }
void PINES_RAM cpu::op_sbc_absx() {
    uint16_t base = mem_read(pc) | (mem_read(pc+1)<<8); pc+=2;
    uint16_t addr = base + x;
    PAGE_CROSS_PENALTY(base, addr);
    do_sbc(a, cpustatus, mem_read(addr));
}
void PINES_RAM cpu::op_sbc_absy() {
    uint16_t base = mem_read(pc) | (mem_read(pc+1)<<8); pc+=2;
    uint16_t addr = base + y;
    PAGE_CROSS_PENALTY(base, addr);
    do_sbc(a, cpustatus, mem_read(addr));
}
void PINES_RAM cpu::op_sbc_indx() {
    uint16_t zp = (mem_read(pc++) + x) & 0xFF;
    uint16_t addr = mem_read(zp) | (mem_read((zp+1)&0xFF)<<8);
    do_sbc(a, cpustatus, mem_read(addr));
}
void PINES_RAM cpu::op_sbc_indy() {
    uint16_t zp = mem_read(pc++);
    uint16_t base = mem_read(zp) | (mem_read((zp+1)&0xFF)<<8);
    uint16_t addr = base + y;
    PAGE_CROSS_PENALTY(base, addr);
    do_sbc(a, cpustatus, mem_read(addr));
}

// ============================================================================
// AND / ORA / EOR
// ============================================================================
#define DEFINE_LOGIC_OP(NAME, OPER)                                                          \
void PINES_RAM cpu::op_##NAME##_imm()  { a OPER##= mem_read(pc++); zerocalc(a); signcalc(a); }          \
void PINES_RAM cpu::op_##NAME##_zp()   { uint16_t addr = mem_read(pc++); a OPER##= mem_read(addr); zerocalc(a); signcalc(a); } \
void PINES_RAM cpu::op_##NAME##_zpx()  { uint16_t addr = (mem_read(pc++) + x) & 0xFF; a OPER##= mem_read(addr); zerocalc(a); signcalc(a); } \
void PINES_RAM cpu::op_##NAME##_abs()  { uint16_t addr = mem_read(pc) | (mem_read(pc+1)<<8); pc+=2; a OPER##= mem_read(addr); zerocalc(a); signcalc(a); } \
void PINES_RAM cpu::op_##NAME##_absx() { uint16_t base = mem_read(pc) | (mem_read(pc+1)<<8); pc+=2; uint16_t addr = base + x; PAGE_CROSS_PENALTY(base, addr); a OPER##= mem_read(addr); zerocalc(a); signcalc(a); } \
void PINES_RAM cpu::op_##NAME##_absy() { uint16_t base = mem_read(pc) | (mem_read(pc+1)<<8); pc+=2; uint16_t addr = base + y; PAGE_CROSS_PENALTY(base, addr); a OPER##= mem_read(addr); zerocalc(a); signcalc(a); } \
void PINES_RAM cpu::op_##NAME##_indx() { uint16_t zp = (mem_read(pc++) + x) & 0xFF; uint16_t addr = mem_read(zp) | (mem_read((zp+1)&0xFF)<<8); a OPER##= mem_read(addr); zerocalc(a); signcalc(a); } \
void PINES_RAM cpu::op_##NAME##_indy() { uint16_t zp = mem_read(pc++); uint16_t base = mem_read(zp) | (mem_read((zp+1)&0xFF)<<8); uint16_t addr = base + y; PAGE_CROSS_PENALTY(base, addr); a OPER##= mem_read(addr); zerocalc(a); signcalc(a); }

DEFINE_LOGIC_OP(and, &)
DEFINE_LOGIC_OP(ora, |)
DEFINE_LOGIC_OP(eor, ^)
#undef DEFINE_LOGIC_OP

// ============================================================================
// CMP / CPX / CPY
// ============================================================================
static inline __attribute__((always_inline)) void do_cmp(uint8_t reg, uint8_t value, uint8_t &cpustatus) {
    uint16_t result = (uint16_t)reg - value;
    if (reg >= value) setcarry(); else clearcarry();
    if (reg == value) setzero(); else clearzero();
    signcalc(result);
}

void PINES_RAM cpu::op_cmp_imm()  { uint8_t v = mem_read(pc++); do_cmp(a, v, cpustatus); }
void PINES_RAM cpu::op_cmp_zp()   { uint16_t addr = mem_read(pc++); do_cmp(a, mem_read(addr), cpustatus); }
void PINES_RAM cpu::op_cmp_zpx()  { uint16_t addr = (mem_read(pc++) + x) & 0xFF; do_cmp(a, mem_read(addr), cpustatus); }
void PINES_RAM cpu::op_cmp_abs()  { uint16_t addr = mem_read(pc) | (mem_read(pc+1)<<8); pc+=2; do_cmp(a, mem_read(addr), cpustatus); }
void PINES_RAM cpu::op_cmp_absx() {
    uint16_t base = mem_read(pc) | (mem_read(pc+1)<<8); pc+=2;
    uint16_t addr = base + x; PAGE_CROSS_PENALTY(base, addr);
    do_cmp(a, mem_read(addr), cpustatus);
}
void PINES_RAM cpu::op_cmp_absy() {
    uint16_t base = mem_read(pc) | (mem_read(pc+1)<<8); pc+=2;
    uint16_t addr = base + y; PAGE_CROSS_PENALTY(base, addr);
    do_cmp(a, mem_read(addr), cpustatus);
}
void PINES_RAM cpu::op_cmp_indx() {
    uint16_t zp = (mem_read(pc++) + x) & 0xFF;
    uint16_t addr = mem_read(zp) | (mem_read((zp+1)&0xFF)<<8);
    do_cmp(a, mem_read(addr), cpustatus);
}
void PINES_RAM cpu::op_cmp_indy() {
    uint16_t zp = mem_read(pc++);
    uint16_t base = mem_read(zp) | (mem_read((zp+1)&0xFF)<<8);
    uint16_t addr = base + y; PAGE_CROSS_PENALTY(base, addr);
    do_cmp(a, mem_read(addr), cpustatus);
}

void PINES_RAM cpu::op_cpx_imm() { uint8_t v = mem_read(pc++); do_cmp(x, v, cpustatus); }
void PINES_RAM cpu::op_cpx_zp()  { uint16_t addr = mem_read(pc++); do_cmp(x, mem_read(addr), cpustatus); }
void PINES_RAM cpu::op_cpx_abs() { uint16_t addr = mem_read(pc) | (mem_read(pc+1)<<8); pc+=2; do_cmp(x, mem_read(addr), cpustatus); }
void PINES_RAM cpu::op_cpy_imm() { uint8_t v = mem_read(pc++); do_cmp(y, v, cpustatus); }
void PINES_RAM cpu::op_cpy_zp()  { uint16_t addr = mem_read(pc++); do_cmp(y, mem_read(addr), cpustatus); }
void PINES_RAM cpu::op_cpy_abs() { uint16_t addr = mem_read(pc) | (mem_read(pc+1)<<8); pc+=2; do_cmp(y, mem_read(addr), cpustatus); }

// ============================================================================
// BIT
// ============================================================================
void PINES_RAM cpu::op_bit_zp() {
    uint16_t addr = mem_read(pc++);
    uint8_t value = mem_read(addr);
    uint16_t result = (uint16_t)a & value;
    zerocalc(result);
    cpustatus = (cpustatus & 0x3F) | (value & 0xC0);
}
void PINES_RAM cpu::op_bit_abs() {
    uint16_t addr = mem_read(pc) | (mem_read(pc+1)<<8); pc+=2;
    uint8_t value = mem_read(addr);
    uint16_t result = (uint16_t)a & value;
    zerocalc(result);
    cpustatus = (cpustatus & 0x3F) | (value & 0xC0);
}

// ============================================================================
// ASL / LSR / ROL / ROR  (R-M-W: nessuna penalità di pagina, costo fisso)
// ============================================================================
void PINES_RAM cpu::op_asl_acc() { uint16_t r = (uint16_t)a << 1; carrycalc(r); zerocalc(r); signcalc(r); a = r & 0xFF; }
void PINES_RAM cpu::op_asl_zp()  { uint16_t addr = mem_read(pc++); uint16_t r = mem_read(addr) << 1; carrycalc(r); zerocalc(r); signcalc(r); mem_write(addr, r & 0xFF); }
void PINES_RAM cpu::op_asl_zpx() { uint16_t addr = (mem_read(pc++) + x) & 0xFF; uint16_t r = mem_read(addr) << 1; carrycalc(r); zerocalc(r); signcalc(r); mem_write(addr, r & 0xFF); }
void PINES_RAM cpu::op_asl_abs() { uint16_t addr = mem_read(pc) | (mem_read(pc+1)<<8); pc+=2; uint16_t r = mem_read(addr) << 1; carrycalc(r); zerocalc(r); signcalc(r); mem_write(addr, r & 0xFF); }
void PINES_RAM cpu::op_asl_absx(){ uint16_t addr = (mem_read(pc) | (mem_read(pc+1)<<8)) + x; pc+=2; uint16_t r = mem_read(addr) << 1; carrycalc(r); zerocalc(r); signcalc(r); mem_write(addr, r & 0xFF); }

void PINES_RAM cpu::op_lsr_acc() { if (a & 1) setcarry(); else clearcarry(); a >>= 1; zerocalc(a); clearsign(); }
void PINES_RAM cpu::op_lsr_zp()  { uint16_t addr = mem_read(pc++); uint8_t v = mem_read(addr); if (v&1) setcarry(); else clearcarry(); uint8_t r = v>>1; zerocalc(r); clearsign(); mem_write(addr, r); }
void PINES_RAM cpu::op_lsr_zpx() { uint16_t addr = (mem_read(pc++) + x) & 0xFF; uint8_t v = mem_read(addr); if (v&1) setcarry(); else clearcarry(); uint8_t r = v>>1; zerocalc(r); clearsign(); mem_write(addr, r); }
void PINES_RAM cpu::op_lsr_abs() { uint16_t addr = mem_read(pc) | (mem_read(pc+1)<<8); pc+=2; uint8_t v = mem_read(addr); if (v&1) setcarry(); else clearcarry(); uint8_t r = v>>1; zerocalc(r); clearsign(); mem_write(addr, r); }
void PINES_RAM cpu::op_lsr_absx(){ uint16_t addr = (mem_read(pc) | (mem_read(pc+1)<<8)) + x; pc+=2; uint8_t v = mem_read(addr); if (v&1) setcarry(); else clearcarry(); uint8_t r = v>>1; zerocalc(r); clearsign(); mem_write(addr, r); }

void PINES_RAM cpu::op_rol_acc() { uint16_t r = ((uint16_t)a << 1) | (cpustatus & FLAG_CARRY); carrycalc(r); zerocalc(r); signcalc(r); a = r & 0xFF; }
void PINES_RAM cpu::op_rol_zp()  { uint16_t addr = mem_read(pc++); uint16_t r = (mem_read(addr) << 1) | (cpustatus & FLAG_CARRY); carrycalc(r); zerocalc(r); signcalc(r); mem_write(addr, r & 0xFF); }
void PINES_RAM cpu::op_rol_zpx() { uint16_t addr = (mem_read(pc++) + x) & 0xFF; uint16_t r = (mem_read(addr) << 1) | (cpustatus & FLAG_CARRY); carrycalc(r); zerocalc(r); signcalc(r); mem_write(addr, r & 0xFF); }
void PINES_RAM cpu::op_rol_abs() { uint16_t addr = mem_read(pc) | (mem_read(pc+1)<<8); pc+=2; uint16_t r = (mem_read(addr) << 1) | (cpustatus & FLAG_CARRY); carrycalc(r); zerocalc(r); signcalc(r); mem_write(addr, r & 0xFF); }
void PINES_RAM cpu::op_rol_absx(){ uint16_t addr = (mem_read(pc) | (mem_read(pc+1)<<8)) + x; pc+=2; uint16_t r = (mem_read(addr) << 1) | (cpustatus & FLAG_CARRY); carrycalc(r); zerocalc(r); signcalc(r); mem_write(addr, r & 0xFF); }

void PINES_RAM cpu::op_ror_acc() { uint8_t r = (a >> 1) | ((cpustatus & FLAG_CARRY) << 7); if (a & 1) setcarry(); else clearcarry(); zerocalc(r); signcalc(r); a = r; }
void PINES_RAM cpu::op_ror_zp()  { uint16_t addr = mem_read(pc++); uint8_t v = mem_read(addr); uint8_t r = (v >> 1) | ((cpustatus & FLAG_CARRY) << 7); if (v&1) setcarry(); else clearcarry(); zerocalc(r); signcalc(r); mem_write(addr, r); }
void PINES_RAM cpu::op_ror_zpx() { uint16_t addr = (mem_read(pc++) + x) & 0xFF; uint8_t v = mem_read(addr); uint8_t r = (v >> 1) | ((cpustatus & FLAG_CARRY) << 7); if (v&1) setcarry(); else clearcarry(); zerocalc(r); signcalc(r); mem_write(addr, r); }
void PINES_RAM cpu::op_ror_abs() { uint16_t addr = mem_read(pc) | (mem_read(pc+1)<<8); pc+=2; uint8_t v = mem_read(addr); uint8_t r = (v >> 1) | ((cpustatus & FLAG_CARRY) << 7); if (v&1) setcarry(); else clearcarry(); zerocalc(r); signcalc(r); mem_write(addr, r); }
void PINES_RAM cpu::op_ror_absx(){ uint16_t addr = (mem_read(pc) | (mem_read(pc+1)<<8)) + x; pc+=2; uint8_t v = mem_read(addr); uint8_t r = (v >> 1) | ((cpustatus & FLAG_CARRY) << 7); if (v&1) setcarry(); else clearcarry(); zerocalc(r); signcalc(r); mem_write(addr, r); }

// ============================================================================
// INC / DEC (memoria e registri)
// ============================================================================
void PINES_RAM cpu::op_inc_zp()   { uint16_t addr = mem_read(pc++); uint8_t r = mem_read(addr) + 1; zerocalc(r); signcalc(r); mem_write(addr, r); }
void PINES_RAM cpu::op_inc_zpx()  { uint16_t addr = (mem_read(pc++) + x) & 0xFF; uint8_t r = mem_read(addr) + 1; zerocalc(r); signcalc(r); mem_write(addr, r); }
void PINES_RAM cpu::op_inc_abs()  { uint16_t addr = mem_read(pc) | (mem_read(pc+1)<<8); pc+=2; uint8_t r = mem_read(addr) + 1; zerocalc(r); signcalc(r); mem_write(addr, r); }
void PINES_RAM cpu::op_inc_absx() { uint16_t addr = (mem_read(pc) | (mem_read(pc+1)<<8)) + x; pc+=2; uint8_t r = mem_read(addr) + 1; zerocalc(r); signcalc(r); mem_write(addr, r); }
void PINES_RAM cpu::op_dec_zp()   { uint16_t addr = mem_read(pc++); uint8_t r = mem_read(addr) - 1; zerocalc(r); signcalc(r); mem_write(addr, r); }
void PINES_RAM cpu::op_dec_zpx()  { uint16_t addr = (mem_read(pc++) + x) & 0xFF; uint8_t r = mem_read(addr) - 1; zerocalc(r); signcalc(r); mem_write(addr, r); }
void PINES_RAM cpu::op_dec_abs()  { uint16_t addr = mem_read(pc) | (mem_read(pc+1)<<8); pc+=2; uint8_t r = mem_read(addr) - 1; zerocalc(r); signcalc(r); mem_write(addr, r); }
void PINES_RAM cpu::op_dec_absx() { uint16_t addr = (mem_read(pc) | (mem_read(pc+1)<<8)) + x; pc+=2; uint8_t r = mem_read(addr) - 1; zerocalc(r); signcalc(r); mem_write(addr, r); }

void PINES_RAM cpu::op_inx() { x++; zerocalc(x); signcalc(x); }
void PINES_RAM cpu::op_iny() { y++; zerocalc(y); signcalc(y); }
void PINES_RAM cpu::op_dex() { x--; zerocalc(x); signcalc(x); }
void PINES_RAM cpu::op_dey() { y--; zerocalc(y); signcalc(y); }

// ============================================================================
// BRANCH (già corretti: +1 se branch preso, +2 se attraversa pagina)
// ============================================================================
// la penalità di branch va nell'accumulatore condiviso
// extraCyclesAccum, sommata al costo base nel loop principale — prima
// veniva sottratta solo a clockgoal6502, senza mai finire in
// realtickcount/totalCycles (vedi commento sopra PAGE_CROSS_PENALTY).
#define DEFINE_BRANCH(NAME, COND)                                  \
void PINES_RAM cpu::op_##NAME() {                                            \
    int8_t offset = (int8_t)mem_read(pc++);                        \
    if (COND) {                                                    \
        uint16_t oldpc = pc;                                       \
        pc += offset;                                              \
        extraCyclesAccum += ((oldpc & 0xFF00) != (pc & 0xFF00)) ? 2 : 1; \
    }                                                               \
}
DEFINE_BRANCH(bpl, (cpustatus & FLAG_SIGN) == 0)
DEFINE_BRANCH(bmi, (cpustatus & FLAG_SIGN) != 0)
DEFINE_BRANCH(bvc, (cpustatus & FLAG_OVERFLOW) == 0)
DEFINE_BRANCH(bvs, (cpustatus & FLAG_OVERFLOW) != 0)
DEFINE_BRANCH(bcc, (cpustatus & FLAG_CARRY) == 0)
DEFINE_BRANCH(bcs, (cpustatus & FLAG_CARRY) != 0)
DEFINE_BRANCH(bne, (cpustatus & FLAG_ZERO) == 0)
DEFINE_BRANCH(beq, (cpustatus & FLAG_ZERO) != 0)
#undef DEFINE_BRANCH

// ============================================================================
// JUMP / CALL
// ============================================================================
void PINES_RAM cpu::op_jmp_abs() { pc = mem_read(pc) | (mem_read(pc+1)<<8); }
void PINES_RAM cpu::op_jmp_ind() {
    uint16_t addr = mem_read(pc) | (mem_read(pc+1)<<8);
    // bug hardware originale del 6502: NON attraversa la pagina (wrap a $xx00)
    uint16_t addr2 = (addr & 0xFF00) | ((addr + 1) & 0x00FF);
    pc = mem_read(addr) | (mem_read(addr2) << 8);
}
void PINES_RAM cpu::op_jsr() {
    uint16_t addr = mem_read(pc) | (mem_read(pc+1)<<8);
    push16(pc + 1);
    pc = addr;
}
void PINES_RAM cpu::op_rts() { pc = pull16() + 1; }
void PINES_RAM cpu::op_rti() { cpustatus = pull8() | FLAG_CONSTANT; pc = pull16(); }
void PINES_RAM cpu::op_brk() {
    pc++;
    push16(pc);
    push8(cpustatus | FLAG_BREAK);
    setinterrupt();
    pc = mem_read(0xFFFE) | (mem_read(0xFFFF) << 8);
}

// ============================================================================
// STACK / TRANSFER / FLAGS
// ============================================================================
void PINES_RAM cpu::op_pha() { push8(a); }
void PINES_RAM cpu::op_pla() { a = pull8(); zerocalc(a); signcalc(a); }
void PINES_RAM cpu::op_php() { push8(cpustatus | FLAG_BREAK); }
void PINES_RAM cpu::op_plp() { cpustatus = pull8() | FLAG_CONSTANT; }

void PINES_RAM cpu::op_tax() { x = a; zerocalc(x); signcalc(x); }
void PINES_RAM cpu::op_txa() { a = x; zerocalc(a); signcalc(a); }
void PINES_RAM cpu::op_tay() { y = a; zerocalc(y); signcalc(y); }
void PINES_RAM cpu::op_tya() { a = y; zerocalc(a); signcalc(a); }
void PINES_RAM cpu::op_tsx() { x = sp; zerocalc(x); signcalc(x); }
void PINES_RAM cpu::op_txs() { sp = x; }

void PINES_RAM cpu::op_clc() { clearcarry(); }
void PINES_RAM cpu::op_sec() { setcarry(); }
void PINES_RAM cpu::op_cli() { clearinterrupt(); }
void PINES_RAM cpu::op_sei() { setinterrupt(); }
void PINES_RAM cpu::op_clv() { clearoverflow(); }
void PINES_RAM cpu::op_cld() { cleardecimal(); }   // su NES inerte per l'ALU, ma il bit si setta/pulisce comunque
void PINES_RAM cpu::op_sed() { setdecimal(); }
void PINES_RAM cpu::op_nop() {}

void PINES_RAM cpu::op_nop_imm()  { pc++; }
void PINES_RAM cpu::op_nop_zp()   { uint16_t addr = mem_read(pc++); (void)mem_read(addr); }
void PINES_RAM cpu::op_nop_zpx()  { uint16_t addr = (mem_read(pc++) + x) & 0xFF; (void)mem_read(addr); }
void PINES_RAM cpu::op_nop_abs()  { uint16_t addr = mem_read(pc) | (mem_read(pc+1)<<8); pc+=2; (void)mem_read(addr); }
void PINES_RAM cpu::op_nop_absx() {
    uint16_t base = mem_read(pc) | (mem_read(pc+1)<<8); pc+=2;
    uint16_t addr = base + x;
    PAGE_CROSS_PENALTY(base, addr);
    (void)mem_read(addr);
}

void PINES_RAM cpu::op_jam() {
    // JAM reale: il 6502 si blocca e ripete all'infinito lo stesso fetch.
    // Decrementiamo pc per "ri-eseguire" lo stesso opcode al prossimo giro.
    pc--;
}

// ============================================================================
// ILLEGAL "COMBO" OPCODES (RMW + logica, nessuna penalità pagina: costo fisso)
// ============================================================================
void PINES_RAM cpu::op_slo_zp()  { uint16_t addr = mem_read(pc++); uint16_t r = mem_read(addr) << 1; carrycalc(r); mem_write(addr, r & 0xFF); a |= r & 0xFF; zerocalc(a); signcalc(a); }
void PINES_RAM cpu::op_slo_zpx() { uint16_t addr = (mem_read(pc++) + x) & 0xFF; uint16_t r = mem_read(addr) << 1; carrycalc(r); mem_write(addr, r & 0xFF); a |= r & 0xFF; zerocalc(a); signcalc(a); }
void PINES_RAM cpu::op_slo_abs() { uint16_t addr = mem_read(pc) | (mem_read(pc+1)<<8); pc+=2; uint16_t r = mem_read(addr) << 1; carrycalc(r); mem_write(addr, r & 0xFF); a |= r & 0xFF; zerocalc(a); signcalc(a); }
void PINES_RAM cpu::op_slo_absx(){ uint16_t addr = (mem_read(pc) | (mem_read(pc+1)<<8)) + x; pc+=2; uint16_t r = mem_read(addr) << 1; carrycalc(r); mem_write(addr, r & 0xFF); a |= r & 0xFF; zerocalc(a); signcalc(a); }
void PINES_RAM cpu::op_slo_absy(){ uint16_t addr = (mem_read(pc) | (mem_read(pc+1)<<8)) + y; pc+=2; uint16_t r = mem_read(addr) << 1; carrycalc(r); mem_write(addr, r & 0xFF); a |= r & 0xFF; zerocalc(a); signcalc(a); }
void PINES_RAM cpu::op_slo_indx(){ uint16_t zp=(mem_read(pc++)+x)&0xFF; uint16_t addr = mem_read(zp) | (mem_read((zp+1)&0xFF)<<8); uint16_t r = mem_read(addr) << 1; carrycalc(r); mem_write(addr, r & 0xFF); a |= r & 0xFF; zerocalc(a); signcalc(a); }
void PINES_RAM cpu::op_slo_indy(){ uint16_t zp=mem_read(pc++); uint16_t addr = (mem_read(zp) | (mem_read((zp+1)&0xFF)<<8)) + y; uint16_t r = mem_read(addr) << 1; carrycalc(r); mem_write(addr, r & 0xFF); a |= r & 0xFF; zerocalc(a); signcalc(a); }

void PINES_RAM cpu::op_rla_zp()  { uint16_t addr = mem_read(pc++); uint16_t r = (mem_read(addr) << 1) | (cpustatus & FLAG_CARRY); carrycalc(r); mem_write(addr, r & 0xFF); a &= r & 0xFF; zerocalc(a); signcalc(a); }
void PINES_RAM cpu::op_rla_zpx() { uint16_t addr = (mem_read(pc++) + x) & 0xFF; uint16_t r = (mem_read(addr) << 1) | (cpustatus & FLAG_CARRY); carrycalc(r); mem_write(addr, r & 0xFF); a &= r & 0xFF; zerocalc(a); signcalc(a); }
void PINES_RAM cpu::op_rla_abs() { uint16_t addr = mem_read(pc) | (mem_read(pc+1)<<8); pc+=2; uint16_t r = (mem_read(addr) << 1) | (cpustatus & FLAG_CARRY); carrycalc(r); mem_write(addr, r & 0xFF); a &= r & 0xFF; zerocalc(a); signcalc(a); }
void PINES_RAM cpu::op_rla_absx(){ uint16_t addr = (mem_read(pc) | (mem_read(pc+1)<<8)) + x; pc+=2; uint16_t r = (mem_read(addr) << 1) | (cpustatus & FLAG_CARRY); carrycalc(r); mem_write(addr, r & 0xFF); a &= r & 0xFF; zerocalc(a); signcalc(a); }
void PINES_RAM cpu::op_rla_absy(){ uint16_t addr = (mem_read(pc) | (mem_read(pc+1)<<8)) + y; pc+=2; uint16_t r = (mem_read(addr) << 1) | (cpustatus & FLAG_CARRY); carrycalc(r); mem_write(addr, r & 0xFF); a &= r & 0xFF; zerocalc(a); signcalc(a); }
void PINES_RAM cpu::op_rla_indx(){ uint16_t zp=(mem_read(pc++)+x)&0xFF; uint16_t addr = mem_read(zp) | (mem_read((zp+1)&0xFF)<<8); uint16_t r = (mem_read(addr) << 1) | (cpustatus & FLAG_CARRY); carrycalc(r); mem_write(addr, r & 0xFF); a &= r & 0xFF; zerocalc(a); signcalc(a); }
void PINES_RAM cpu::op_rla_indy(){ uint16_t zp=mem_read(pc++); uint16_t addr = (mem_read(zp) | (mem_read((zp+1)&0xFF)<<8)) + y; uint16_t r = (mem_read(addr) << 1) | (cpustatus & FLAG_CARRY); carrycalc(r); mem_write(addr, r & 0xFF); a &= r & 0xFF; zerocalc(a); signcalc(a); }

void PINES_RAM cpu::op_sre_zp()  { uint16_t addr = mem_read(pc++); uint8_t v = mem_read(addr); if (v&1) setcarry(); else clearcarry(); uint8_t r = v>>1; mem_write(addr, r); a ^= r; zerocalc(a); signcalc(a); }
void PINES_RAM cpu::op_sre_zpx() { uint16_t addr = (mem_read(pc++) + x) & 0xFF; uint8_t v = mem_read(addr); if (v&1) setcarry(); else clearcarry(); uint8_t r = v>>1; mem_write(addr, r); a ^= r; zerocalc(a); signcalc(a); }
void PINES_RAM cpu::op_sre_abs() { uint16_t addr = mem_read(pc) | (mem_read(pc+1)<<8); pc+=2; uint8_t v = mem_read(addr); if (v&1) setcarry(); else clearcarry(); uint8_t r = v>>1; mem_write(addr, r); a ^= r; zerocalc(a); signcalc(a); }
void PINES_RAM cpu::op_sre_absx(){ uint16_t addr = (mem_read(pc) | (mem_read(pc+1)<<8)) + x; pc+=2; uint8_t v = mem_read(addr); if (v&1) setcarry(); else clearcarry(); uint8_t r = v>>1; mem_write(addr, r); a ^= r; zerocalc(a); signcalc(a); }
void PINES_RAM cpu::op_sre_absy(){ uint16_t addr = (mem_read(pc) | (mem_read(pc+1)<<8)) + y; pc+=2; uint8_t v = mem_read(addr); if (v&1) setcarry(); else clearcarry(); uint8_t r = v>>1; mem_write(addr, r); a ^= r; zerocalc(a); signcalc(a); }
void PINES_RAM cpu::op_sre_indx(){ uint16_t zp=(mem_read(pc++)+x)&0xFF; uint16_t addr = mem_read(zp) | (mem_read((zp+1)&0xFF)<<8); uint8_t v = mem_read(addr); if (v&1) setcarry(); else clearcarry(); uint8_t r = v>>1; mem_write(addr, r); a ^= r; zerocalc(a); signcalc(a); }
void PINES_RAM cpu::op_sre_indy(){ uint16_t zp=mem_read(pc++); uint16_t addr = (mem_read(zp) | (mem_read((zp+1)&0xFF)<<8)) + y; uint8_t v = mem_read(addr); if (v&1) setcarry(); else clearcarry(); uint8_t r = v>>1; mem_write(addr, r); a ^= r; zerocalc(a); signcalc(a); }

void PINES_RAM cpu::op_rra_zp()  { uint16_t addr = mem_read(pc++); uint8_t v = mem_read(addr); uint8_t r = (v>>1)|((cpustatus&FLAG_CARRY)<<7); if (v&1) setcarry(); else clearcarry(); mem_write(addr, r); do_adc(a, cpustatus, r); }
void PINES_RAM cpu::op_rra_zpx() { uint16_t addr = (mem_read(pc++) + x) & 0xFF; uint8_t v = mem_read(addr); uint8_t r = (v>>1)|((cpustatus&FLAG_CARRY)<<7); if (v&1) setcarry(); else clearcarry(); mem_write(addr, r); do_adc(a, cpustatus, r); }
void PINES_RAM cpu::op_rra_abs() { uint16_t addr = mem_read(pc) | (mem_read(pc+1)<<8); pc+=2; uint8_t v = mem_read(addr); uint8_t r = (v>>1)|((cpustatus&FLAG_CARRY)<<7); if (v&1) setcarry(); else clearcarry(); mem_write(addr, r); do_adc(a, cpustatus, r); }
void PINES_RAM cpu::op_rra_absx(){ uint16_t addr = (mem_read(pc) | (mem_read(pc+1)<<8)) + x; pc+=2; uint8_t v = mem_read(addr); uint8_t r = (v>>1)|((cpustatus&FLAG_CARRY)<<7); if (v&1) setcarry(); else clearcarry(); mem_write(addr, r); do_adc(a, cpustatus, r); }
void PINES_RAM cpu::op_rra_absy(){ uint16_t addr = (mem_read(pc) | (mem_read(pc+1)<<8)) + y; pc+=2; uint8_t v = mem_read(addr); uint8_t r = (v>>1)|((cpustatus&FLAG_CARRY)<<7); if (v&1) setcarry(); else clearcarry(); mem_write(addr, r); do_adc(a, cpustatus, r); }
void PINES_RAM cpu::op_rra_indx(){ uint16_t zp=(mem_read(pc++)+x)&0xFF; uint16_t addr = mem_read(zp) | (mem_read((zp+1)&0xFF)<<8); uint8_t v = mem_read(addr); uint8_t r = (v>>1)|((cpustatus&FLAG_CARRY)<<7); if (v&1) setcarry(); else clearcarry(); mem_write(addr, r); do_adc(a, cpustatus, r); }
void PINES_RAM cpu::op_rra_indy(){ uint16_t zp=mem_read(pc++); uint16_t addr = (mem_read(zp) | (mem_read((zp+1)&0xFF)<<8)) + y; uint8_t v = mem_read(addr); uint8_t r = (v>>1)|((cpustatus&FLAG_CARRY)<<7); if (v&1) setcarry(); else clearcarry(); mem_write(addr, r); do_adc(a, cpustatus, r); }

void PINES_RAM cpu::op_lax_zp()   { uint16_t addr = mem_read(pc++); a = x = mem_read(addr); zerocalc(a); signcalc(a); }
void PINES_RAM cpu::op_lax_zpy()  { uint16_t addr = (mem_read(pc++) + y) & 0xFF; a = x = mem_read(addr); zerocalc(a); signcalc(a); }
void PINES_RAM cpu::op_lax_abs()  { uint16_t addr = mem_read(pc) | (mem_read(pc+1)<<8); pc+=2; a = x = mem_read(addr); zerocalc(a); signcalc(a); }
void PINES_RAM cpu::op_lax_absy() {
    uint16_t base = mem_read(pc) | (mem_read(pc+1)<<8); pc+=2;
    uint16_t addr = base + y; PAGE_CROSS_PENALTY(base, addr);
    a = x = mem_read(addr); zerocalc(a); signcalc(a);
}
void PINES_RAM cpu::op_lax_indx() { uint16_t zp=(mem_read(pc++)+x)&0xFF; uint16_t addr = mem_read(zp) | (mem_read((zp+1)&0xFF)<<8); a = x = mem_read(addr); zerocalc(a); signcalc(a); }
void PINES_RAM cpu::op_lax_indy() {
    uint16_t zp = mem_read(pc++);
    uint16_t base = mem_read(zp) | (mem_read((zp+1)&0xFF)<<8);
    uint16_t addr = base + y; PAGE_CROSS_PENALTY(base, addr);
    a = x = mem_read(addr); zerocalc(a); signcalc(a);
}

void PINES_RAM cpu::op_sax_zp()   { uint16_t addr = mem_read(pc++); mem_write(addr, a & x); }
void PINES_RAM cpu::op_sax_zpy()  { uint16_t addr = (mem_read(pc++) + y) & 0xFF; mem_write(addr, a & x); }
void PINES_RAM cpu::op_sax_abs()  { uint16_t addr = mem_read(pc) | (mem_read(pc+1)<<8); pc+=2; mem_write(addr, a & x); }
void PINES_RAM cpu::op_sax_indx() { uint16_t zp=(mem_read(pc++)+x)&0xFF; uint16_t addr = mem_read(zp) | (mem_read((zp+1)&0xFF)<<8); mem_write(addr, a & x); }

void PINES_RAM cpu::op_dcp_zp()  { uint16_t addr = mem_read(pc++); uint8_t v = mem_read(addr) - 1; mem_write(addr, v); do_cmp(a, v, cpustatus); }
void PINES_RAM cpu::op_dcp_zpx() { uint16_t addr = (mem_read(pc++) + x) & 0xFF; uint8_t v = mem_read(addr) - 1; mem_write(addr, v); do_cmp(a, v, cpustatus); }
void PINES_RAM cpu::op_dcp_abs() { uint16_t addr = mem_read(pc) | (mem_read(pc+1)<<8); pc+=2; uint8_t v = mem_read(addr) - 1; mem_write(addr, v); do_cmp(a, v, cpustatus); }
void PINES_RAM cpu::op_dcp_absx(){ uint16_t addr = (mem_read(pc) | (mem_read(pc+1)<<8)) + x; pc+=2; uint8_t v = mem_read(addr) - 1; mem_write(addr, v); do_cmp(a, v, cpustatus); }
void PINES_RAM cpu::op_dcp_absy(){ uint16_t addr = (mem_read(pc) | (mem_read(pc+1)<<8)) + y; pc+=2; uint8_t v = mem_read(addr) - 1; mem_write(addr, v); do_cmp(a, v, cpustatus); }
void PINES_RAM cpu::op_dcp_indx(){ uint16_t zp=(mem_read(pc++)+x)&0xFF; uint16_t addr = mem_read(zp) | (mem_read((zp+1)&0xFF)<<8); uint8_t v = mem_read(addr) - 1; mem_write(addr, v); do_cmp(a, v, cpustatus); }
void PINES_RAM cpu::op_dcp_indy(){ uint16_t zp=mem_read(pc++); uint16_t addr = (mem_read(zp) | (mem_read((zp+1)&0xFF)<<8)) + y; uint8_t v = mem_read(addr) - 1; mem_write(addr, v); do_cmp(a, v, cpustatus); }

void PINES_RAM cpu::op_isc_zp()  { uint16_t addr = mem_read(pc++); uint8_t v = mem_read(addr) + 1; mem_write(addr, v); do_sbc(a, cpustatus, v); }
void PINES_RAM cpu::op_isc_zpx() { uint16_t addr = (mem_read(pc++) + x) & 0xFF; uint8_t v = mem_read(addr) + 1; mem_write(addr, v); do_sbc(a, cpustatus, v); }
void PINES_RAM cpu::op_isc_abs() { uint16_t addr = mem_read(pc) | (mem_read(pc+1)<<8); pc+=2; uint8_t v = mem_read(addr) + 1; mem_write(addr, v); do_sbc(a, cpustatus, v); }
void PINES_RAM cpu::op_isc_absx(){ uint16_t addr = (mem_read(pc) | (mem_read(pc+1)<<8)) + x; pc+=2; uint8_t v = mem_read(addr) + 1; mem_write(addr, v); do_sbc(a, cpustatus, v); }
void PINES_RAM cpu::op_isc_absy(){ uint16_t addr = (mem_read(pc) | (mem_read(pc+1)<<8)) + y; pc+=2; uint8_t v = mem_read(addr) + 1; mem_write(addr, v); do_sbc(a, cpustatus, v); }
void PINES_RAM cpu::op_isc_indx(){ uint16_t zp=(mem_read(pc++)+x)&0xFF; uint16_t addr = mem_read(zp) | (mem_read((zp+1)&0xFF)<<8); uint8_t v = mem_read(addr) + 1; mem_write(addr, v); do_sbc(a, cpustatus, v); }
void PINES_RAM cpu::op_isc_indy(){ uint16_t zp=mem_read(pc++); uint16_t addr = (mem_read(zp) | (mem_read((zp+1)&0xFF)<<8)) + y; uint8_t v = mem_read(addr) + 1; mem_write(addr, v); do_sbc(a, cpustatus, v); }

void PINES_RAM cpu::op_anc_imm() { a &= mem_read(pc++); zerocalc(a); signcalc(a); if (cpustatus & FLAG_SIGN) setcarry(); else clearcarry(); }
void PINES_RAM cpu::op_arr_imm() {
    uint8_t value = mem_read(pc++);
    a &= value;
    a = (a >> 1) | ((cpustatus & FLAG_CARRY) << 7);
    zerocalc(a); signcalc(a);
    if ((a & 0x40) ^ ((a & 0x20) << 1)) setoverflow(); else clearoverflow();
    if (a & 0x40) setcarry(); else clearcarry();
}
void PINES_RAM cpu::op_alr_imm() { uint8_t value = mem_read(pc++); a &= value; if (a & 1) setcarry(); else clearcarry(); a >>= 1; zerocalc(a); clearsign(); }
void PINES_RAM cpu::op_axs_imm() {
    uint8_t value = mem_read(pc++);
    uint16_t result = (a & x) - value;
    x = (uint8_t)result;
    if (result < 0x100) setcarry(); else clearcarry();
    zerocalc(x); signcalc(x);
}
void PINES_RAM cpu::op_shx_absy() { uint16_t addr = mem_read(pc) | (mem_read(pc+1)<<8); pc+=2; uint16_t hi=((addr+y)&0xFF00); uint8_t val = x & ((addr>>8)+1); mem_write((addr&0x00FF)|hi, val); }
void PINES_RAM cpu::op_shy_absx() { uint16_t addr = mem_read(pc) | (mem_read(pc+1)<<8); pc+=2; uint16_t hi=((addr+x)&0xFF00); uint8_t val = y & ((addr>>8)+1); mem_write((addr&0x00FF)|hi, val); }
void PINES_RAM cpu::op_las_absy() { uint16_t addr = (mem_read(pc) | (mem_read(pc+1)<<8)) + y; pc+=2; uint8_t value = mem_read(addr); a = x = sp = (sp & value); zerocalc(a); signcalc(a); }
void PINES_RAM cpu::op_tas_absy() { uint16_t addr = mem_read(pc) | (mem_read(pc+1)<<8); pc+=2; sp = a & x; uint16_t hi=((addr+y)&0xFF00); uint8_t val = sp & ((addr>>8)+1); mem_write((addr&0x00FF)|hi, val); }
void PINES_RAM cpu::op_ahx_absy() { uint16_t addr = (mem_read(pc) | (mem_read(pc+1)<<8)) + y; pc+=2; uint8_t val = a & x & ((addr>>8)+1); mem_write(addr, val); }
void PINES_RAM cpu::op_ahx_indy() { uint16_t zp=mem_read(pc++); uint16_t addr = (mem_read(zp) | (mem_read((zp+1)&0xFF)<<8)) + y; uint8_t val = a & x & ((addr>>8)+1); mem_write(addr, val); }
void PINES_RAM cpu::op_xaa_imm()  { uint8_t value = mem_read(pc++); x = a; a &= value; zerocalc(a); signcalc(a); }
void PINES_RAM cpu::op_lxa_imm()  { uint8_t value = mem_read(pc++); a = x = value; zerocalc(a); signcalc(a); }
