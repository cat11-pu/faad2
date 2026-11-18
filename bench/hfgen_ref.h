#ifndef HFGEN_REF_H
#define HFGEN_REF_H

#include "common.h"
#include "structs.h"
#include "sbr_dec.h"

#ifdef SBR_DEC

void hf_generation_ref(sbr_info *sbr, qmf_t Xlow[MAX_NTSRHFG][64],
                       qmf_t Xhigh[MAX_NTSRHFG][64]
#ifdef SBR_LOW_POWER
                       ,real_t *deg
#endif
                       ,uint8_t ch);

#endif

#endif
