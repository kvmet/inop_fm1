/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* DIGITAL (engine 1, four-operator FM): retired, and its sounds converted to FM6.
 *
 * "The DIGITAL engine has been replaced by FM6 (Dexed-based)" (1.0). Its DSP stays in the tree (eng_digital.c)
 * and builds with FELUCCA_FM4=1 (core.h; off by default, as FELUCCA_SLICE), so it can come back. Engine index 1
 * stays reserved: the stores name engines by index and the table is append-only. Without FELUCCA_FM4 engine 1 is
 * ENG_FM4_GONE below: no presets, never offered (PRESETS, the EDIT layer, the editor's lists skip it: engines.c
 * eng_ok), and a DIGITAL sound that arrives anyway -- a project (FUN1..FUN8), a user preset, the editor's PRESET /
 * SET G_ENGSEL / UP_PUT, a full backup, a stored DIGITAL preset number (TRK_DEF of old projects, favourites) --
 * becomes an FM6 sound with a patch of its own, made by fm4_convert from its DIGITAL values:
 *
 *   DIGITAL                        FM6 patch (155-byte voice; operator numbers 1..6 as the six-operator charts)
 *   ALG 1  4>3>2>1                 ALG 1   6>5>4>3        op 4 -> 6, 3 -> 5, 2 -> 4, 1 -> 3   (2>1 silent)
 *   ALG 2  (3+4)>2>1               ALG 14  (6+5)>4>3      op 4 -> 6, 3 -> 5, 2 -> 4, 1 -> 3   (2>1 silent)
 *   ALG 3  3>2, (2+4)>1            ALG 8   6>5, (5+4)>3   op 4 -> 4 (FB), 3 -> 6, 2 -> 5, 1 -> 3
 *   ALG 4  4>3, (2+3)>1            ALG 7   6>5, (5+4)>3   op 4 -> 6, 3 -> 5, 2 -> 4, 1 -> 3
 *   ALG 5  2>1, 4>3, (1+3)         ALG 5   2>1, 6>5 (4>3) op 4 -> 6, 3 -> 5, 2 -> 2, 1 -> 1   (4>3 silent)
 *   ALG 6  4>(1, 2, 3), (1+2+3)    ALG 22  6>(3, 4, 5)    op 4 -> 6, 3 -> 5, 2 -> 4, 1 -> 3   (2>1 silent)
 *   ALG 7  4>3, (1+2+3)            ALG 29  1, 2, 6>5      op 4 -> 6, 3 -> 5, 2 -> 2, 1 -> 1   (4>3 silent)
 *   ALG 8  (1+2+3+4)               ALG 32  1, 2, 3, 6     op 4 -> 6, 3 -> 3, 2 -> 2, 1 -> 1   (4, 5 silent)
 *   (DIGITAL's op 4 always lands on the operator that has the feedback; the two left over: level 0, rates 99)
 *   R2 R3 R4 (.5 1 2 .. 12 14 16)  coarse of the ops of 2, 3, 4 (0 = .5, n = n), op 1 coarse 1; fine 0, detune 0
 *   INDEX x the op's LEVEL         a modulator's output level: DIGITAL's deviation is 2 cycles x INDEX / 127 (/ 2
 *                                  where two are summed into one input), FM6's 2 cycles at level 99, -0.75 dB a step
 *   MODDEC                         a modulator's EG: L1 99, R2 so that it falls to INDEX / 4 (-12 dB, where
 *                                  MODDEC's exponential lands) over the MODDEC time, L3 = L2; L4 = L3 with
 *                                  R4 = R2 (the index keeps falling after the key, as DIGITAL's does)
 *   ENV -> FLT (P_ED_FLT)          added to the index at the envelope's peak and x SUS at the sustain (DIGITAL
 *                                  adds the master envelope x FLT to the index), the attack from ATK
 *   FDBK                           FB 0..7 of the operator op 4 became: 8 + log2 of DIGITAL's feedback deviation
 *                                  (FDBK x 0.0158 cycles of op 4's sine, whatever its level) over that operator's
 *                                  output at the sustain, rounded. From FDBK 20 (FM4_FB_NOISE: DIGITAL's op 4 turns
 *                                  to noise) the operator is held up by at most 18 dB so that FM6's gets there too
 *   op ATK DEC SUS REL LEVEL       (P_FM1_ATK .. P_FM4_LEVEL, the OP ENV pages) the op's EG on top of the above:
 *                                  attack the longer one, decay towards the product of the sustains over the
 *                                  longer decay, release in parallel (1 / (1 / a + 1 / b)), REL 0 a cut; LEVEL
 *                                  the output level. The flat default (0 0 127 0) changes nothing (as DIGITAL)
 *   master ENV (ATK DEC SUS REL)   kept as the track's ENV (FM6's own envelopes are the voice's amplitude: the
 *                                  track ADSR does nothing there), and folded into the carriers' EGs: R1 from ATK,
 *                                  R2 / L2 from DEC / SUS, R4 from REL, L4 0
 *   times                          DIGITAL's 1 ms .. 10 s (TIME_MS_X10): attack linear, decay and release -40 dB at
 *                                  the time; the FM6 rate the nearest in samples per 6 dB (msfa's step per block,
 *                                  rate scaling 0; its attack: 1.46 x that)
 *   carriers' level                DIGITAL's output / 1 .. / 4 (summed carriers), LEVEL; FM6 renders one carrier
 *                                  at level 99 at half DIGITAL's full scale (eng_fm6.c), so a lone carrier stays
 *                                  6 dB lower (level 99 is the top); 3 steps lower for the velocity below
 *   velocity                       carriers' sensitivity 3 (DIGITAL's output is linear in velocity: -2.4 / -6 / -12
 *                                  dB at 96 / 64 / 32; sensitivity 3: -2.6 / -6.8 / -12.4 from 127), modulators' 1
 *                                  (DIGITAL adds (velocity - 96) / 4 to the index)
 *   the rest                       pitch EG flat (50), LFO off (depths 0), transpose 0 (24), osc key sync, name
 *                                  the DIGITAL preset's when the values are one, else "4OP ALG n"
 *   EDIT values after              FM6's macros neutral (ALG PAT, FB MLVL MRAT MEG VMOD DTUN 0), PTCH the factory
 *                                  patch of the nearest DIGITAL preset (E.PIANO -> TINE EP, BELL, BASS -> FM BASS,
 *                                  BRASS, ORGAN, PAD, MARIMBA, FUNK KEY -> PLUCK): what reloads when the sound is
 *                                  saved as a user preset again (a user preset keeps PTCH, not the patch); the
 *                                  OP ENV values their defaults (inert since; ids 61..80 stay, no id moves)
 * Not carried: the accent's index boost (velocity > 110), LFO / matrix amounts keep their values (FM6 reads FLT
 * as MLVL, SHP as the feedback). fm4_test.c renders DIGITAL (FELUCCA_FM4=1) against the conversion. */

