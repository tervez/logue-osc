/*
 * File: fm64.cpp
 *
 * DX7/DX11-series compatible FM oscillator
 * 
 * 2020 (c) Oleg Burdaev
 * mailto: dukesrg@gmail.com
 *
 */

#include "userosc.h"
#include "fixed_mathq.h"
#include "fm64.h"

#define USE_Q31
#ifdef USE_Q31 //use fixed-point math to reduce CPU consumption
//todo: check and fix osc_apiq
  #define USE_Q31_PHASE //a bit less CPU consuming, but looks like have a slight phase drift over time
  #ifdef USE_Q31_PHASE 
//todo: doesn't work as expected
//  #define USE_Q31_PITCH //another bit less CPU consuming
  #endif
//  #define OSC_NOTE_Q
  #define OSC_SIN_Q
  #include "osc_apiq.h"
#endif

#define FEEDBACK_RECIP .0078125f //1/128
#define SCALE_RECIP .01010101f //1/99
#define DX7_MAX_RATE 99
#define DX11_MAX_RATE 31
#define DX7_EG_LEVEL_SCALE_RECIP .01010101f //1/99
#define DX11_EG_LEVEL_SCALE_RECIP .06666667f //1/15
#define DX11_MAX_LEVEL 15
#define DX7_RATE_FACTOR .01f //linear EG rate multiplier
//#define DX11_RATE_FACTOR 0.28125f //9s/32
#define DX11_RATE_FACTOR .03f

#define FREQ_FACTOR .08860606f // (9.772 - 1)/99

//static const dx7_voice_t *voice;
static uint32_t s_bank;
static uint32_t s_voice;
static uint32_t s_opcount;
static const uint8_t *s_algorithm;
static uint8_t s_opi;
static uint8_t s_fixedfreq[DX7_OPERATOR_COUNT];
static uint8_t s_egstage[DX7_OPERATOR_COUNT];
static uint8_t s_transpose;
static uint8_t s_feedback_src;
//static uint8_t s_pegstage;
//static uint8_t s_waveform[DX7_OPERATOR_COUNT];

#ifdef USE_Q31
static q31_t s_egrate[DX7_OPERATOR_COUNT][EG_STAGE_COUNT];
static q31_t s_eglevel[DX7_OPERATOR_COUNT][EG_STAGE_COUNT];
static q31_t s_egval[DX7_OPERATOR_COUNT];
static q31_t s_oplevel[DX7_OPERATOR_COUNT];
static q31_t s_opval[DX7_OPERATOR_COUNT];
static q31_t s_modlevel[DX7_OPERATOR_COUNT];
static q31_t s_feedback;
static q31_t s_feedback_opval[2];
/*
static float s_pegrate[EG_STAGE_COUNT];
static float s_peglevel[EG_STAGE_COUNT];
static float s_pegval[DX7_OPERATOR_COUNT];
*/
#else
static float s_egrate[DX7_OPERATOR_COUNT][EG_STAGE_COUNT];
static float s_eglevel[DX7_OPERATOR_COUNT][EG_STAGE_COUNT];
static float s_egval[DX7_OPERATOR_COUNT];
static float s_oplevel[DX7_OPERATOR_COUNT];
static float s_opval[DX7_OPERATOR_COUNT];
static float s_modlevel[DX7_OPERATOR_COUNT];
static float s_feedback;
static float s_feedback_opval[2];
/*
static q31_t s_pegrate[EG_STAGE_COUNT];
static q31_t s_peglevel[EG_STAGE_COUNT];
static q31_t s_pegval[DX7_OPERATOR_COUNT];
*/
#endif
#ifdef USE_Q31_PITCH
static q31_t s_oppitch[DX7_OPERATOR_COUNT];
#else
static float s_oppitch[DX7_OPERATOR_COUNT];
#endif
#ifdef USE_Q31_PHASE
static q31_t s_phase[DX7_OPERATOR_COUNT];
#else
static float s_phase[DX7_OPERATOR_COUNT];
#endif

