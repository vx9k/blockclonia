/* The whole body as a tagged record (tagged.h) for saves: every field of
 * health and of the heart, vessels and nerves inside it, so a body loaded
 * steps on exactly as the one saved would have. Leaving a world or losing
 * the game to a crash then neither heals nor harms it.
 *
 * Fields used as an index or an enum carry their range. The
 * _Static_asserts below trip when a struct changes size: a new field goes
 * into its table here, or it is silently lost on every load. */
#include "health.h"
#include "tagged.h"

_Static_assert(sizeof(health) == 30544, "a field changed in health: update HEALTH_F (health_tags.c)");
_Static_assert(sizeof(body_part) == 80, "a field changed in body_part: update PART_F");
_Static_assert(sizeof(wound) == 40, "a field changed in wound: update WOUND_F");
_Static_assert(sizeof(defib_state) == 28, "a field changed in defib_state: update DEFIB_F");
_Static_assert(sizeof(health_baseline) == 56, "a field changed in health_baseline: update BASE_F");
_Static_assert(sizeof(circ) == 1968, "a field changed in circ (circulation.h): update CIRC_F");
_Static_assert(sizeof(conduction) == 96, "a field changed in conduction (conduction.h): update COND_F");
_Static_assert(sizeof(autonomic) == 2976, "a field changed in autonomic (autonomic.h): update ANS_F");


static const tag_field PART_F[] = {
    TAG_F32(body_part, integrity),
    TAG_U8R(body_part, fracture, 0, FX_OPEN),
    TAG_U8R(body_part, splinted, 0, 1),
    TAG_U8R(body_part, dislocated, 0, DISLOC_ELBOW),
    TAG_F32(body_part, fracture_heal),
    TAG_F32(body_part, sprain),
    TAG_F32(body_part, internal),
    TAG_F32(body_part, pain),
    TAG_F32(body_part, pain_spike),
    TAG_F32(body_part, skin),
    TAG_F32(body_part, surface),
    TAG_F32(body_part, burn_omega),
    TAG_F32(body_part, burn_area),
    TAG_F32(body_part, burn_age),
    TAG_F32(body_part, cool_time),
    TAG_U8R(body_part, burn, 0, 3),
    TAG_U8R(body_part, burn_cooled, 0, 1),
    TAG_U8R(body_part, frost, 0, FROST_DEEP),
    TAG_F32(body_part, freeze_time),
    TAG_F32(body_part, frost_heal),
    TAG_F32(body_part, crush),
    TAG_F32(body_part, crush_released),
    TAG_F32(body_part, crush_load),
    TAG_F32(body_part, crush_next),
};
static const tag_table PART = TAG_TABLE(PART_F);

static const tag_field WOUND_F[] = {
    TAG_U8R(wound, part, 0, BP_COUNT - 1),
    TAG_U8R(wound, kind, 0, WOUND_FROSTBITE),
    TAG_U8R(wound, bandage, 0, BANDAGE_IMPROVISED),
    TAG_U8R(wound, arterial, 0, 1),
    TAG_F32(wound, depth),
    TAG_F32(wound, bleed0),
    TAG_F32(wound, bleed),
    TAG_F32(wound, clot),
    TAG_F32(wound, contamination),
    TAG_F32(wound, infection),
    TAG_F32(wound, closure),
    TAG_F32(wound, bandage_age),
    TAG_F32(wound, area),
};
static const tag_table WOUND = TAG_TABLE(WOUND_F);

static const tag_field DEFIB_F[] = {
    TAG_I32R(defib_state, phase, DEFIB_OFF, DEFIB_CLEAR),
    TAG_I32R(defib_state, seen, -1, RHYTHM_COUNT - 1),
    TAG_I32R(defib_state, advice, 0, 1),
    TAG_I32R(defib_state, shocks, 0, 2147483647),
    TAG_F32(defib_state, timer),
    TAG_F32(defib_state, joules),
    TAG_F32(defib_state, since_shock),
};
static const tag_table DEFIB = TAG_TABLE(DEFIB_F);