/* ------------------------------------------------- DIGITAL's tables --- */
static const char *const N_FMALG[] = {"1", "2", "3", "4", "5", "6", "7", "8"};
static const char *const N_RATIO[] = {".5", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "14", "16"};

/* the factory presets (the stores' preset numbers of engine 1 index this table) */
static const preset_t DIGITAL_PRESETS[] = {
    /* ALG R2 R3 R4 INDEX MODDEC FDBK - */
    {"E.PIANO", {4, 1, 1, 14, 70, 55, 0, 0}, {0, 80, 40, 60}, 0, 0, FX(0, 50, 25, 35), TAG(PT_KEYS)},
    {"BELL", {4, 8, 1, 4, 90, 70, 0, 0}, {0, 95, 0, 85}, 0, 0, FX(0, 0, 30, 70), TAG(PT_KEYS)},
    {"BASS", {0, 1, 1, 1, 48, 40, 8, 0}, {0, 60, 70, 25}, 0, 1, FX(0, 0, 10, 10), TAG(PT_BASS)},
    {"BRASS", {0, 1, 1, 1, 70, 40, 20, 0}, {40, 70, 100, 40}, 20, 0, FX(0, 20, 20, 40), TAG(PT_LEAD)},
    {"ORGAN", {7, 2, 3, 4, 0, 0, 0, 0}, {2, 60, 127, 20}, 0, 0, FX(10, 40, 0, 30), TAG(PT_KEYS)},
    {"PAD", {5, 2, 1, 3, 40, 90, 10, 0}, {80, 90, 110, 95}, 0, 0, FX(0, 60, 30, 70), TAG(PT_PAD)},
    {"MARIMBA", {4, 4, 1, 1, 60, 30, 0, 0}, {0, 80, 0, 60}, 0, 0, FX(0, 0, 25, 40), TAG(PT_KEYS)},
    {"FUNK KEY", {3, 1, 3, 5, 90, 25, 20, 0}, {0, 45, 30, 30}, 0, 0, FX(0, 20, 30, 20), TAG(PT_KEYS)},
};
#define FM4_NPRESETS 8u
_Static_assert(NELEM(DIGITAL_PRESETS) == FM4_NPRESETS, "DIGITAL presets");
/* DIGITAL preset k -> the FM6 factory preset (= its PTCH) that covers that sound */
static const uint8_t FM4_TO_FM6[FM4_NPRESETS] = {0, 1, 2, 3, 6, 4, 5, 7};

/* EDIT: the ranges are the stores' (a value of engine 1 is clamped to them before it converts) */
#define FM4_EDIT(l0, l1, l2, l3, l4, l5, l6, f0, f1, n0, n1) {                                              \
        {l0, n0, 0, 7, 0, f0, 0}, {l1, n1, 0, 14, 1, f0, 0}, {l2, n1, 0, 14, 1, f0, 0}, {l3, n1, 0, 14, 1, f0, 0}, \
        {l4, 0, 0, 127, 60, F_PCT, 0}, {l5, 0, 0, 127, 60, f1, 0}, {l6, 0, 0, 127, 0, F_PCT, 0},               \
        {"-", 0, 0, 0, 0, F_INT, 0}}

#if !FELUCCA_FM4
/* engine 1 without the DSP: never selectable, no presets, silent (a track never keeps it: fm4_convert) */
static void fm4_gone_note_on(struct track *t, voice_t *v) { (void)t; (void)v; }
static void fm4_gone_render(struct track *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    (void)t; (void)v; (void)out; (void)n; (void)m;
}
static const engine_t ENG_FM4_GONE = {
    .name = "-",
    .page_title = {"-", "-"},
    .edit = FM4_EDIT("-", "-", "-", "-", "-", "-", "-", F_INT, F_INT, 0, 0),
    .note_on = fm4_gone_note_on,
    .render = fm4_gone_render,
    .knob = {P_E4, P_E5, P_E6, P_REL},
};
#endif

/* ------------------------------------------------------- conversion --- */