void initvoice() {
  if (dx_voices[s_bank][s_voice].dx7.vnam[0]) {
    const dx7_voice_t *voice = &dx_voices[s_bank][s_voice].dx7;
    s_opcount = DX7_OPERATOR_COUNT;
    s_opi = voice->opi;
    s_algorithm = dx7_algorithm[voice->als];
    s_transpose = voice->trnp - TRANSPOSE_CENTER;

#ifdef USE_Q31
    s_feedback = f32_to_q31((0x100 >> (8 - voice->fbl)) * FEEDBACK_RECIP);
    s_feedback_opval[0] = 0;
    s_feedback_opval[1] = 0;
#else
//    s_feedback = (0x100 >> (8 - voice->fbl)) * FEEDBACK_RECIP;
    s_feedback = (0x100 >> (8 - voice->fbl)) * FEEDBACK_RECIP * .5f; // 1/2 to compensate averaged operator output sum
    s_feedback_opval[0] = 0.f;
    s_feedback_opval[1] = 0.f;
#endif
/*
#ifdef USE_Q31
//todo: PEG level precalc & Q31
#else
  float preveglevel = (float)(voice->pl[EG_STAGE_COUNT - 1] - PEG_CENTER) * PEG_SCALE;
  for (uint32_t j = 0; j < EG_STAGE_COUNT; j++) {
    s_pegrate[j] = k_samplerate_recipf * SCALE_RECIP * (s_peglevel[j] - prevlevel) / (RATE_FACTOR * (100 - voice->pr[j]));
    prevlevel = voice->pl[j];
    s_peglevel[j] = voice->pl[j];
#endif
  }
*/
    for (uint32_t i = s_opcount; i--;) {
      if (s_algorithm[i] & ALG_FBK_MASK) {
        s_feedback_src = 0;
        for (uint32_t j = (s_algorithm[i] & (ALG_FBK_MASK - 1)) >> 1; j; j >>= 1, s_feedback_src++);
      }

      s_fixedfreq[i] = voice->op[i].pm;
//      s_waveform[i] = 0;

#ifdef USE_Q31_PHASE
      s_phase[i] = 0;
#else
      s_phase[i] = 0.f;
#endif

      int32_t dl;
#ifdef USE_Q31
//todo: non-linear rates
//todo: reverse rates
      for (uint32_t j = EG_STAGE_COUNT; j--;) {
        dl = voice->op[i].l[j] - voice->op[i].l[j ? (j - 1) : EG_STAGE_COUNT - 1];
        if (dl > 0)
          s_egrate[i][j] = f32_to_q31(k_samplerate_recipf / (DX7_RATE_FACTOR * (DX7_MAX_RATE + 1 - voice->op[i].r[j])));
        else if (dl < 0)
          s_egrate[i][j] = f32_to_q31(- k_samplerate_recipf / (DX7_RATE_FACTOR * (DX7_MAX_RATE + 1 - voice->op[i].r[j])));
        else 
          s_egrate[i][j] = 0;
        s_eglevel[i][j] = f32_to_q31(voice->op[i].l[j] * DX7_EG_LEVEL_SCALE_RECIP);
      }
      s_opval[i] = 0;
      s_oplevel[i] = f32_to_q31(voice->op[i].tl * SCALE_RECIP);
      s_modlevel[i] = f32_to_q31(dx7_modindex(voice->op[i].tl));
#else
      for (uint32_t j = EG_STAGE_COUNT; j--;) {
        dl = voice->op[i].l[j] - voice->op[i].l[j ? (j - 1) : EG_STAGE_COUNT - 1];
        if (dl > 0)
          s_egrate[i][j] = k_samplerate_recipf / (DX7_RATE_FACTOR * (DX7_MAX_RATE + 1 - voice->op[i].r[j]));
        else if (dl < 0)
          s_egrate[i][j] = - k_samplerate_recipf / (DX7_RATE_FACTOR * (DX7_MAX_RATE + 1 - voice->op[i].r[j]));
        else 
          s_egrate[i][j] = 0.f;
        s_eglevel[i][j] = voice->op[i].l[j] * DX7_EG_LEVEL_SCALE_RECIP;
      }
      s_opval[i] = 0.f;
      s_oplevel[i] = voice->op[i].tl * SCALE_RECIP;
      s_modlevel[i] = dx7_modindex(voice->op[i].tl);
#endif
      s_egstage[i] = 0;
      s_egval[i] = s_eglevel[i][EG_STAGE_COUNT - 1];

#ifdef USE_Q31_PITCH
      if (s_fixedfreq[i])
        s_oppitch[i] = f32_to_q31(((voice->op[i].pc == 0 ? 1.f : voice->op[i].pc == 1 ? 10.f : voice->op[i].pc == 2 ? 100.f : 1000.f) * (1.f + voice->op[i].pf * FREQ_FACTOR)) * k_samplerate_recipf);
      else
        s_oppitch[i] = f32_to_q31(((voice->op[i].pc == 0 ? .5f : voice->op[i].pc) * (1.f + voice->op[i].pf * .01f)));
#else
      if (s_fixedfreq[i])
        s_oppitch[i] = ((voice->op[i].pc == 0 ? 1.f : voice->op[i].pc == 1 ? 10.f : voice->op[i].pc == 2 ? 100.f : 1000.f) * (1.f + voice->op[i].pf * FREQ_FACTOR)) * k_samplerate_recipf;
      else
        s_oppitch[i] = ((voice->op[i].pc == 0 ? .5f : voice->op[i].pc) * (1.f + voice->op[i].pf * .01f));
#endif
    }
  } else {
    const dx11_voice_t *voice = &dx_voices[s_bank][s_voice].dx11;
    s_opcount = DX11_OPERATOR_COUNT;
    s_algorithm = dx11_algorithm[voice->alg];
    s_opi = 0;
    s_transpose = voice->trps - TRANSPOSE_CENTER;

#ifdef USE_Q31
    s_feedback = f32_to_q31((1 << (voice->fbl - 7)) * FEEDBACK_RECIP);
#else
    s_feedback = (1 << (voice->fbl - 7)) * FEEDBACK_RECIP;
#endif

    for (uint32_t i = s_opcount; i--;) {
      if (s_algorithm[i] & ALG_FBK_MASK) {
        s_feedback_src = 0;
        for (uint32_t j = (s_algorithm[i] & (ALG_FBK_MASK - 1)) >> 1; j; j >>= 1, s_feedback_src++);
      }

      s_fixedfreq[i] = voice->opadd[i].fixrg;
//      s_waveform[i] =  voice->opadd[i].osw;

#ifdef USE_Q31_PHASE
      s_phase[i] = 0;
#else
      s_phase[i] = 0.f;
#endif

      int32_t dl;
#ifdef USE_Q31
//todo: non-linear rates
//todo: reverse rates
      for (uint32_t j = 0; j < EG_STAGE_COUNT; j++) {
        if (j == (EG_STAGE_COUNT - 2) && s_egrate[i][j] == 0)
          dl = 0;
        else
          dl = (j==0 ? DX11_MAX_LEVEL : j == 1 ? voice->op[i].d1l - DX11_MAX_LEVEL : - voice->op[i].d1l);
        if (dl > 0)
          s_egrate[i][j] = f32_to_q31(k_samplerate_recipf / (DX11_RATE_FACTOR * (DX11_MAX_RATE + 1 - (voice->op[i].r[j] + (voice->op[i].r[j] == 0 && j == (EG_STAGE_COUNT - 1) ? 0 : 1)))));
        else if (dl < 0)
          s_egrate[i][j] = f32_to_q31(- k_samplerate_recipf / (DX11_RATE_FACTOR * (DX11_MAX_RATE + 1 - (voice->op[i].r[j] + (voice->op[i].r[j] == 0 && j == (EG_STAGE_COUNT - 1) ? 0 : 1)))));
        else 
          s_egrate[i][j] = 0;
        s_eglevel[i][j] = f32_to_q31(1.f - (1.f - (j==0 ? 1.f : j == 1 ? voice->op[i].d1l * DX11_EG_LEVEL_SCALE_RECIP : 0.f)) / (1 << (i != s_opcount - 1 ? voice->opadd[i].egsft : 0)));
      }
      s_opval[i] = 0;
      s_oplevel[i] = f32_to_q31(voice->op[i].out * SCALE_RECIP);
      s_modlevel[i] = f32_to_q31(dx7_modindex(voice->op[i].out));
#else
      for (uint32_t j = EG_STAGE_COUNT; j--;) {
        if (j == (EG_STAGE_COUNT - 2) && s_egrate[i][j] == 0)
          dl = 0;
        else
          dl = (j==0 ? DX11_MAX_LEVEL : j == 1 ? voice->op[i].d1l - DX11_MAX_LEVEL : - voice->op[i].d1l);
        if (dl > 0)
          s_egrate[i][j] = k_samplerate_recipf / (DX11_RATE_FACTOR * (DX11_MAX_RATE + 1 - (voice->op[i].r[j] + (voice->op[i].r[j] == 0 && j == (EG_STAGE_COUNT - 1) ? 0 : 1))));
        else if (dl < 0)
          s_egrate[i][j] = - k_samplerate_recipf / (DX11_RATE_FACTOR * (DX11_MAX_RATE + 1 - (voice->op[i].r[j] + (voice->op[i].r[j] == 0 && j == (EG_STAGE_COUNT - 1) ? 0 : 1))));
        else 
          s_egrate[i][j] = 0.f;
        s_eglevel[i][j] = 1.f - (1.f - (j==0 ? 1.f : j == 1 ? voice->op[i].d1l * DX11_EG_LEVEL_SCALE_RECIP : 0.f)) / (1 << (i != s_opcount - 1 ? voice->opadd[i].egsft : 0));
      }
      s_opval[i] = 0.f;
      s_oplevel[i] = voice->op[i].out * SCALE_RECIP;
      s_modlevel[i] = dx11_modindex(voice->op[i].out);
#endif
      s_egstage[i] = 0;
      s_egval[i] = s_eglevel[i][EG_STAGE_COUNT - 1];

#ifdef USE_Q31_PITCH
      if (s_fixedfreq[i])
        s_oppitch[i] = f32_to_q31(((((voice->op[i].f & 0x3C) << 2) + voice->opadd[i].fine + (voice->op[i].f < 4 ? 8 : 0)) << voice->opadd[i].fixrg) * k_samplerate_recipf);
      else
        s_oppitch[i] = f32_to_q31(dx11_ratio_lut[voice->op[i].f]);
#else
      if (s_fixedfreq[i])
        s_oppitch[i] = ((((voice->op[i].f & 0x3C) << 2) + voice->opadd[i].fine + (voice->op[i].f < 4 ? 8 : 0)) << voice->opadd[i].fixrg) * k_samplerate_recipf;
      else
//todo: Fine freq ratio
        s_oppitch[i] = dx11_ratio_lut[voice->op[i].f];
//todo: Waveform
//if (s_waveform[i] & 0x01)
//  s_oppitch[i] *= 2;
#endif
    }
  }
}