static const tag_field BASE_F[] = {
    TAG_F32(health_baseline, hr_rest),      TAG_F32(health_baseline, map_set),
    TAG_F32(health_baseline, pain_gain),    TAG_F32(health_baseline, clot_gain),
    TAG_F32(health_baseline, metab_gain),   TAG_F32(health_baseline, cold_bias),
    TAG_F32(health_baseline, heart_size),   TAG_F32(health_baseline, art_stiff),
    TAG_F32(health_baseline, blood_per_kg), TAG_F32(health_baseline, baro_gain),
    TAG_F32(health_baseline, venous_tone),  TAG_F32(health_baseline, symp_noise),
    TAG_F32(health_baseline, hr_max),       TAG_I32R(health_baseline, vary, 0, 1),
};
static const tag_table BASE = TAG_TABLE(BASE_F);

static const tag_field CIRC_F[] = {
    TAG_F64(circ, v),
    TAG_F32(circ, p),
    TAG_F32(circ, q_bed),
    TAG_F32(circ, q_cor),
    TAG_F32(circ, q_ao),
    TAG_F32(circ, q_finger),
    TAG_F32(circ, lost),
    TAG_F32(circ, edv),
    TAG_F32(circ, esv),
    TAG_F32(circ, p_ao_max),
    TAG_F32(circ, p_ao_min),
    TAG_F32(circ, cor_dia_frac),
    TAG_F32(circ, beat_len),
    TAG_F32(circ, p_ao_mean),
    TAG_F32(circ, p_ra_mean),
    TAG_F32(circ, beat_q_ao),
    TAG_F32(circ, beat_q_cor),
    TAG_F32(circ, beat_q_bed),
    TAG_F32(circ, pv_v),
    TAG_F32(circ, pv_p),
    TAG_I32R(circ, pv_n, 0, CIRC_PV_N),
    TAG_F32(circ, wave_ao),
    TAG_F32(circ, wave_finger),
    TAG_F64(circ, c),
    TAG_F64(circ, v0),
    TAG_F64(circ, vmax),
    TAG_F64(circ, r_bed0),
    TAG_F64(circ, r_ven),
    TAG_F64(circ, emax0),
    TAG_F64(circ, emin0),
    TAG_F64(circ, hill_peak),
    /* The sub-step count resyncs to the clock when it is off (circ_step),
     * so a bad one costs no more than a skipped step. */
    TAG_I64R(circ, steps, 0, 1e18),
    TAG_F64(circ, beat_t),
    TAG_F64(circ, b_vmax),
    TAG_F64(circ, b_vmin),
    TAG_F64(circ, b_pmax),
    TAG_F64(circ, b_pmin),
    TAG_F64(circ, b_cor),
    TAG_F64(circ, b_cor_dia),
    TAG_F64(circ, b_pao),
    TAG_F64(circ, b_pra),
    TAG_F64(circ, b_qao),
    TAG_F64(circ, b_qbed),
    TAG_I64R(circ, b_n, 0, 1e18),
    TAG_F32(circ, w_v),
    TAG_F32(circ, w_p),
    TAG_I32R(circ, w_n, 0, CIRC_PV_N),
    TAG_I32R(circ, w_i, 0, 2147483647),
};
static const tag_table CIRC = TAG_TABLE(CIRC_F);

static const tag_field COND_F[] = {
    TAG_F64(conduction, t_atria),
    TAG_F64(conduction, t_vent),
    TAG_F64(conduction, prev_vent),
    TAG_F64(conduction, pending_av),
    TAG_F64(conduction, av_free),
    TAG_F64(conduction, next_af),
    TAG_F64(conduction, next_own),
    TAG_F64(conduction, pending_ect),
    TAG_F64(conduction, ect_coupling),
    TAG_F64(conduction, esc_ref),
    TAG_F32R(conduction, sa_phase, 0, 1),
    TAG_I32R(conduction, last_ectopic, 0, 1),
    TAG_I32R(conduction, rhythm, -1, RHYTHM_COUNT - 1),
    TAG_I32(conduction, beats),
};
static const tag_table COND = TAG_TABLE(COND_F);

