#pragma once
// Minimal RX compatibility header for the extracted upstream translation units.
#define _LINUX_
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <liquid/liquid.h>
#include "frameformat.h"
#include "symboltracker.h"
#define CRC16RX 1
extern int bitsPerSymbol, constellationSize, speed;
extern uint64_t rx_header_candidates, rx_fec_failures, rx_crc_failures;
void init_packer();
uint8_t* unpack_data(uint8_t*, int);
uint8_t* RX_Scramble(uint8_t*, int);
int cfec_Reconstruct(uint8_t*, uint8_t*);
uint16_t Crc16_messagecalc(int, uint8_t*, int);
void convertBytesToSyms_BPSK(uint8_t*, uint8_t*, int);
void convertBytesToSyms_QPSK(uint8_t*, uint8_t*, int);
void convertBytesToSyms_8PSK(uint8_t*, uint8_t*, int);
void rotateBPSKsyms(uint8_t*, uint8_t*, int);
void rotateQPSKsyms(uint8_t*, uint8_t*, int);
void rotate8APSKsyms(uint8_t*, uint8_t*, int);
uint8_t* rotateBackBPSK(uint8_t*, int, int);
uint8_t* rotateBackQPSK(uint8_t*, int, int);
uint8_t* rotateBack8APSK(uint8_t*, int, int);
uint8_t* convertBPSKSymToBytes(uint8_t*);
uint8_t* convertQPSKSymToBytes(uint8_t*);
uint8_t* convert8PSKSymToBytes(uint8_t*, int);
SYMTRACK* km_symtrack_cccf_create(int, unsigned int, unsigned int, float, int);
void km_symtrack_cccf_reset(SYMTRACK*, int);
void km_symtrack_cccf_set_bandwidth(SYMTRACK*, float);
void km_symtrack_execute(SYMTRACK*, liquid_float_complex, liquid_float_complex*, unsigned int*, unsigned int*);
void km_symtrack_cccf_destroy(SYMTRACK*);