void OSC_INIT(__attribute__((unused)) uint32_t platform, __attribute__((unused)) uint32_t api)
{
#ifdef USE_Q31
  osc_api_initq();
#endif
  s_bank = 0;
  s_voice = 0;
  initvoice();
}

void OSC_CYCLE(const user_osc_param_t * const params, int32_t *yn, const uint32_t frames)
{
#ifdef USE_Q31
  q31_t osc_out, modw0;
#else 
  float osc_out, modw0;
#endif

#ifdef USE_Q31_PHASE
#ifdef USE_Q31_PITCH
//todo: PEG level
  q31_t basew0 = f32_to_q31(osc_w0f_for_note((params->pitch >> 8) + s_transpose, params->pitch & 0xFF));
#else
  float basew0 = osc_w0f_for_note((params->pitch >> 8) + s_transpose, params->pitch & 0xFF);
#endif
  q31_t opw0[DX7_OPERATOR_COUNT];
  for (uint32_t i = s_opcount; i--;) {
#ifdef USE_Q31_PITCH
    if (s_fixedfreq[i])
      opw0[i] = s_oppitch[i];
    else
      opw0[i] = q31mul(s_oppitch[i], basew0);
#else
    if (s_fixedfreq[i])
      opw0[i] = f32_to_q31(s_oppitch[i]);
    else
      opw0[i] = f32_to_q31(s_oppitch[i] * basew0);
#endif
  }
#else 
  float basew0 = osc_w0f_for_note((params->pitch >> 8) + s_transpose, params->pitch & 0xFF);
  float opw0[DX7_OPERATOR_COUNT];
  for (uint32_t i = s_opcount; i--;) {
    if (s_fixedfreq[i])
      opw0[i] = s_oppitch[i];
    else
      opw0[i] = s_oppitch[i] * basew0;
  }
#endif

  q31_t * __restrict y = (q31_t *)yn;
  for (uint32_t f = frames; f--; y++) {
#ifdef USE_Q31
    osc_out = 0;
#else
    osc_out = 0.f;
#endif
    for (uint32_t i = 0; i < s_opcount; i++) {
#ifdef USE_Q31
#ifdef USE_Q31_PHASE
      modw0 = s_phase[i];
#else
      modw0 = f32_to_q31(s_phase[i]);
#endif
      if (s_algorithm[i] & ALG_FBK_MASK) {
//        modw0 += q31mul(s_opval[s_feedback_src], s_feedback);
        modw0 += q31mul(q31add(s_feedback_opval[0], s_feedback_opval[1]), s_feedback);
      } else if (s_algorithm[i] & (ALG_FBK_MASK - 1)) {
        if (s_algorithm[i] & ALG_MOD6_MASK) modw0 += q31mul(s_opval[0], s_modlevel[0]);
        if (s_algorithm[i] & ALG_MOD5_MASK) modw0 += q31mul(s_opval[1], s_modlevel[1]);
        if (s_algorithm[i] & ALG_MOD4_MASK) modw0 += q31mul(s_opval[2], s_modlevel[2]);
        if (s_algorithm[i] & ALG_MOD3_MASK) modw0 += q31mul(s_opval[3], s_modlevel[3]);
        if (s_algorithm[i] & ALG_MOD2_MASK) modw0 += q31mul(s_opval[4], s_modlevel[4]);
        if (s_algorithm[i] & ALG_MOD1_MASK) modw0 += q31mul(s_opval[5], s_modlevel[5]);
      }

      s_opval[i] = q31mul(osc_sinq(modw0), s_egval[i]);
      if (s_algorithm[i] & ALG_OUT_MASK)
        osc_out = q31add(osc_out, q31mul(s_opval[i], s_oplevel[i]));
      if (i == s_feedback_src) {
        s_feedback_opval[1] = s_feedback_opval[0];
        s_feedback_opval[0] = s_opval[i] >> 1;
      }
#else
      modw0 = s_phase[i];
      if (s_algorithm[i] & ALG_FBK_MASK) {
//        modw0 += s_opval[s_feedback_src] * s_feedback;
        modw0 += (s_feedback_opval[0] + s_feedback_opval[1]) * s_feedback;
      } else if (s_algorithm[i] & (ALG_FBK_MASK - 1)) {
        if (s_algorithm[i] & ALG_MOD6_MASK) modw0 += s_opval[0] * s_modlevel[0];
        if (s_algorithm[i] & ALG_MOD5_MASK) modw0 += s_opval[1] * s_modlevel[1];
        if (s_algorithm[i] & ALG_MOD4_MASK) modw0 += s_opval[2] * s_modlevel[2];
        if (s_algorithm[i] & ALG_MOD3_MASK) modw0 += s_opval[3] * s_modlevel[3];
        if (s_algorithm[i] & ALG_MOD2_MASK) modw0 += s_opval[4] * s_modlevel[4];
        if (s_algorithm[i] & ALG_MOD1_MASK) modw0 += s_opval[5] * s_modlevel[5];
      }

      s_opval[i] = osc_sinf(modw0) * s_egval[i];
      if (s_algorithm[i] & ALG_OUT_MASK)
        osc_out += s_opval[i] * s_oplevel[i];
      if (i == s_feedback_src) {
        s_feedback_opval[1] = s_feedback_opval[0];
        s_feedback_opval[0] = s_opval[i];
      }
#endif

      s_phase[i] += opw0[i];
#ifndef USE_Q31_PHASE
      s_phase[i] -= (uint32_t)(s_phase[i]);
#endif

//todo: flatten the level/rate arrays and get rid of the excessive indexing
#ifdef USE_Q31
      s_egval[i] = q31add(s_egval[i], s_egrate[i][s_egstage[i]]);
      if (
        (s_egrate[i][s_egstage[i]] > 0 && s_egval[i] >= s_eglevel[i][s_egstage[i]])
        || (s_egrate[i][s_egstage[i]] < 0 && s_egval[i] <= s_eglevel[i][s_egstage[i]])
      ) {
#else
      s_egval[i] += s_egrate[i][s_egstage[i]];
      if (
        (s_egrate[i][s_egstage[i]] > 0.f && s_egval[i] >= s_eglevel[i][s_egstage[i]])
        || (s_egrate[i][s_egstage[i]] < 0.f && s_egval[i] <= s_eglevel[i][s_egstage[i]])
      ) {
#endif
        s_egval[i] = s_eglevel[i][s_egstage[i]];
        if (s_egstage[i] < EG_STAGE_COUNT - 2)
          s_egstage[i]++;
      }

    }
/*
//todo: PEG level
#ifdef USE_Q31
    s_pegval = q31add(s_pegval, s_pegrate[s_pegstage]);
    if (
      (s_pegrate[s_pegstage] > 0 && s_pegval >= s_pegrate[s_pegstage])
      || (s_pegrate[s_pegstage] < 0 && s_pegval <= s_pegrate[s_pegstage])
      || s_pegrate[s_pegstage] == 0
    ) {
#else
    s_pegval += s_pegrate[s_pegstage];
    if (
      (s_pegrate[s_pegstage] > 0.f && s_pegval >= s_pegrate[s_pegstage])
      || (s_pegrate[s_pegstage] < 0.f && s_pegval <= s_pegrate[s_pegstage])
      || s_pegrate[s_pegstage] == 0.f
    ) {
#endif
       s_pegval = s_peglevel[s_pegstage];
       if (s_pegstage < 3)
        s_pegstage++;
    }
*/
#ifdef USE_Q31
    *y = osc_out;
#else
    *y = f32_to_q31(osc_out);
#endif
  }
}

void OSC_NOTEON(__attribute__((unused)) const user_osc_param_t * const params)
{
  for (uint32_t i = s_opcount; i--;) {
    if (s_opi)
#ifdef USE_Q31_PHASE
      s_phase[i] = 0;
#else
      s_phase[i] = 0.f;
#endif
//todo: to reset or not to reset - that is the question (stick with the operator phase init)
#ifdef USE_Q31
    s_opval[i] = 0;
#else
    s_opval[i] = 0.f;
#endif
    s_egstage[i] = 0;
    s_egval[i] = s_eglevel[i][EG_STAGE_COUNT - 1];
  }
/*
  s_pegstage = 0;
  s_egval = s_eglevel[EG_STAGE_COUNT - 1];
*/
}

void OSC_NOTEOFF(__attribute__((unused)) const user_osc_param_t * const params)
{
  for (uint32_t i = s_opcount; i--;) {
    s_egstage[i] = EG_STAGE_COUNT - 1;
 }
}

void OSC_PARAM(uint16_t index, uint16_t value)
{
  switch (index) {
    case k_user_osc_param_shape:
//      s_shape = param_val_to_f32(value);
      value >>= 5;
    case k_user_osc_param_id1:
      if (s_voice != value) {
        s_voice = value;
        initvoice();
      }
      break;
      break;
    case k_user_osc_param_shiftshape:
//      s_shiftshape = param_val_to_f32(value);
      value >>= 8;
    case k_user_osc_param_id2:
      if (s_bank != value) {
        s_bank = value;
        initvoice();
      }
      break;
    case k_user_osc_param_id3:
      break;
    case k_user_osc_param_id4:
      break;
    case k_user_osc_param_id5:
      break;
    case k_user_osc_param_id6:
      break;
    default:
      break;
  }
}