static const tag_field ANS_F[] = {
    TAG_F32(autonomic, p_cs),       TAG_F32(autonomic, p_prev),
    TAG_F32(autonomic, f_cs),       TAG_F32(autonomic, f_s),
    TAG_F32(autonomic, f_v),        TAG_F32(autonomic, hist_s),
    TAG_F32(autonomic, hist_v),     TAG_I32R(autonomic, hist_pos, 0, AUTO_HIST - 1),
    TAG_I32R(autonomic, hist_n, 0, AUTO_HIST), TAG_F32(autonomic, eff_emax),
    TAG_F32(autonomic, eff_r),      TAG_F32(autonomic, eff_v0),
    TAG_F32(autonomic, eff_ts),     TAG_F32(autonomic, eff_tv),
    TAG_F32(autonomic, ls0),        TAG_F32(autonomic, fv0),
    TAG_F32(autonomic, t0),         TAG_F32(autonomic, command),
    TAG_F32(autonomic, vagal_off),  TAG_F32(autonomic, ex_drive),
    TAG_F32(autonomic, metab),      TAG_F32(autonomic, metab_fast),
    TAG_F32(autonomic, metabo),     TAG_F32(autonomic, noise),
    TAG_F32(autonomic, autoreg),    TAG_F32(autonomic, fail),
};
static const tag_table ANS = TAG_TABLE(ANS_F);

static const tag_field HEALTH_F[] = {
    TAG_SUB(health, part, body_part, PART),
    TAG_F32(health, organ),
    TAG_SUB(health, wounds, wound, WOUND),
    TAG_I32R(health, wound_count, 0, HEALTH_MAX_WOUNDS),
    TAG_F32(health, blood),
    TAG_F32(health, rbc),
    TAG_F32(health, water),
    TAG_F32(health, stomach_water),
    TAG_F32(health, stomach_kcal),
    TAG_F32(health, glycogen),
    TAG_F32(health, fat),
    TAG_F32(health, bleed_ext),
    TAG_F32(health, bleed_int),
    TAG_F32(health, hr),
    TAG_F32(health, sv),
    TAG_F32(health, co),
    TAG_F32(health, svr),
    TAG_F32(health, map),
    TAG_F32(health, sbp),
    TAG_F32(health, dbp),
    TAG_F32(health, symp),
    TAG_F32(health, ischemia),
    TAG_F32(health, strain),
    TAG_F32(health, catechol),
    TAG_F32(health, surge),
    TAG_F32(health, surge_ng),
    TAG_I32R(health, rhythm, 0, RHYTHM_COUNT - 1),
    TAG_F32(health, arrest_time),
    TAG_I32R(health, last_beat_pvc, 0, 1),
    TAG_F64(health, last_r),
    TAG_F64(health, prev_r),
    TAG_F64(health, next_r),
    TAG_SUB(health, defib, defib_state, DEFIB),
    TAG_F32(health, brain_flow),
    TAG_F32(health, plasma_deficit),
    TAG_F32(health, rr),
    TAG_F32(health, breath_phase),
    TAG_I32R(health, breathing, 0, 1),
    TAG_F32(health, o2_store),
    TAG_F32(health, pao2),
    TAG_F32(health, sao2),
    TAG_F32(health, spo2_shown),
    TAG_F32(health, paco2),
    TAG_F32(health, etco2),
    TAG_F32(health, lung_water),
    TAG_F32(health, pneumothorax),
    TAG_F32(health, apnea_time),
    TAG_F32(health, gasp_timer),
    TAG_F32(health, power),
    TAG_F32(health, vo2),
    TAG_F32(health, wbal),
    TAG_F32(health, lactate),
    TAG_F32(health, shock_debt),
    TAG_F32(health, temp),
    TAG_F32(health, vaso),
    TAG_F32(health, pressor),
    TAG_F32(health, sweat_w),
    TAG_F32(health, sweat_acc),
    TAG_F32(health, wet),
    TAG_F32(health, heat_loss),
    TAG_F32(health, burn_tbsa),
    TAG_F32(health, air_temp),
    TAG_F32(health, radiant),
    TAG_I32R(health, in_fire, 0, 1),
    TAG_F32(health, reperfused),
    TAG_F32(health, myoglobin),
    TAG_F32(health, potassium),
    TAG_F32(health, brain_o2),
    TAG_F32(health, ich),
    TAG_F32(health, ich_rate),
    TAG_F32(health, icp),
    TAG_F32(health, concussion),
    TAG_F32(health, confusion),
    TAG_F32(health, pain),
    TAG_F32(health, painkiller),
    TAG_F32(health, antibiotic),
    TAG_F32(health, pk_load),
    TAG_F32(health, hepatotox),
    TAG_F32(health, gut_upset),
    TAG_F32(health, crash),
    TAG_F32(health, pain_ref),
    TAG_F32(health, opioid_gut),
    TAG_F32(health, opioid),
    TAG_F32(health, naloxone),
    TAG_F32(health, alcohol_gut),
    TAG_F32(health, bac),
    TAG_F32(health, drink_load),
    TAG_F32(health, vagal),
    TAG_F32(health, faint_drive),
    TAG_F32(health, blood_seen),
    TAG_F32(health, down),
    TAG_F32(health, empty_t),
    TAG_F32(health, fear),
    TAG_F32(health, fear_ref),
    TAG_F32(health, dark),
    TAG_F32(health, sepsis),
    TAG_I32R(health, conscious, CONS_UNCONSCIOUS, CONS_ALERT),
    TAG_F32(health, level),
    TAG_F32(health, lapse),
    TAG_F32(health, glimpse),
    TAG_I32R(health, dead, 0, 1),
    TAG_I32R(health, cause, DEATH_NONE, DEATH_OVERDOSE),
    TAG_F32(health, hurt_flash),
    TAG_F64(health, t),
    /* xorshift streams stay at zero forever, so zero is never valid. */
    TAG_AS("rng", health, rng, TAG_U32, 4u, 1, 4294967295.0),
    TAG_SUB(health, base, health_baseline, BASE),
    TAG_AS("vrng", health, vrng, TAG_U32, 4u, 1, 4294967295.0),
    TAG_F32(health, rr_noise),
    TAG_F32(health, spo2_noise),
    TAG_F32(health, ecg),
    TAG_F32(health, art),
    TAG_F32(health, pleth),
    TAG_F32(health, capno),
    TAG_I32R(health, wave_pos, 0, HEALTH_WAVE_LEN - 1),
    /* Whole samples owed are written out in a loop, so it stays below one. */
    TAG_F32R(health, wave_acc, 0, 1),
    TAG_U8(health, log),
    TAG_F32(health, log_age),
    TAG_SUB(health, cv, circ, CIRC),
    TAG_SUB(health, cond, conduction, COND),
    TAG_SUB(health, ans, autonomic, ANS),
    TAG_F64(health, atria_prev),
};
const tag_table g_health_tags = TAG_TABLE(HEALTH_F);

void health_loaded(health *h)
{
    /* The event log is printed on the HUD: every line ends inside its
     * buffer and holds no control characters. */
    for (int i = 0; i < HEALTH_LOG; i++) {
        h->log[i][sizeof h->log[i] - 1] = '\0';
        for (size_t k = 0; h->log[i][k]; k++)
            if ((unsigned char)h->log[i][k] < 0x20 || h->log[i][k] == 0x7f) h->log[i][k] = '?';
    }
}
