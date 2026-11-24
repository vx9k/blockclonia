#include "health.h"
#include "block.h"
#include "mem.h"
#include "wire.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Physiological constants for a fit 75 kg adult. */
#define BMR_W 85.0f          /* resting metabolic power */
#define CRIT_POWER 1400.0f   /* W metabolic, running ~4.4 m/s: sustainable ceiling */
#define VO2MAX_W 1500.0f     /* W metabolic at VO2max (60 mL/kg/min) */
#define WPRIME 60000.0f      /* J anaerobic reserve above critical power */
#define J_PER_ML_O2 20.1f
#define HR_REST 64.0f
#define MAP_SET 93.0f
#define O2_VOL 10.0f         /* L: lungs after a breath plus blood and myoglobin stores */
#define HEAT_CAP (75.0f * 3500.0f)
#define SVR0 18.75f          /* systemic vascular resistance at rest, mmHg min/L */
/* Resting flows of the simulated circulation (circulation.c), standing
 * still: what the brain's, the heart's and a finger's flow are measured
 * against. */
#define Q_BRAIN_REST 10.0f   /* mL/s, ~600 mL/min */
#define Q_COR_REST 3.0f      /* mL/s, ~4% of output */
#define Q_FINGER_REST 14.0f  /* mL/s into the arms' bed */
#define ARREST_DEATH_S 60.0f /* arrest called when nothing here can restart the heart (no CPR) */
#define DEFIB_ANALYSE_S 6.0f /* AEDs take 5-15 s to analyse a rhythm */
#define DEFIB_CHARGE_S 6.0f  /* and under 10 s to charge */
#define DEFIB_CLEAR_S 3.0f   /* the "stand clear" countdown of a fully automatic AED */
#define DEFIB_J 150.0f       /* biphasic: the fixed dose of many public-access AEDs */

/* Rhythms and the circulating catecholamines. */
#define ADR_HALF_S 150.0f    /* s, plasma half-life of adrenaline (2-3 min) */
#define ADR_BETA_EC50 0.5f   /* ng/mL: heart rate and contractility respond to low levels */
#define ADR_ALPHA_EC50 5.0f  /* ng/mL: vasoconstriction only at pressor levels (Clutter et al. 1980) */
#define SURGE_NG_S 0.02f     /* ng/mL/s released in a surge: a 30 s fright reaches ~0.5 ng/mL */
#define DOWN_S 25.0f         /* s a body that came round stays down */
#define SIT_S 5.0f           /* of which it spends the last sitting up */
#define CRISIS_NG_S 0.05f    /* ng/mL/s from a phaeochromocytoma: holds ~11 ng/mL */
#define FEAR_NG_S 0.006f     /* ng/mL/s at full fear: holds ~1.3 ng/mL, the range of acute terror */

/* Drugs. Oral morphine: 10 mg absorbed over about half an hour, a 3 h
 * half-life (longer when the liver fails), analgesia from a few mg, and
 * breathing held down by a sigmoid that halves the rate near 30 mg; alcohol
 * shifts it left. Ethanol spreads through the body's water (Widmark, r =
 * 0.68): 14 g, one drink, raises a 75 kg adult ~0.27 g/L, and the liver
 * clears ~0.15 g/L an hour whatever the level. The breathing curve is
 * steep (Hill 3): a tablet does little, six bring the rate near apnoea. */
#define MORPHINE_MG 10.0f
#define OPI_ABS_H 0.5f
#define OPI_HALF_H 3.0f
#define OPI_PAIN_EC50 8.0f
#define OPI_RESP_EC50 30.0f
#define DRINK_G 14.0f
#define WIDMARK (0.68f * 75.0f)
#define ALC_ELIM 0.15f
#define NALOXONE_H 1.0f /* shorter than morphine lasts: it can wear off first */
/* A vasovagal spell: ~20 s of warning (pallor, sweat, nausea, greying),
 * then the faint if the head stays up. Falling flat brings the blood back
 * to the brain, so the reflex fades within seconds of fainting. */
#define FAINT_S 20.0f

static const float PAIN_SCALE = 10.0f;
#define DEG "\x7f" /* the UI font's degree sign */

/* Heat exchange with the surroundings (ISO 7730/9920 conventions). */
#define BODY_AREA 1.9f       /* m^2, DuBois area of a 75 kg, 1.78 m adult */
#define H_RAD 4.7f           /* W/m^2K, linearised radiation to surroundings near room temperature */
#define ABSORB 0.95f         /* skin and most fabrics absorb ~95% of infrared */
#define LATENT 2.43e6f       /* J/kg to evaporate water near skin temperature */
#define SHOE_W_K 1.0f        /* W/K per foot through the sole to the ground (~2 W/K for both) */
#define SOLE_G 67.0f         /* W/m^2K: the same through the 0.015 m^2 of a sole, for burns */
#define CLOTHES_WATER 1.2f   /* kg of water soaked clothes hold */
#define AIR_RH 0.5f          /* relative humidity assumed for evaporation */
#define COOL_G 400.0f        /* W/m^2K: running water on skin */
#define CLOTHED_AREA (0.91f * BODY_AREA)
/* The burn layer: epidermis and upper dermis (~0.75 mm, rho c ~4 MJ/m^3K)
 * over the shell. With these, bare skin under 10 kW/m^2 blisters in about
 * 11 s and under 20 kW/m^2 in about 5 s (Stoll and Chianta 1969: 12-14 s
 * and ~5 s), while 2 kW/m^2 settles near the 44-45 C pain threshold. */
#define BURN_C 3000.0f       /* J/m^2K */
#define BURN_G 250.0f        /* W/m^2K: conduction and skin blood flow into the shell */
/* Henriques-Moritz (1947): dOmega/dt = P exp(-E/RT) above 44 C at the basal
 * layer; Omega 0.53 is a first-degree burn, 1 second degree, 10^4 third. */
#define HM_LN_P 226.785      /* ln(3.1e98 1/s) */
#define HM_E_R 75000.0       /* K */
#define FLAME_C 800.0        /* degrees C of the gas in a wood fire */

/* Per part: rule-of-nines share of the body surface (Wallace), clothing
 * insulation (m^2K/W; ~0.9 clo on average: the trunk layered, thin sleeves
 * on the arms, whose shell is the bare hand, a bare head), the bare
 * fraction, the area facing a fire (m^2 of projected area, 0.6 in all),
 * the share of a part flames reach when standing in a fire (they climb
 * from the feet), and the skin blood-flow conductance from constricted to
 * dilated (W/m^2K; ~10 W/K for the whole body constricted, ~190 W/K or
 * 3 L/min of skin blood flow dilated, more under local heat). */
static const float NINES[BP_COUNT] = {0.09f, 0.18f, 0.19f, 0.09f, 0.09f, 0.18f, 0.18f};
static const float CLO_R[BP_COUNT] = {0.0f, 0.2f, 0.2f, 0.03f, 0.03f, 0.14f, 0.14f};
static const float BARE[BP_COUNT] = {1.0f, 0.0f, 0.0f, 0.25f, 0.25f, 0.0f, 0.0f};
static const float PROJ[BP_COUNT] = {0.05f, 0.12f, 0.1f, 0.05f, 0.05f, 0.115f, 0.115f};
static const float FLAMES[BP_COUNT] = {0.1f, 0.25f, 0.5f, 0.35f, 0.35f, 1.0f, 1.0f};
static const float K_MIN[BP_COUNT] = {10.0f, 9.0f, 9.0f, 1.5f, 1.5f, 1.5f, 1.5f};
static const float K_MAX[BP_COUNT] = {40.0f, 90.0f, 90.0f, 120.0f, 120.0f, 120.0f, 120.0f};
/* J/m^2K of the shell: ~7 mm of tissue over the head and trunk, and the
 * thin fingers and toes for the limbs, which is why they cool first. */
static const float SHELL_C[BP_COUNT] = {25000.0f, 25000.0f, 25000.0f, 10000.0f, 10000.0f, 10000.0f, 10000.0f};
/* Crush: muscle mass (kg), the area a resting weight bears on (m^2) and the
 * static force that breaks the part's bone (N). */
static const float MUSCLE_KG[BP_COUNT] = {0.5f, 6.0f, 6.0f, 3.5f, 3.5f, 11.0f, 11.0f};
static const float PRESS_AREA[BP_COUNT] = {0.02f, 0.08f, 0.08f, 0.03f, 0.03f, 0.06f, 0.06f};
static const float BREAK_N[BP_COUNT] = {6000.0f, 3500.0f, 4000.0f, 2000.0f, 2000.0f, 4000.0f, 4000.0f};

/* ------------------------------------------------------------ helpers */

static float frand_s(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return (float)(x >> 8) * (1.0f / 16777216.0f);
}

static float frand(health *h) { return frand_s(&h->rng); }

/* murmur3's finaliser, used to spread a small or sequential seed (1, 2,
 * 3, ...) across the whole 32 bits before drawing a body's constitution
 * from it: the xorshift above needs a well-mixed seed or its first draws
 * cluster near 0. */
static uint32_t hash32(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

/* A bounded wander for short-term variability: each call redraws the
 * target within +-amp and eases `cur` toward it over `tau` seconds, so the
 * result never leaves [-amp, amp] and stays smooth from step to step. */
static float wander(uint32_t *rng, float cur, double dt, double tau, float amp)
{
    float target = (frand_s(rng) * 2.0f - 1.0f) * amp;
    return cur + (target - cur) * (float)(1.0 - exp(-dt / tau));
}

static float fclamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static float maxf(float a, float b) { return a > b ? a : b; }
static float minf(float a, float b) { return a < b ? a : b; }

/* 0 below a, 1 above b, smooth in between. */
static float ramp(float v, float a, float b)
{
    float t = fclamp((v - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

static float lag(float cur, float target, double dt, double tau)
{
    return cur + (target - cur) * (float)(1.0 - exp(-dt / tau));
}

/* Adrenoceptor occupancy by circulating adrenaline, as Emax models: beta
 * effects (rate, contractility, beta-2 vasodilation) come at a fraction
 * of the levels that clamp the vessels through alpha receptors, which is
 * why a small dose lowers diastolic pressure and a large one raises it. */
static float catechol_beta(const health *h) { return h->catechol / (h->catechol + ADR_BETA_EC50); }

static float catechol_alpha(const health *h)
{
    float c2 = h->catechol * h->catechol;
    return c2 / (c2 + ADR_ALPHA_EC50 * ADR_ALPHA_EC50);
}

/* Haemoglobin saturation from PO2 (Severinghaus 1979). */
static float severinghaus(float po2)
{
    if (po2 <= 0.0f) return 0.0f;
    double p = (double)po2;
    return (float)(1.0 / (23400.0 / (p * p * p + 150.0 * p) + 1.0));
}

static float dehydration(const health *h) { return maxf(0.0f, 1.0f - h->water / WATER_NORMAL); }
static float hct(const health *h) { return h->blood > 0.05f ? h->rbc / h->blood : 0.0f; }
float health_hb(const health *h) { return hct(h) * 33.3f; }
static float vo2max_l(float metab_gain) { return VO2MAX_W * metab_gain / J_PER_ML_O2 * 0.06f; }

/* Opioid the brain sees: naloxone takes its receptors while it lasts. */
static float opioid_free(const health *h) { return h->naloxone > 0.0f ? 0.03f * h->opioid : h->opioid; }

float health_resp_depression(const health *h)
{
    float o = opioid_free(h), ec = OPI_RESP_EC50 / (1.0f + h->bac / 0.8f);
    float o3 = o * o * o, d_o = o3 / (o3 + ec * ec * ec);
    float d_a = 0.9f * ramp(h->bac, 2.5f, 5.0f);
    return 1.0f - (1.0f - d_o) * (1.0f - d_a);
}

float health_sedation(const health *h)
{
    float o = opioid_free(h);
    return o / (o + 40.0f) + h->bac / 3.5f;
}

static int is_limb(int part) { return part >= BP_LARM; }
static int is_leg(int part) { return part == BP_LLEG || part == BP_RLEG; }
static int is_arm(int part) { return part == BP_LARM || part == BP_RARM; }

static void die(health *h, int cause);

void health_log(health *h, const char *fmt, ...)
{
    memmove(h->log[1], h->log[0], sizeof h->log[0] * (HEALTH_LOG - 1));
    memmove(&h->log_age[1], &h->log_age[0], sizeof h->log_age[0] * (HEALTH_LOG - 1));
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(h->log[0], sizeof h->log[0], fmt, ap);
    va_end(ap);
    h->log_age[0] = 0.0f;
    h->log_count++;
}

static const char *const PART_NAMES[BP_COUNT] = {"Head",      "Chest",    "Abdomen",  "Left arm",
                                                 "Right arm", "Left leg", "Right leg"};
static const char *const PART_NAMES_LC[BP_COUNT] = {"head",      "chest",    "abdomen",  "left arm",
                                                    "right arm", "left leg", "right leg"};
static const char *const BONE_NAMES[BP_COUNT] = {"skull",         "ribs",       "pelvis",     "left forearm",
                                                 "right forearm", "left tibia", "right tibia"};
static const char *const ORGAN_NAMES[ORG_COUNT] = {"Brain", "Heart", "Lungs", "Liver", "Kidneys", "Gut"};

static const char *const RHYTHM_NAMES[RHYTHM_COUNT] = {"Sinus rhythm",
                                                       "Ventricular fibrillation",
                                                       "Asystole",
                                                       "Atrial fibrillation",
                                                       "Ventricular tachycardia",
                                                       "Pulseless ventricular tachycardia",
                                                       "Pulseless electrical activity",
                                                       "Complete heart block"};
/* What a monitor prints under the heart rate: 12 characters at most. */
static const char *const RHYTHM_SHORT[RHYTHM_COUNT] = {"SINUS", "VF",           "ASYSTOLE", "AF",
                                                       "VT",    "PULSELESS VT", "PEA",      "AV BLOCK III"};

const char *health_rhythm_name(int rhythm) { return rhythm >= 0 && rhythm < RHYTHM_COUNT ? RHYTHM_NAMES[rhythm] : "?"; }
const char *health_rhythm_short(int rhythm)
{
    return rhythm >= 0 && rhythm < RHYTHM_COUNT ? RHYTHM_SHORT[rhythm] : "?";
}

int health_rhythm_perfusing(int rhythm)
{
    return rhythm == RHYTHM_SINUS || rhythm == RHYTHM_AF || rhythm == RHYTHM_VT || rhythm == RHYTHM_AVB3;
}

int health_rhythm_shockable(int rhythm) { return rhythm == RHYTHM_VF || rhythm == RHYTHM_PVT; }

const char *health_part_name(int part) { return part >= 0 && part < BP_COUNT ? PART_NAMES[part] : "?"; }
const char *health_bone_name(int part) { return part >= 0 && part < BP_COUNT ? BONE_NAMES[part] : "?"; }
const char *health_organ_name(int organ) { return organ >= 0 && organ < ORG_COUNT ? ORGAN_NAMES[organ] : "?"; }

const char *health_death_text(int cause)
{
    switch (cause) {
    case DEATH_BLOOD_LOSS: return "Bled to death (haemorrhagic shock)";
    case DEATH_DROWNING: return "Drowned";
    case DEATH_SUFFOCATION: return "Suffocated";
    case DEATH_HEAD_INJURY: return "Fatal head injury";
    case DEATH_CARDIAC: return "Cardiac arrest";
    case DEATH_SEPSIS: return "Septic shock from an infected wound";
    case DEATH_DEHYDRATION: return "Died of dehydration";
    case DEATH_STARVATION: return "Starved to death";
    case DEATH_HYPOTHERMIA: return "Hypothermia";
    case DEATH_HYPERTHERMIA: return "Heat stroke";
    case DEATH_TRAUMA: return "Crushed";
    case DEATH_BURNS: return "Burns (burn shock)";
    case DEATH_OVERDOSE: return "Overdose: breathing stopped";
    default: return "Alive";
    }
}

/* --------------------------------------------------------------- init */

/* The heart and vessels of this constitution, full and at rest. */
static void start_circulation(health *h)
{
    circ_init(&h->cv, h->base.blood_per_kg * 75.0f, h->base.heart_size, h->base.art_stiff);
    cond_init(&h->cond);
    auto_init(&h->ans, h->base.hr_rest);
    h->blood = circ_volume(&h->cv) / 1000.0f;
    h->rbc = h->blood * 0.45f;
}

void health_init(health *h, uint32_t seed)
{
    memset(h, 0, sizeof *h);
    h->rng = seed ? seed : 0x9E3779B9u;
    /* The variability stream, never zero (xorshift would stay there). */
    h->vrng = hash32(h->rng ^ 0xA5A5A5A5u) | 1u;
    h->base.hr_rest = HR_REST;
    h->base.map_set = MAP_SET;
    h->base.pain_gain = h->base.clot_gain = h->base.metab_gain = 1.0f;
    h->base.heart_size = h->base.art_stiff = h->base.baro_gain = h->base.venous_tone = h->base.symp_noise = 1.0f;
    h->base.hr_max = 200.0f;
    h->base.blood_per_kg = 70.0f;
    for (int i = 0; i < BP_COUNT; i++) {
        h->part[i].integrity = 1.0f;
        h->part[i].skin = is_limb(i) ? 31.0f : 33.5f; /* clothed at rest in 20 C air */
    }
    for (int i = 0; i < ORG_COUNT; i++) h->organ[i] = 1.0f;

    h->blood = BLOOD_NORMAL;
    h->rbc = BLOOD_NORMAL * 0.45f;
    h->water = WATER_NORMAL;
    h->stomach_water = 0.2f;
    h->stomach_kcal = 250.0f;
    h->glycogen = 1600.0f;
    h->fat = FAT_NORMAL;

    h->hr = h->base.hr_rest;
    h->sv = 75.0f;
    h->co = h->base.hr_rest * 75.0f / 1000.0f;
    h->svr = SVR0;
    h->map = h->base.map_set;
    h->sbp = 120.0f;
    h->dbp = 80.0f;
    h->symp = 0.15f;
    h->rhythm = RHYTHM_SINUS;
    h->last_r = h->prev_r = h->next_r = h->atria_prev = -1.0;
    h->brain_flow = 1.0f;

    h->rr = 14.0f;
    h->breathing = 1;
    h->o2_store = O2_VOL * 100.0f / 713.0f;
    h->pao2 = 100.0f;
    h->sao2 = h->spo2_shown = 0.975f;
    h->paco2 = 40.0f;
    h->etco2 = 37.0f;

    h->power = BMR_W;
    h->vo2 = BMR_W / J_PER_ML_O2 * 0.06f;
    h->wbal = WPRIME;
    h->lactate = 1.0f;
    h->temp = 37.0f;
    h->vaso = 0.2f;
    h->air_temp = 20.0f;
    h->potassium = 4.2f;

    h->brain_o2 = 1.0f;
    h->icp = 10.0f;
    h->conscious = CONS_ALERT;
    h->level = 1.0f;

    for (int i = 0; i < HEALTH_WAVE_LEN; i++) {
        h->art[i] = 80.0f;
        h->pleth[i] = 0.0f;
    }
    for (int i = 0; i < HEALTH_LOG; i++) h->log_age[i] = 1e9f;
    start_circulation(h);
}

/* Nobody is born mid-collapse: the circuit starts from resting lying-down
 * volumes, and standing up from them drains the chest into the legs and
 * sets the reflex racing. Settle a copy standing still for half a minute
 * and keep its blood, reflex state and readings, so the first frame
 * already shows a body at rest. Its clocks and logs are thrown away. */
void health_settle(health *h)
{
    health *t = mem_alloc(sizeof *t);
    memcpy(t, h, sizeof *t);
    health_env e = {0};
    e.on_ground = 1;
    e.airway = AIRWAY_AIR;
    e.water_temp = e.air_temp = e.contact_temp = 20.0;
    /* Stop as the atria fire, the start of a beat, and start this body's
     * sinus node at that same instant so the copied volumes fit the beat. */
    double fired = -1.0;
    for (int i = 0; i < 40 * 60; i++) {
        health_step(t, &e, 1.0 / 60.0);
        if (i >= 30 * 60 && fired >= 0.0 && t->cond.t_atria != fired) break;
        fired = t->cond.t_atria;
    }
    h->cond.sa_phase = 1.0f;
    /* The whole circuit, last beat's readings included, on this body's clock. */
    long long steps = h->cv.steps;
    h->cv = t->cv;
    h->cv.steps = steps;
    h->cv.beat_t = -1.0;
    h->ans = t->ans;
    h->hr = t->hr;
    h->sv = t->sv;
    h->co = t->co;
    h->svr = t->svr;
    h->map = t->map;
    h->sbp = t->sbp;
    h->dbp = t->dbp;
    mem_free(t);
}

void health_init_varied(health *h, uint32_t seed)
{
    health_init(h, seed);
    /* Drawn from a hash of seed, not h->rng itself: the constitution must
     * not depend on how it happens to be mixed, and must not shift the
     * sequence health_init already handed out to injuries. */
    uint32_t s = hash32(seed);
    h->base.hr_rest = 55.0f + 30.0f * frand_s(&s);   /* bpm */
    h->base.map_set = 85.0f + 16.0f * frand_s(&s);   /* mmHg */
    h->base.pain_gain = 0.8f + 0.4f * frand_s(&s);
    h->base.clot_gain = 0.75f + 0.6f * frand_s(&s);
    h->base.metab_gain = 0.92f + 0.18f * frand_s(&s);
    h->base.cold_bias = -0.4f + 0.8f * frand_s(&s);
    h->base.heart_size = 0.85f + 0.3f * frand_s(&s);
    h->base.art_stiff = 0.75f + 0.5f * frand_s(&s);
    h->base.blood_per_kg = 65.0f + 10.0f * frand_s(&s);
    h->base.baro_gain = 0.7f + 0.6f * frand_s(&s);
    h->base.venous_tone = 0.9f + 0.2f * frand_s(&s);
    h->base.symp_noise = 0.5f + 1.0f * frand_s(&s);
    h->base.vary = 1;
    h->vrng = hash32(s) | 1u; /* a stream of its own for the short-term wander */
    h->base.hr_max = 185.0f + 30.0f * frand_s(&s); /* drawn last: the earlier draws stay as they were */

    h->hr = h->base.hr_rest;
    h->co = h->base.hr_rest * 75.0f / 1000.0f;
    h->map = h->base.map_set;
    start_circulation(h);
    h->power = BMR_W * h->base.metab_gain;
    h->vo2 = h->power / J_PER_ML_O2 * 0.06f;
}

/* ------------------------------------------------------------ injuries */

static wound *add_wound(health *h, int part, int kind, float depth, int arterial, float contamination)
{
    wound *w = NULL;
    if (h->wound_count < HEALTH_MAX_WOUNDS) {
        w = &h->wounds[h->wound_count++];
    } else {
        /* Merge into the least severe existing wound rather than drop it. */
        w = &h->wounds[0];
        for (int i = 1; i < h->wound_count; i++)
            if (h->wounds[i].bleed0 < w->bleed0) w = &h->wounds[i];
    }
    memset(w, 0, sizeof *w);
    w->part = (uint8_t)part;
    w->kind = (uint8_t)kind;
    w->depth = fclamp(depth, 0.0f, 1.0f);
    w->arterial = (uint8_t)arterial;
    /* Capillary/venous ooze grows with depth; a severed artery pours, and
     * deeper cuts reach bigger arteries (a femoral bleed tops 1 L/min). */
    w->bleed0 = arterial ? 150.0f + 850.0f * w->depth * w->depth : 3.0f + 60.0f * w->depth * w->depth;
    w->bleed = w->bleed0;
    w->contamination = fclamp(contamination, 0.0f, 1.0f);
    return w;
}

static void break_bone(health *h, int part, int open)
{
    body_part *p = &h->part[part];
    if (p->fracture == FX_OPEN || (p->fracture == FX_CLOSED && !open)) return;
    p->fracture = (uint8_t)(open ? FX_OPEN : FX_CLOSED);
    p->fracture_heal = 0.0f;
    p->splinted = 0;
    if (open) add_wound(h, part, WOUND_OPEN_FRACTURE, 0.6f, 0, 0.45f);
    /* Broken long bones and the pelvis bleed into the surrounding tissue. */
    if (part == BP_ABDOMEN) p->internal += 60.0f + 90.0f * frand(h);
    else if (is_leg(part)) p->internal += 20.0f + 25.0f * frand(h);
    else if (is_limb(part)) p->internal += 5.0f + 10.0f * frand(h);
    health_log(h, "%s fracture: %s", open ? "Open" : "Closed", BONE_NAMES[part]);
}

static void head_injury(health *h, float s)
{
    if (s < 0.1f) return;
    body_part *p = &h->part[BP_HEAD];
    p->integrity = maxf(0.0f, p->integrity - 0.6f * minf(s, 1.0f));
    if (s > 0.35f) {
        h->concussion = maxf(h->concussion, 5.0f + 60.0f * (s - 0.35f));
        health_log(h, "Knocked out: concussion");
    }
    h->confusion = maxf(h->confusion, 60.0f + 600.0f * minf(s, 1.0f));
    if (frand(h) < ramp(s, 0.55f, 0.9f)) break_bone(h, BP_HEAD, s > 0.85f && frand(h) < 0.5f);
    if (frand(h) < ramp(s, 0.5f, 0.95f)) {
        h->ich_rate += 3.0f + 20.0f * minf(s, 1.0f) * frand(h);
        health_log(h, "Bleeding inside the skull");
    }
    if (s > 1.0f) {
        h->organ[ORG_BRAIN] = maxf(0.0f, h->organ[ORG_BRAIN] - (s - 1.0f) * 2.5f);
        h->ich_rate += 40.0f * (s - 1.0f);
    }
}

static void chest_injury(health *h, float s)
{
    if (s < 0.1f) return;
    body_part *p = &h->part[BP_CHEST];
    p->integrity = maxf(0.0f, p->integrity - 0.5f * minf(s, 1.0f));
    if (frand(h) < ramp(s, 0.3f, 0.8f)) {
        break_bone(h, BP_CHEST, 0);
        if (frand(h) < 0.35f) {
            h->pneumothorax = minf(1.0f, h->pneumothorax + 0.15f + 0.45f * frand(h));
            health_log(h, "Collapsed lung (pneumothorax)");
        }
    }
    h->organ[ORG_LUNGS] = maxf(0.0f, h->organ[ORG_LUNGS] - 0.4f * ramp(s, 0.4f, 1.2f));
    float heart = 0.4f * ramp(s, 0.65f, 1.3f);
    if (heart > 0.01f) {
        h->organ[ORG_HEART] = maxf(0.0f, h->organ[ORG_HEART] - heart);
        health_log(h, "Heart contusion");
    }
    /* Commotio cordis: a blow over the heart at the wrong moment. */
    if (s > 0.25f && s < 0.7f && frand(h) < 0.03f && h->rhythm == RHYTHM_SINUS) {
        h->rhythm = RHYTHM_VF;
        h->cause = DEATH_CARDIAC;
        health_log(h, "Chest blow stopped the heart (commotio cordis)");
    }
    if (s > 0.8f) p->internal += 30.0f * (s - 0.8f) / 0.2f * frand(h);
}

static void abdomen_injury(health *h, float s)
{
    if (s < 0.1f) return;
    body_part *p = &h->part[BP_ABDOMEN];
    p->integrity = maxf(0.0f, p->integrity - 0.5f * minf(s, 1.0f));
    if (frand(h) < ramp(s, 0.45f, 1.0f)) {
        float cut = 0.2f + 0.3f * frand(h);
        h->organ[ORG_LIVER] = maxf(0.0f, h->organ[ORG_LIVER] - cut);
        p->internal += 30.0f + 120.0f * minf(s, 1.2f) * frand(h);
        health_log(h, "Liver or spleen torn: internal bleeding");
    }
    if (frand(h) < ramp(s, 0.4f, 1.0f)) h->organ[ORG_KIDNEYS] = maxf(0.0f, h->organ[ORG_KIDNEYS] - 0.2f);
    if (frand(h) < ramp(s, 0.4f, 1.0f)) h->organ[ORG_GUT] = maxf(0.0f, h->organ[ORG_GUT] - 0.3f);
}

/* A shoulder (anterior, from a fall on the outstretched hand or a blow)
 * or an elbow out of joint. Arms only. */
static void dislocate(health *h, int part, int kind)
{
    body_part *p = &h->part[part];
    if (!is_arm(part) || p->dislocated || p->fracture) return;
    p->dislocated = (uint8_t)kind;
    p->pain_spike = PAIN_SCALE;
    health_log(h, "Dislocated %s %s", part == BP_LARM ? "left" : "right", kind == DISLOC_ELBOW ? "elbow" : "shoulder");
}

static void limb_injury(health *h, int part, float s)
{
    if (s < 0.1f) return;
    body_part *p = &h->part[part];
    p->integrity = maxf(0.0f, p->integrity - 0.6f * minf(s, 1.0f));
    if (frand(h) < ramp(s, 0.3f, 0.9f)) break_bone(h, part, s > 0.8f && frand(h) < 0.5f);
    else if (is_arm(part) && frand(h) < 0.4f * ramp(s, 0.2f, 0.6f)) dislocate(h, part, DISLOC_SHOULDER);
    else if (frand(h) < ramp(s, 0.15f, 0.5f)) {
        p->sprain = maxf(p->sprain, 0.3f + 0.5f * frand(h));
        health_log(h, "%s sprained", PART_NAMES[part]);
    }
}

void health_fall(health *h, double dv, double cushion)
{
    if (h->dead) return;
    float v = (float)(dv * cushion);
    if (v < 5.0f) return;
    h->hurt_flash = minf(1.0f, h->hurt_flash + (v - 4.0f) * 0.15f);

    /* Legs take the landing: bruising, sprained ankles, then fractures. */
    for (int leg = BP_LLEG; leg <= BP_RLEG; leg++) {
        body_part *p = &h->part[leg];
        p->integrity = maxf(0.0f, p->integrity - fclamp((v - 5.0f) * 0.05f, 0.0f, 0.9f));
        float t = fclamp((v - 6.5f) / 6.0f, 0.0f, 1.0f);
        if (frand(h) < t * t) {
            break_bone(h, leg, v > 11.0f && frand(h) < 0.6f * ramp(v, 11.0f, 16.0f));
        } else if (frand(h) < 0.6f * ramp(v, 5.5f, 9.0f)) {
            p->sprain = maxf(p->sprain, 0.3f + 0.5f * frand(h));
            health_log(h, "%s sprained", PART_NAMES[leg]);
        }
    }
    /* Arms brace the fall: wrist fractures, or the load levers the
     * shoulder (sometimes the elbow) out of joint. */
    for (int arm = BP_LARM; arm <= BP_RARM; arm++) {
        h->part[arm].integrity = maxf(0.0f, h->part[arm].integrity - fclamp((v - 7.0f) * 0.04f, 0.0f, 0.6f));
        if (frand(h) < 0.5f * ramp(v, 8.0f, 14.0f)) break_bone(h, arm, 0);
        else if (v > 6.5f && frand(h) < 0.3f * ramp(v, 6.5f, 11.0f))
            dislocate(h, arm, frand(h) < 0.7f ? DISLOC_SHOULDER : DISLOC_ELBOW);
    }
    /* Hard landings travel up the skeleton. */
    if (v > 9.0f) {
        float s = (v - 9.0f) / 8.0f;
        if (frand(h) < ramp(v, 10.0f, 17.0f)) break_bone(h, BP_ABDOMEN, 0);
        abdomen_injury(h, s * (0.6f + 0.6f * frand(h)));
        chest_injury(h, (v - 10.0f) / 7.0f * (0.6f + 0.6f * frand(h)));
    }
    if (v > 11.0f) head_injury(h, (v - 11.0f) / 9.0f * (0.5f + 0.7f * frand(h)));
}

void health_blunt(health *h, int part, double dv)
{
    if (h->dead || part < 0 || part >= BP_COUNT) return;
    float s = ((float)dv - 6.0f) / 12.0f;
    if (s <= 0.0f) return;
    h->hurt_flash = minf(1.0f, h->hurt_flash + s);
    switch (part) {
    case BP_HEAD: head_injury(h, s); break;
    case BP_CHEST: chest_injury(h, s); break;
    case BP_ABDOMEN: abdomen_injury(h, s); break;
    default: limb_injury(h, part, s); break;
    }
}

void health_struck(health *h, double mass, double speed, double height, double softness)
{
    if (h->dead || mass <= 0.0 || speed <= 0.0) return;
    double e = 0.5 * mass * speed * speed * (1.0 - (double)fclamp((float)softness, 0.0f, 0.98f));
    if (e < 20.0) return;
    /* Severity on a log scale between a harmless and a devastating blow. */
#define SEV(lo, hi) (float)((log10(e) - log10(lo)) / (log10(hi) - log10(lo)))
    int part = 0;
    if (height >= 0.87) part = BP_HEAD;
    else if (height >= 0.6) part = frand(h) < 0.6f ? BP_CHEST : (frand(h) < 0.5f ? BP_LARM : BP_RARM);
    else if (height >= 0.45) part = BP_ABDOMEN;
    else part = frand(h) < 0.5f ? BP_LLEG : BP_RLEG;

    h->hurt_flash = 1.0f;
    health_log(h, "Hit by a %.0f kg block at %.1f m/s", mass, speed);
    switch (part) {
    case BP_HEAD: {
        float s = SEV(30.0, 3000.0);
        head_injury(h, s);
        /* A heavy load landing on the head also loads the neck and chest. */
        if (mass > 150.0) chest_injury(h, SEV(300.0, 20000.0));
        if (s > 1.3f) {
            h->dead = 1;
            h->cause = DEATH_TRAUMA;
            health_log(h, "Crushed by a falling block");
        }
        break;
    }
    case BP_CHEST: chest_injury(h, SEV(100.0, 10000.0)); break;
    case BP_ABDOMEN: abdomen_injury(h, SEV(100.0, 8000.0)); break;
    default: limb_injury(h, part, SEV(50.0, 3000.0)); break;
    }
#undef SEV
}

void health_cut(health *h, int part, float severity, int arterial, float contamination)
{
    if (h->dead || part < 0 || part >= BP_COUNT) return;
    wound *w = add_wound(h, part, WOUND_CUT, severity, arterial, contamination);
    h->part[part].integrity = maxf(0.0f, h->part[part].integrity - 0.1f * severity);
    h->hurt_flash = minf(1.0f, h->hurt_flash + 0.3f + severity);
    if (arterial) health_log(h, "Artery cut on the %s: %.0f mL/min", PART_NAMES_LC[part], (double)w->bleed0);
    else health_log(h, "%s cut on the %s", severity > 0.5f ? "Deep" : "Shallow", PART_NAMES_LC[part]);
}

void health_break_bone(health *h, int part, int open)
{
    if (h->dead || part < 0 || part >= BP_COUNT) return;
    break_bone(h, part, open);
    h->hurt_flash = minf(1.0f, h->hurt_flash + 0.6f);
}

void health_dislocate(health *h, int part)
{
    if (h->dead || !is_arm(part)) return;
    dislocate(h, part, DISLOC_SHOULDER);
    h->hurt_flash = minf(1.0f, h->hurt_flash + 0.5f);
}

void health_abrasion(health *h, int part, float area_fraction, float contamination)
{
    if (h->dead || part < 0 || part >= BP_COUNT || !(area_fraction > 0.0f)) return;
    float area = fclamp(area_fraction, 0.01f, 1.0f);
    wound *w = add_wound(h, part, WOUND_ABRASION, 0.08f + 0.12f * area, 0, contamination);
    /* Shallow and wide: capillary ooze from the whole scraped area. */
    w->bleed0 = w->bleed = 2.0f + 25.0f * area;
    w->area = area;
    h->part[part].integrity = maxf(0.0f, h->part[part].integrity - 0.1f * area);
    h->hurt_flash = minf(1.0f, h->hurt_flash + 0.3f + area);
    health_log(h, "Skin scraped off the %s (%.0f%% of it)", PART_NAMES_LC[part], (double)(area * 100.0f));
}

/* Muscle under more than capillary pressure (~4 kPa, 30 mmHg) is starved of
 * blood; after a few hours it dies, sooner under a heavier load (the
 * clinical threshold for crush syndrome is about an hour of a limb under
 * rubble). Damage runs on the survival clock; bones give way at once. */
void health_crush(health *h, int part, double mass_kg, double dt)
{
    if (h->dead || part < 0 || part >= BP_COUNT || !(mass_kg > 0.0) || !(dt > 0.0)) return;
    body_part *p = &h->part[part];
    p->crush_next += (float)mass_kg;
    float force = (float)mass_kg * 9.81f, pressure = force / PRESS_AREA[part];
    float gh = (float)(dt * HEALTH_CLOCK / 3600.0);
    p->crush = minf(1.0f, p->crush + 0.5f * ramp(pressure, 4000.0f, 40000.0f) * gh);
    p->integrity = minf(p->integrity, 1.0f - 0.7f * p->crush);
    if (force > BREAK_N[part] && !p->fracture) {
        break_bone(h, part, 0);
        h->hurt_flash = 1.0f;
    }
    if (part == BP_HEAD && force > 2.0f * BREAK_N[BP_HEAD]) {
        health_log(h, "Skull crushed under %.0f kg", mass_kg);
        die(h, DEATH_TRAUMA);
    }
}

static wound *part_wound(health *h, int part, int kind)
{
    for (int i = 0; i < h->wound_count; i++)
        if (h->wounds[i].part == part && h->wounds[i].kind == kind) return &h->wounds[i];
    return NULL;
}

static const char *ordinal(int n) { return n == 1 ? "1st" : (n == 2 ? "2nd" : "3rd"); }

/* Turns a part's damage integral into a burn degree. Second and third
 * degree burns are open wounds (they leak plasma, get contaminated and
 * are dressed like one); third degree destroys the full thickness. */
static void burn_degree(health *h, int part)
{
    body_part *p = &h->part[part];
    float om = p->burn_omega;
    int deg = om >= 1e4f ? 3 : (om >= 1.0f ? 2 : (om >= 0.53f ? 1 : 0));
    if (deg <= p->burn) return;
    if (!p->burn) {
        p->burn_age = 0.0f;
        p->burn_cooled = p->cool_time > 0.0f;
    }
    if (p->burn_area < 0.01f) p->burn_area = 0.5f * (BARE[part] > 0.0f ? BARE[part] : 1.0f);
    p->burn = (uint8_t)deg;
    h->hurt_flash = minf(1.0f, h->hurt_flash + 0.3f * (float)deg);
    if (deg >= 2) {
        wound *w = part_wound(h, part, WOUND_BURN);
        if (!w) w = add_wound(h, part, WOUND_BURN, 0.4f, 0, 0.15f);
        w->depth = deg == 3 ? 0.9f : 0.4f;
        w->bleed0 = w->bleed = 0.0f;
        w->area = p->burn_area;
        if (deg == 3) p->integrity = minf(p->integrity, 1.0f - 0.8f * p->burn_area);
    }
    health_log(h, "%s-degree burn on the %s, %.0f%% of body", ordinal(deg), PART_NAMES_LC[part],
               (double)(p->burn_area * NINES[part] * 100.0f));
}

void health_burn(health *h, int part, double omega)
{
    if (h->dead || part < 0 || part >= BP_COUNT || !(omega > 0.0)) return;
    body_part *p = &h->part[part];
    p->burn_omega = maxf(p->burn_omega, (float)fmin(omega, 1e6));
    if (p->burn_area < 0.01f) p->burn_area = 0.5f;
    burn_degree(h, part);
}

/* ------------------------------------------------------------ treatment */

/* The two treatments that bypass a safeguard: adrenaline, which on a
 * living circulation races the heart, and a shock the AED did not advise. */
static int treat_adrenaline(health *h, inventory *inv, char *msg, size_t msg_size)
{
    if (!inv_take(inv, I_ADRENALINE, 1)) {
        snprintf(msg, msg_size, "No adrenaline auto-injector");
        return 0;
    }
    /* 0.3 mg into the thigh: on a living circulation it peaks in minutes
     * and races the heart; in arrest it is what is given every few
     * minutes, and it can coarsen a flat line into a shockable one. */
    h->catechol += 1.5f;
    if (!health_rhythm_perfusing(h->rhythm)) {
        if ((h->rhythm == RHYTHM_ASYSTOLE || h->rhythm == RHYTHM_PEA) && frand(h) < 0.15f) {
            h->rhythm = RHYTHM_VF;
            health_log(h, "Adrenaline: the heart fibrillates again (shockable)");
        }
        snprintf(msg, msg_size, "Injected adrenaline into a stopped heart");
    } else {
        snprintf(msg, msg_size,
                 h->catechol > 3.0f ? "Adrenaline again: heart pounding, pressure soaring"
                                    : "Injected adrenaline: heart racing, hands shaking");
    }
    return 1;
}

/* Morphine, naloxone and a drink. */
static int treat_drug(health *h, inventory *inv, int what, char *msg, size_t msg_size)
{
    if (what == TREAT_MORPHINE) {
        if (!inv_take(inv, I_MORPHINE, 1)) {
            snprintf(msg, msg_size, "No morphine left");
            return 0;
        }
        h->opioid_gut += MORPHINE_MG;
        float total = h->opioid + h->opioid_gut;
        snprintf(msg, msg_size,
                 total > 25.0f ? (h->bac > 0.5f ? "Morphine on top of drink: breathing may stop"
                                                : "More morphine (%.0f mg): breathing will slow")
                               : "Took 10 mg of morphine: relief in half an hour",
                 (double)total);
        return 1;
    }
    if (what == TREAT_NALOXONE) {
        if (!inv_take(inv, I_NALOXONE, 1)) {
            snprintf(msg, msg_size, "No naloxone");
            return 0;
        }
        /* Snatching the receptors back all at once: the pain returns, and
         * with it a surge of adrenaline. */
        float was = opioid_free(h);
        h->naloxone = NALOXONE_H;
        if (was > 5.0f) {
            h->surge = maxf(h->surge, 15.0f);
            h->surge_ng = maxf(h->surge_ng, SURGE_NG_S);
            health_log(h, "Naloxone: the opioid lets go");
            snprintf(msg, msg_size, "Naloxone: breathing returns. It wears off in an hour; morphine may not");
        } else {
            snprintf(msg, msg_size, "Naloxone: no opioid to reverse");
        }
        return 1;
    }
    if (h->stomach_water > 1.5f) {
        snprintf(msg, msg_size, "Too full to drink");
        return 0;
    }
    if (!inv_take(inv, I_SPIRITS, 1)) {
        snprintf(msg, msg_size, "No spirits (4 apples and glass over a fire)");
        return 0;
    }
    h->alcohol_gut += DRINK_G;
    h->stomach_water += 0.04f;
    float bac = h->bac + h->alcohol_gut / WIDMARK;
    snprintf(msg, msg_size,
             bac > 2.0f ? "Another drink: that's far too much" : (bac > 0.8f ? "Another drink" : "A drink of brandy"));
    return 1;
}

static int treat_shock(health *h, char *msg, size_t msg_size)
{
    if (h->defib.phase == DEFIB_OFF) {
        snprintf(msg, msg_size, "Put the defibrillator pads on first");
        return 0;
    }
    /* The override skips the analysis that keeps an AED from shocking a
     * beating heart. */
    health_shock(h);
    h->defib.shocks++;
    h->defib.since_shock = 0.0f;
    snprintf(msg, msg_size, health_rhythm_perfusing(h->rhythm) ? "Shocked a beating heart" : "Shock delivered");
    return 1;
}

/* Doses of the painkiller (1 g each) the liver takes in a day: 4, down to
 * 2 after days of heavy drinking. */
static float pk_limit(const health *h) { return 4.0f - 2.0f * ramp(h->drink_load, 60.0f, 200.0f); }

int health_treat(health *h, inventory *inv, int part, int what, int water_nearby, char *msg, size_t msg_size)
{
    if (h->dead) {
        snprintf(msg, msg_size, "You are dead");
        return 0;
    }
    if (!health_awake(h)) {
        snprintf(msg, msg_size, "You are unconscious");
        return 0;
    }
    if (part < 0 || part >= BP_COUNT) part = BP_CHEST;
    const char *pn = PART_NAMES_LC[part];

    switch (what) {
    case TREAT_BANDAGE: {
        int need = 0;
        for (int i = 0; i < h->wound_count; i++) {
            const wound *w = &h->wounds[i];
            if (w->part == part && (!w->bandage || w->bandage_age > 24.0f || w->bleed > 5.0f)) need = 1;
        }
        if (!need) {
            snprintf(msg, msg_size, "No open wound to dress on the %s", pn);
            return 0;
        }
        int kind = 0;
        if (inv_take(inv, I_BANDAGE, 1)) {
            kind = BANDAGE_STERILE;
        } else if (inv_count(inv, I_FIBRE) >= 2) {
            inv_take(inv, I_FIBRE, 2);
            kind = BANDAGE_IMPROVISED;
        } else {
            snprintf(msg, msg_size, "No bandages (2 plant fibre make one)");
            return 0;
        }
        int burns = 0, others = 0;
        for (int i = 0; i < h->wound_count; i++) {
            wound *w = &h->wounds[i];
            if (w->part != part) continue;
            w->bandage = (uint8_t)kind;
            w->bandage_age = 0.0f;
            if (kind == BANDAGE_IMPROVISED) w->contamination = minf(1.0f, w->contamination + 0.08f);
            if (w->kind == WOUND_BURN) burns++;
            else others++;
        }
        snprintf(msg, msg_size, "%s the %s (%s)", burns && !others ? "Dressed the burn on" : "Pressure dressing on", pn,
                 kind == BANDAGE_STERILE ? "sterile" : "improvised");
        return 1;
    }
    case TREAT_SPLINT: {
        body_part *p = &h->part[part];
        if (!is_limb(part) && part != BP_ABDOMEN) {
            snprintf(msg, msg_size, "The %s can't be splinted", pn);
            return 0;
        }
        if ((!p->fracture && p->sprain < 0.05f) || p->splinted) {
            snprintf(msg, msg_size, p->splinted ? "The %s is already splinted" : "Nothing to splint on the %s", pn);
            return 0;
        }
        int have = inv_take(inv, I_SPLINT, 1);
        if (!have && inv_count(inv, I_STICK) >= 2 && inv_count(inv, I_FIBRE) >= 1) {
            inv_take(inv, I_STICK, 2);
            inv_take(inv, I_FIBRE, 1);
            have = 1;
        }
        if (!have) {
            snprintf(msg, msg_size, "No splint (2 sticks + 1 fibre make one)");
            return 0;
        }
        p->splinted = 1;
        snprintf(msg, msg_size, part == BP_ABDOMEN ? "Pelvic binder tied" : "Splinted the %s", pn);
        return 1;
    }
    case TREAT_DISINFECT: {
        int any = 0;
        for (int i = 0; i < h->wound_count; i++) any |= h->wounds[i].part == part;
        if (!any) {
            snprintf(msg, msg_size, "No wound on the %s", pn);
            return 0;
        }
        float c_mul = 0.0f, i_mul = 0.0f;
        if (inv_take(inv, I_ANTISEPTIC, 1)) {
            c_mul = 0.15f;
            i_mul = 0.5f;
        } else if (inv_take(inv, I_SPIRITS, 1)) {
            /* 40% ethanol kills bacteria on contact, though less surely
             * than a 70% antiseptic. */
            c_mul = 0.3f;
            i_mul = 0.7f;
        } else if (water_nearby) {
            c_mul = 0.5f;
            i_mul = 1.0f;
        } else {
            snprintf(msg, msg_size, "No antiseptic or spirits, and no water to rinse with");
            return 0;
        }
        for (int i = 0; i < h->wound_count; i++) {
            wound *w = &h->wounds[i];
            if (w->part != part) continue;
            w->contamination *= c_mul;
            /* Surface antisepsis only reaches an early, shallow infection. */
            w->infection *= w->infection < 0.35f ? i_mul : 0.9f;
        }
        h->part[part].pain = minf(PAIN_SCALE, h->part[part].pain + 2.0f);
        snprintf(msg, msg_size,
                 c_mul < 0.2f
                     ? "Cleaned the %s wounds with antiseptic"
                     : (c_mul < 0.4f ? "Cleaned the %s wounds with spirits" : "Rinsed the %s wounds with water"),
                 pn);
        return 1;
    }
    case TREAT_PAINKILLER: {
        if (!inv_take(inv, I_PAINKILLER, 1)) {
            snprintf(msg, msg_size, "No painkillers left");
            return 0;
        }
        /* Paracetamol-type: about 4 g a day is safe. Past that, the liver's
         * glutathione runs out and a toxic metabolite kills its cells, a day
         * or two later, not at once. */
        h->pk_load += 1.0f;
        /* Steady drinking induces the enzyme (CYP2E1) that makes the toxic
         * metabolite and drains glutathione: the safe dose halves. */
        float limit = pk_limit(h);
        if (h->pk_load > limit) {
            h->hepatotox = minf(1.0f, h->hepatotox + 0.12f * (h->pk_load - limit));
            health_log(h, limit < 4.0f ? "Painkiller on top of heavy drinking: the liver will suffer"
                                       : "Over 4 g of painkiller in a day: the liver will suffer");
        }
        h->painkiller = 6.0f;
        snprintf(msg, msg_size,
                 h->pk_load > limit ? "Took a painkiller (6 h). Too many today" : "Took a painkiller (6 h)");
        return 1;
    }
    case TREAT_ANTIBIOTIC: {
        if (!inv_take(inv, I_ANTIBIOTIC, 1)) {
            snprintf(msg, msg_size, "No antibiotics left");
            return 0;
        }
        float worst = h->sepsis;
        for (int i = 0; i < h->wound_count; i++) worst = maxf(worst, h->wounds[i].infection);
        h->antibiotic = 7.0f * 24.0f;
        if (worst < 0.05f) {
            /* Nothing to treat: they only clear the gut's own bacteria. */
            h->gut_upset = 12.0f;
            h->organ[ORG_GUT] = maxf(0.0f, h->organ[ORG_GUT] - 0.03f);
            snprintf(msg, msg_size, "No infection to treat: the antibiotics upset your gut");
            return 1;
        }
        snprintf(msg, msg_size, "Started a 7-day course of antibiotics");
        return 1;
    }
    case TREAT_EAT: {
        /* The food in hand, else the best there is. */
        static const item_id PREF[] = {I_COOKED_MEAT, I_APPLE, I_RAW_MEAT};
        const item_stack *held = inv_held(inv);
        item_id food = held->count && item_food(held->id, NULL) > 0.0f ? held->id : 0;
        for (size_t k = 0; !food && k < sizeof PREF / sizeof PREF[0]; k++)
            if (inv_count(inv, PREF[k]) > 0) food = PREF[k];
        if (!food) {
            snprintf(msg, msg_size, "No food (apples drop from leaves)");
            return 0;
        }
        if (h->stomach_kcal > 1200.0f) {
            snprintf(msg, msg_size, "Too full to eat");
            return 0;
        }
        float water = 0.0f, kcal = item_food(food, &water);
        inv_take(inv, food, 1);
        h->stomach_kcal += kcal;
        h->stomach_water += water;
        snprintf(msg, msg_size, "Ate %s (%.0f kcal)", food == I_APPLE ? "an apple" : "meat", (double)kcal);
        return 1;
    }
    case TREAT_DRINK:
        if (!water_nearby) {
            snprintf(msg, msg_size, "No water within reach");
            return 0;
        }
        if (h->stomach_water > 1.5f) {
            snprintf(msg, msg_size, "Too full to drink");
            return 0;
        }
        h->stomach_water += 0.25f;
        snprintf(msg, msg_size, "Drank 250 mL of water");
        return 1;
    case TREAT_COOL: {
        /* First aid for burns: cool running water carries off the heat still
         * in the skin, and cooling within about 3 hours limits how far the
         * zone around a burn goes on to die (Cuttle 2008). */
        body_part *p = &h->part[part];
        if (!p->burn && p->surface < 3.0f && p->skin < 40.0f) {
            snprintf(msg, msg_size, "No burn to cool on the %s", pn);
            return 0;
        }
        if (!water_nearby) {
            snprintf(msg, msg_size, "No water to cool the burn with");
            return 0;
        }
        p->cool_time = 30.0f;
        int in_time = !p->burn || p->burn_age < 3.0f;
        if (in_time) p->burn_cooled = 1;
        h->wet = minf(1.0f, h->wet + 0.3f * NINES[part]);
        snprintf(msg, msg_size,
                 in_time ? "Cooling the %s under water" : "Cooled the %s (too late to stop the burn deepening)", pn);
        return 1;
    }
    case TREAT_REDUCE: {
        body_part *p = &h->part[part];
        if (!p->dislocated) {
            snprintf(msg, msg_size, "Nothing out of joint on the %s", pn);
            return 0;
        }
        if (p->fracture) {
            snprintf(msg, msg_size, "The %s is broken as well: splint it, don't force the joint", pn);
            return 0;
        }
        /* Traction against the body usually slides the joint home; muscle
         * spasm from pain fights it, and a painkiller relaxes it. */
        const char *joint = p->dislocated == DISLOC_ELBOW ? "elbow" : "shoulder";
        const char *side = part == BP_LARM ? "left" : "right";
        float chance = 0.55f + (h->painkiller > 0.0f ? 0.25f : 0.0f) - 0.04f * maxf(0.0f, h->pain - 5.0f);
        p->pain_spike = PAIN_SCALE;
        h->hurt_flash = minf(1.0f, h->hurt_flash + 0.4f);
        if (frand(h) < fclamp(chance, 0.2f, 0.9f)) {
            p->dislocated = DISLOC_NONE;
            p->sprain = maxf(p->sprain, 0.35f); /* the torn capsule still needs rest */
            snprintf(msg, msg_size, "Popped the %s %s back in", side, joint);
            health_log(h, "Reduced the %s %s", side, joint);
        } else {
            snprintf(msg, msg_size, "The %s %s won't go back in. Try again (a painkiller helps)", side, joint);
        }
        return 1;
    }
    case TREAT_DEFIB:
        if (h->defib.phase != DEFIB_OFF) {
            health_pads(h, 0);
            snprintf(msg, msg_size, "Took the defibrillator pads off");
            return 1;
        }
        if (inv_count(inv, I_DEFIBRILLATOR) <= 0) {
            snprintf(msg, msg_size, "No defibrillator");
            return 0;
        }
        health_pads(h, 1);
        snprintf(msg, msg_size, "Defibrillator pads on: it shocks by itself if the heart fibrillates");
        return 1;
    case TREAT_ADRENALINE: return treat_adrenaline(h, inv, msg, msg_size);
    case TREAT_SHOCK: return treat_shock(h, msg, msg_size);
    case TREAT_MORPHINE:
    case TREAT_NALOXONE:
    case TREAT_SPIRITS: return treat_drug(h, inv, what, msg, msg_size);
    default: snprintf(msg, msg_size, "?"); return 0;
    }
}

/* ------------------------------------------------------- waveforms */

static float gauss(float t, float mu, float sigma)
{
    float d = (t - mu) / sigma;
    return expf(-0.5f * d * d);
}

/* The P wave of an atrial activation t s ago (lead II, mV). */
static float ecg_p(float t) { return t < -0.1f || t > 0.3f ? 0.0f : 0.15f * gauss(t, 0.05f, 0.025f); }

/* The QRS and T wave of a ventricular activation t s ago; rr sets the QT.
 * A beat that starts in the ventricle itself (ectopic, VT, an escape
 * below a block) spreads cell to cell: wide and bizarre, its T wave
 * turned against it. */
static float ecg_qrst(float t, float rr, int wide, float st_shift)
{
    if (t < -0.1f || t > 1.2f) return 0.0f;
    if (wide) return 1.4f * gauss(t, 0.04f, 0.03f) - 0.7f * gauss(t, 0.11f, 0.03f) - 0.45f * gauss(t, 0.34f, 0.06f);
    float qt = sqrtf(rr);
    float v = -0.12f * gauss(t, 0.005f, 0.01f) + 1.2f * gauss(t, 0.04f, 0.012f) - 0.3f * gauss(t, 0.075f, 0.012f) +
              0.33f * gauss(t, 0.04f + 0.25f * qt, 0.045f * qt);
    if (st_shift != 0.0f && t > 0.09f && t < 0.2f * qt + 0.14f) v += st_shift;
    return v;
}

/* Fills the monitor traces for this step. The arterial line and the
 * finger's pulse are the circulation's own (art_s, pleth_s: ns samples,
 * taken at the sample times as the circuit reached them); the ECG draws a
 * complex for each activation the conduction system fired. */
static void write_samples(health *h, const health_env *e, double dt, const float *art_s, const float *pleth_s, int ns)
{
    h->wave_acc += (float)(dt * HEALTH_WAVE_HZ);
    const double ds = 1.0 / HEALTH_WAVE_HZ;
    const double t0 = h->t - dt;
    const conduction *c = &h->cond;
    const int r = h->rhythm;
    const float rr = 60.0f / maxf(h->hr, 30.0f);
    const float st = h->ischemia > 1.0f ? -0.08f * minf(h->ischemia, 3.0f) : 0.0f;
    const int wide = r == RHYTHM_VT || r == RHYTHM_PVT || r == RHYTHM_AVB3;
    int k = 0;
    while (h->wave_acc >= 1.0f) {
        h->wave_acc -= 1.0f;
        double ts = t0 + ds * ++k;
        if (ts > h->t) ts = h->t;
        float ecg = 0.0f, art = 0.0f, pleth = 0.0f, capno = 0.0f;
        const float s = (float)ts;
        if (!h->dead && r != RHYTHM_VF && r != RHYTHM_ASYSTOLE) {
            double tv = ts >= c->t_vent ? c->t_vent : c->prev_vent;
            int ectopic = tv == c->t_vent && c->last_ectopic;
            if (tv >= 0.0) ecg += ecg_qrst((float)(ts - tv), rr, wide || ectopic, st);
            double ta = ts >= c->t_atria ? c->t_atria : h->atria_prev;
            if (ta >= 0.0 && cond_atria_on(r) > 0.0f) ecg += ecg_p((float)(ts - ta));
            /* AF's fibrillatory baseline. */
            if (r == RHYTHM_AF) ecg += 0.05f * sinf(37.7f * s + 0.8f * sinf(2.3f * s)) + 0.03f * sinf(51.0f * s);
        } else if (r == RHYTHM_VF && !h->dead) {
            /* Coarse VF fading to fine VF as the myocardium runs out of energy. */
            float amp = 0.55f * expf(-h->arrest_time / 240.0f) + 0.08f;
            ecg = amp * (0.6f * sinf(25.8f * s + 1.3f * sinf(1.7f * s)) + 0.4f * sinf(33.3f * s + 0.7f) +
                         0.3f * sinf(41.9f * s + 2.0f * sinf(0.9f * s)));
        }
        /* Baseline wander with breathing, and motion artefact when moving. */
        ecg += 0.04f * sinf(6.2831853f * h->breath_phase) +
               (frand(h) - 0.5f) * (0.02f + 0.03f * (float)fclamp((float)e->speed, 0.0f, 8.0f));

        if (ns > 0) {
            int j = k - 1 < ns ? k - 1 : ns - 1;
            art = art_s[j];
            pleth = pleth_s[j];
        } else {
            art = h->map;
            pleth = 0.0f;
        }
        if (h->breathing && h->rr > 0.5f) {
            float ph = h->breath_phase;
            if (ph < 0.4f) capno = h->etco2 * (1.0f - ramp(ph, 0.0f, 0.06f));
            else capno = h->etco2 * (0.85f * ramp(ph, 0.4f, 0.5f) + 0.15f * (ph - 0.4f) / 0.6f);
        } else {
            capno = 0.0f;
        }
        int i = h->wave_pos;
        h->ecg[i] = ecg;
        h->art[i] = art;
        h->pleth[i] = pleth;
        h->capno[i] = capno;
        h->wave_pos = (i + 1) % HEALTH_WAVE_LEN;
        if (h->breathing) {
            h->breath_phase += (float)(ds * (double)h->rr / 60.0);
            if (h->breath_phase >= 1.0f) h->breath_phase -= 1.0f;
        }
    }
}

/* ---------------------------------------------------------------- step */

static int arrest_cause(const health *h, int airway)
{
    if (airway == AIRWAY_WATER && h->sao2 < 0.7f) return DEATH_DROWNING;
    if ((airway == AIRWAY_BLOCKED || h->part[BP_CHEST].crush_load > 60.0f) && h->sao2 < 0.7f) return DEATH_SUFFOCATION;
    if (h->burn_tbsa > 10.0f && h->blood < health_blood_normal(h) * 0.8f) return DEATH_BURNS;
    if (h->ich > 25.0f || h->icp > 40.0f) return DEATH_HEAD_INJURY;
    if (h->blood < health_blood_normal(h) * 0.72f) return DEATH_BLOOD_LOSS;
    if (h->sepsis > 0.6f) return DEATH_SEPSIS;
    if (h->temp < 30.0f) return DEATH_HYPOTHERMIA;
    if (h->temp > 41.5f) return DEATH_HYPERTHERMIA;
    if (dehydration(h) > 0.12f) return DEATH_DEHYDRATION;
    if (h->lung_water > 0.3f) return DEATH_DROWNING;
    if (health_resp_depression(h) > 0.5f) return DEATH_OVERDOSE;
    return DEATH_CARDIAC;
}

static void die(health *h, int cause)
{
    if (h->dead) return;
    h->dead = 1;
    h->cause = cause;
    h->rhythm = RHYTHM_ASYSTOLE;
    h->conscious = CONS_UNCONSCIOUS;
    h->level = 0.0f;
    health_log(h, "%s", health_death_text(cause));
}

static void stop_heart(health *h, int rhythm, int airway)
{
    if (!health_rhythm_perfusing(h->rhythm)) return;
    h->rhythm = rhythm;
    h->arrest_time = 0.0f;
    h->cause = arrest_cause(h, airway);
    switch (rhythm) {
    case RHYTHM_VF: health_log(h, "Ventricular fibrillation!"); break;
    case RHYTHM_PVT: health_log(h, "Pulseless ventricular tachycardia!"); break;
    case RHYTHM_PEA: health_log(h, "No pulse: electrical activity without output (PEA)"); break;
    default: health_log(h, "Heart stopped (asystole)"); break;
    }
}

/* A change between rhythms that keep a pulse, or back to sinus. The
 * conduction system (conduction.c) takes it from the next event. */
static void set_rhythm(health *h, int rhythm)
{
    if (h->rhythm == rhythm || !health_rhythm_perfusing(h->rhythm)) return;
    h->rhythm = rhythm;
    if (rhythm == RHYTHM_SINUS) health_log(h, "The heart is back in sinus rhythm");
    else health_log(h, "%s", RHYTHM_NAMES[rhythm]);
}

/* Crouching keeps the head near the heart and squeezes the leg veins: it
 * eases the reflex. Lying flat (out cold) ends it. */
static void step_vagal(health *h, const health_env *e, double dt, int awake)
{
    float vagal_t = 0.0f;
    if (h->faint_drive > 0.0f && awake) vagal_t = e->crouched ? 0.45f : 1.0f;
    h->vagal = lag(h->vagal, vagal_t, dt, vagal_t > h->vagal ? 4.0 : 5.0);
    h->faint_drive = maxf(0.0f, h->faint_drive - (float)dt);
}

/* Sets off the vasovagal reflex unless it is already running. Returns 1
 * if it started. */
static int start_faint(health *h)
{
    if (h->faint_drive > 0.0f || h->vagal > 0.1f) return 0;
    h->faint_drive = FAINT_S;
    return 1;
}

/* Where each part's wounds open into the circuit: the bed whose artery a
 * cut artery bleeds from, and the veins a venous cut drains. */
static int part_bed(int part)
{
    if (is_leg(part)) return CB_LEGS;
    if (part == BP_ABDOMEN) return CB_SPLANCH;
    return part == BP_CHEST ? -1 : CB_UPPER;
}

static int part_vein(int part)
{
    if (is_leg(part)) return CC_LEGS;
    if (part == BP_ABDOMEN) return CC_SPLANCH;
    return part == BP_CHEST ? CC_SVC : CC_UPPER;
}

/* mmHg a wound's vessel normally holds standing, the posture a wound's
 * rate (bleed0) is given for: the column of blood below the heart adds
 * to the legs' and the gut's. Lying down, or raising the limb, lowers
 * it, and the wound bleeds less. */
static float vein_ref(int vein)
{
    if (vein == CC_LEGS) return 45.0f;
    if (vein == CC_SPLANCH) return 16.0f;
    return vein == CC_UPPER ? 6.0f : 4.0f;
}

static float bed_ref(int bed) { return 93.0f + (bed == CB_LEGS ? 47.0f : bed == CB_SPLANCH ? 16.0f : 0.0f); }

/* Wounds bleed through the circulation: each clots (less when the limb
 * is used, more when dressed) and becomes an opening of some conductance,
 * sized so it bleeds bleed0 at the pressure its vessel normally holds
 * standing. What actually comes out is then the circuit's: it falls as
 * pressure falls, and a leg bleeds harder standing than raised. Broken
 * bones and torn organs bleed inside, into the gut's veins. Returns the
 * conductances in `in`. */
static void step_bleeding(health *h, const health_env *e, double dt, float gh, circ_input *in)
{
    const float fdt = (float)dt;
    float coag = sqrtf(h->organ[ORG_LIVER]) *
                 (h->temp < 35.0f ? fclamp(1.0f - (35.0f - h->temp) * 0.15f, 0.3f, 1.0f) : 1.0f) *
                 fclamp(hct(h) / 0.3f, 0.3f, 1.0f) * (1.0f - 0.5f * h->sepsis) * h->base.clot_gain;
    float ext = 0.0f;
    for (int i = 0; i < h->wound_count; i++) {
        wound *w = &h->wounds[i];
        float bf = 1.0f, tau = 0.0f;
        if (w->bandage == BANDAGE_STERILE) bf = w->arterial ? 0.35f : 0.08f;
        else if (w->bandage == BANDAGE_IMPROVISED) bf = w->arterial ? 0.5f : 0.15f;
        if (w->arterial) tau = w->bandage ? 600.0f : 1e9f;
        else tau = (60.0f + 400.0f * w->depth) / maxf(coag, 0.05f) / (w->bandage ? 2.0f : 1.0f);
        w->clot += (1.0f - w->clot) * (float)(1.0 - exp(-dt / (double)tau));
        /* Using the limb tears the clot off an undressed wound. */
        if (!w->bandage) {
            if (is_leg(w->part) && e->speed > 2.0) w->clot *= 1.0f - 0.02f * fdt * (float)e->speed;
            if (w->part == BP_RARM && e->actions) w->clot *= 0.85f;
        }
        float rate = w->bleed0 * (1.0f - w->clot) * bf * (1.0f - w->closure); /* mL/min at the reference pressure */
        float g = 0.0f, p = 0.0f; /* mL/s/mmHg, and the pressure driving it out */
        int bed = part_bed(w->part);
        if (w->arterial && bed >= 0) {
            g = rate / 60.0f / bed_ref(bed);
            in->drain_art[bed] += g;
            /* The circuit drains the bed's artery at its own height. */
            float h_bed = bed == CB_LEGS ? in->h_legs : bed == CB_SPLANCH ? in->h_splanch : 0.0f;
            p = h->cv.p[bed == CB_UPPER ? CC_AO : CC_AA] - 78.0f * h_bed;
        } else {
            int vein = w->arterial ? CC_AO : part_vein(w->part);
            g = rate / 60.0f / (w->arterial ? 93.0f : vein_ref(vein));
            in->drain[vein] += g;
            p = h->cv.p[vein];
        }
        w->bleed = g * maxf(p, 0.0f) * 60.0f;
        ext += w->bleed;
    }
    /* A fresh rush of blood is what sets off the blood-injury faint; blood
     * that has been running a while no longer does. A cut artery's flow
     * builds over several beats; only a bleed at least doubling what the
     * eyes have already seen is a new sight, so one rush is one chance. */
    if (health_awake(h) && ext > 2.0f * h->blood_seen + 30.0f) {
        if (frand(h) < 0.15f && start_faint(h)) health_log(h, "The sight of the blood makes you woozy");
        h->blood_seen = ext;
    }
    /* Getting used to it is a reflex of the moment, in real seconds. */
    h->blood_seen = maxf(ext, h->blood_seen * expf(-fdt / 25.0f));
    float internal = 0.0f;
    for (int i = 0; i < BP_COUNT; i++) {
        body_part *bp = &h->part[i];
        if (bp->internal <= 0.0f) continue;
        /* Tamponade and clotting slowly contain bleeding into tissue. */
        bp->internal *= (float)exp(-dt * (double)coag / (300.0 + 3.0 * (double)bp->internal));
        if (is_leg(i) && bp->fracture && !bp->splinted && e->speed > 0.5) bp->internal += 0.3f * (float)e->speed * fdt;
        if (bp->internal < 0.05f) bp->internal = 0.0f;
        float g = bp->internal / 60.0f / vein_ref(CC_SPLANCH);
        in->drain[CC_SPLANCH] += g;
        internal += g * maxf(h->cv.p[CC_SPLANCH], 0.0f) * 60.0f;
    }
    h->bleed_ext = ext;
    h->bleed_int = internal;
    h->ich += h->ich_rate * fdt / 60.0f;
    h->ich_rate *= (float)exp(-dt / 600.0);
    if (h->rbc < health_blood_normal(h) * 0.45f) h->rbc += 0.02f / 24.0f * gh * h->organ[ORG_KIDNEYS];
}

/* Blood the circuit lost to wounds takes its red cells and water with it;
 * plasma refills from the tissues over hours (Starling forces), on the
 * survival clock, which dilutes the red cells that are left. The
 * capillaries can only move so much fluid, though: a few hundred mL in
 * the first hour, about 0.25 mL/kg/min at the most, in real time. Without
 * that cap the survival clock's speed-up refilled hundreds of mL a minute
 * and an open artery never emptied the body. */
#define REFILL_MAX_ML_S (0.25f * 75.0f / 60.0f)
static void account_blood(health *h, float lost_ml, float gh, float dehyd, float dt)
{
    float lost = lost_ml / 1000.0f;
    h->rbc -= lost * hct(h);
    h->water -= lost * 0.8f;
    float vol = circ_volume(&h->cv);
    h->plasma_deficit = maxf(0.0f, h->base.blood_per_kg * 75.0f - vol);
    /* Past what the nerves alone can hold, about a sixth of the blood,
     * vasopressin and renin's angiotensin II pour out over a minute or
     * two and carry the vessels' tone through a third lost (Schadt and
     * Ludbrook 1991, see CREDITS.md); they linger for many minutes. A
     * pressure back up at the baroreceptors shuts their release off. */
    float want = maxf(ramp(h->plasma_deficit / (h->base.blood_per_kg * 75.0f), 0.04f, 0.25f) *
                          (1.0f - ramp(h->map, 95.0f, 120.0f)),
                      1.0f - ramp(h->map, 50.0f, 80.0f));
    h->pressor = lag(h->pressor, want, (double)dt, want > h->pressor ? 60.0 : 900.0);
    if (h->plasma_deficit > 0.0f && dehyd < 0.15f) {
        float refill = minf(h->plasma_deficit * (1.0f - expf(-gh / 8.0f)), REFILL_MAX_ML_S * dt);
        circ_add(&h->cv, CC_IVC, refill);
        h->water -= refill * 0.0002f; /* the interstitium pays (L), which shows up as thirst */
    }
    h->blood = circ_volume(&h->cv) / 1000.0f;
    if (h->rbc < 0.05f) h->rbc = 0.05f;
    if (h->rbc > h->blood * 0.9f) h->rbc = h->blood * 0.9f;
}

/* Relative closure speed: burns re-epithelialise from the edges and from
 * surviving hair follicles in 2-3 weeks, full-thickness ones and dead
 * frostbitten tissue much slower. */
static float wound_heal_rate(const wound *w)
{
    switch (w->kind) {
    case WOUND_OPEN_FRACTURE: return 0.5f;
    case WOUND_BURN: return w->depth > 0.6f ? 0.15f : 0.4f;
    case WOUND_FROSTBITE: return 0.2f;
    default: return 1.0f;
    }
}

/* Contamination, infection, sepsis and wound closure, on the survival clock. */
static void step_wounds(health *h, const health_env *e, float gh, float dehyd)
{
    float max_inf = 0.0f;
    float immune = 0.04f * (h->glycogen < 50.0f ? 0.5f : 1.0f) * (1.0f - fclamp(dehyd * 4.0f, 0.0f, 0.5f)) *
                   (1.0f - 0.5f * h->sepsis);
    float abx = h->antibiotic > 0.0f ? 0.3f : 0.0f;
    for (int i = 0; i < h->wound_count; i++) {
        wound *w = &h->wounds[i];
        w->bandage_age += gh;
        /* An old sterile dressing is as dirty as none; a cloth one lets a little in. */
        if (!w->bandage || (w->bandage == BANDAGE_STERILE && w->bandage_age > 24.0f)) w->contamination += 0.01f * gh;
        else if (w->bandage == BANDAGE_IMPROVISED) w->contamination += 0.004f * gh;
        if (e->submerged > 0.3) w->contamination += 0.2f * gh;
        w->contamination = fclamp(w->contamination, 0.0f, 1.0f);
        float seed = w->contamination > 0.1f ? w->contamination * 0.02f : 0.0f;
        float grow = 0.12f * w->infection * (1.0f - w->infection) * fclamp(w->contamination / 0.3f, 0.0f, 1.5f);
        w->infection = fclamp(w->infection + (seed + grow - (immune + abx) * w->infection) * gh, 0.0f, 1.0f);
        if (w->infection > 0.3f && w->infection - (seed + grow) * gh <= 0.3f)
            health_log(h, "Wound on the %s is infected", PART_NAMES_LC[w->part]);
        max_inf = maxf(max_inf, w->infection);
        if (w->bleed < 1.0f)
            w->closure += gh / (24.0f * (3.0f + 7.0f * w->depth)) * (w->infection < 0.3f ? 1.0f : 0.3f) *
                          (w->bandage ? 1.2f : 1.0f) * wound_heal_rate(w);
        if (w->closure >= 1.0f) { h->wounds[i--] = h->wounds[--h->wound_count]; }
    }
    float sepsis_t = fclamp((max_inf - 0.45f) / 0.4f, 0.0f, 1.0f);
    h->sepsis = fclamp(h->sepsis + (sepsis_t - h->sepsis) * minf(1.0f, 0.1f * gh), 0.0f, 1.0f);
}

/* ---- heat and cold
 *
 * Each part has a shell (skin and the tissue under it) between the core and
 * the surroundings. Blood carries heat from the core into the shell with a
 * conductance set by vasomotor tone; the shell loses it through clothing to
 * the air by convection (h_c = 3.1 + 8.3 sqrt(v) W/m^2K, still air to
 * wind) and radiation (4.7 W/m^2K), to water when submerged, to the ground
 * through the soles, and by evaporating sweat and the water in wet clothes.
 * Clothing loses most of its insulation when soaked. Fires add radiant heat
 * on the side facing them (0.6 m^2 of the body) and, standing in one,
 * flames at ~800 C that climb from the feet.
 *
 * Calibration: at rest in still 20 C air, clothed, the skin carries off
 * the ~68 W the old whole-body model lost (0.8 of resting metabolism; the
 * rest leaves by breathing and diffusion through the skin) with the vessels
 * mostly constricted, so the core stays at 37 C. In water the core still
 * loses heat through the calibrated whole-body immersion conductance
 * (25 W/K); the shells follow the water so a swimmer's hands still chill.
 *
 * Cold: vessels in the hands and feet close first (they also shut with
 * local cold), so the limbs drift toward the air temperature, broken by
 * cold-induced vasodilation only while the core and the rest of the skin
 * are still warm. Tissue freezes below
 * -0.5 C: frostnip, then superficial and deep frostbite with time frozen.
 *
 * Heat: a thin burn layer over each shell takes the flux on the skin facing
 * the source and heats in seconds; its temperature drives the
 * Henriques-Moritz damage integral. */

static float sat_vp(float t) /* kPa, Tetens over water, and over ice below 0 C */
{
    return t >= 0.0f ? 0.6108f * expf(17.27f * t / (t + 237.3f)) : 0.6108f * expf(21.87f * t / (t + 265.5f));
}

/* How much of a part is under water when `s` of the body is (feet first). */
static float part_submerged(int part, float s)
{
    switch (part) {
    case BP_LLEG:
    case BP_RLEG: return fclamp(s / 0.45f, 0.0f, 1.0f);
    case BP_ABDOMEN: return fclamp((s - 0.4f) / 0.2f, 0.0f, 1.0f);
    case BP_HEAD: return fclamp((s - 0.85f) / 0.15f, 0.0f, 1.0f);
    default: return fclamp((s - 0.55f) / 0.3f, 0.0f, 1.0f);
    }
}

/* W/m^2 from flames onto skin at ts: convection from the hot gas
 * (h ~ 25 W/m^2K) plus radiation from a sooty flame of emissivity ~0.3,
 * about 40 kW/m^2 onto cool skin. */
static float flame_flux(float ts)
{
    const double tg = FLAME_C + 273.15, tk = (double)ts + 273.15;
    return (float)(25.0 * (tg - tk) + 0.3 * 5.67e-8 * (tg * tg * tg * tg - tk * tk * tk * tk));
}

float health_part_area(int part) { return part >= 0 && part < BP_COUNT ? NINES[part] : 0.0f; }

float health_skin_mean(const health *h)
{
    float t = 0.0f;
    for (int i = 0; i < BP_COUNT; i++) t += NINES[i] * h->part[i].skin;
    return t;
}

/* Frostbite from how long, and how far below freezing, the tissue has
 * been frozen; frostnip only freezes the surface and clears within a
 * minute or so of thawing. */
static void step_frost(health *h, int i, float fdt)
{
    body_part *bp = &h->part[i];
    if (bp->skin < -0.5f) bp->freeze_time += fdt * (1.0f + (-0.5f - bp->skin) / 3.0f);
    else if (bp->frost <= FROST_NIP && bp->skin > 5.0f) bp->freeze_time = maxf(0.0f, bp->freeze_time - 3.0f * fdt);
    int stage = bp->freeze_time > 900.0f
                    ? FROST_DEEP
                    : (bp->freeze_time > 240.0f ? FROST_SUPERFICIAL : (bp->freeze_time > 15.0f ? FROST_NIP : 0));
    if (stage > bp->frost) {
        bp->frost = (uint8_t)stage;
        bp->frost_heal = 0.0f;
        if (stage == FROST_NIP) health_log(h, "Frostnip: the %s is white and numb", PART_NAMES_LC[i]);
        else if (stage == FROST_SUPERFICIAL) health_log(h, "Frostbite on the %s", PART_NAMES_LC[i]);
        else {
            /* Frozen through: the tissue dies and becomes an open, dirty wound. */
            bp->integrity = minf(bp->integrity, 0.5f);
            if (!part_wound(h, i, WOUND_FROSTBITE)) add_wound(h, i, WOUND_FROSTBITE, 0.7f, 0, 0.25f)->bleed0 = 0.0f;
            health_log(h, "Deep frostbite: the %s is frozen through", PART_NAMES_LC[i]);
        }
    } else if (bp->frost == FROST_NIP && bp->freeze_time <= 0.0f) {
        bp->frost = FROST_NONE;
        health_log(h, "Feeling returns to the %s", PART_NAMES_LC[i]);
    }
}

/* Heat gets into the burn layer faster than it leaves: the damage integral
 * runs above 44 C, and the area burned is what faced the source. */
static void step_burn_layer(health *h, int i, const health_env *e, float fdt, float si, float tau_cl)
{
    body_part *bp = &h->part[i];
    float ts = bp->skin, td = ts + bp->surface, g = 0.0f, gt = 0.0f, area = 0.0f;
    float tau = BARE[i] > 0.0f ? 1.0f : tau_cl; /* the most exposed skin of the part */
    float q = (float)e->radiant * ABSORB * tau;
    if (q > 1500.0f) area = 0.5f * (BARE[i] > 0.0f ? BARE[i] : 1.0f);
    if (e->in_fire) {
        q += FLAMES[i] * flame_flux(td) * (BARE[i] > 0.0f ? 1.0f : 0.7f); /* burning cloth protects little */
        area = maxf(area, FLAMES[i]);
    }
    if (is_leg(i) && e->contact_temp > (double)td && si < 0.5f) {
        g += SOLE_G;
        gt += SOLE_G * (float)e->contact_temp;
        if (e->contact_temp > 50.0) area = maxf(area, 0.06f);
    }
    float cool = bp->cool_time > 0.0f ? COOL_G : 150.0f * si;
    g += cool;
    gt += cool * (float)e->water_temp;
    /* Implicit step of C dS/dt = q + sum g (T - ts - S) - G S. */
    bp->surface = (BURN_C * bp->surface + fdt * (q + gt - g * ts)) / (BURN_C + fdt * (BURN_G + g));
    td = ts + bp->surface;
    if (td > 44.0f) {
        double rate = exp(fmin(HM_LN_P - HM_E_R / ((double)td + 273.15), 30.0));
        bp->burn_omega = (float)fmin((double)bp->burn_omega + rate * (double)fdt, 1e6);
        if (td > 46.0f && area > bp->burn_area) bp->burn_area = area;
        burn_degree(h, i);
    }
}

/* One step of the whole heat balance. p is metabolic power (W); returns
 * the litres of sweat evaporated. */
static float step_heat(health *h, const health_env *e, double dt, float gh, float p)
{
    const float fdt = (float)dt;
    const float ta = (float)e->air_temp, tw = (float)e->water_temp, tg = (float)e->contact_temp, tc = h->temp;
    const float sub = fclamp((float)e->submerged, 0.0f, 1.0f);
    /* Air speed over the skin: the body's own motion and the wind. */
    const float v = fclamp((float)sqrt(e->speed * e->speed + e->wind * e->wind), 0.0f, 40.0f);
    const float rad = maxf((float)e->radiant, 0.0f);
    const float hc = 3.1f + 8.3f * sqrtf(v), hcr = hc + H_RAD;
    const float set = 37.0f + 2.5f * h->sepsis;
    const float tsk = health_skin_mean(h);
    h->air_temp = ta;
    h->radiant = rad;
    h->in_fire = e->in_fire;

    /* Thermoregulation: skin vessels open as the core warms and close as it
     * or the skin cools; sweating starts just above the set point. */
    /* Alcohol opens the skin's vessels whatever the core needs: the warm
     * glow of a drink is heat leaving the body. */
    float vaso_t = fclamp(0.1f + 2.5f * (tc - set) + 0.06f * (tsk - 32.0f) + 0.8f * ramp(h->bac, 0.2f, 1.0f), 0.0f,
                          1.0f);
    h->vaso = lag(h->vaso, vaso_t, dt, 20.0);
    float sweat = fclamp(800.0f * (tc - set - 0.1f + 0.1f * (tsk - 34.0f)), 0.0f, 900.0f) * (1.0f - sub);
    h->sweat_w = sweat;

    /* Wet clothes: soaked in seconds under water; they dry by evaporation,
     * faster in warm, dry, moving air (Lewis relation: h_e = 16.5 h_c
     * W/m^2kPa) and when a fire's heat drives the water off. Drying runs
     * on the survival clock like the rest of the slow processes. */
    float evap = 0.0f;
    if (sub > h->wet) {
        h->wet = lag(h->wet, sub, dt, 2.0);
    } else if (h->wet > 0.0f) {
        float tev = ta + 0.4f * (tsk - ta);
        float dp = maxf(0.0f, sat_vp(tev) - AIR_RH * sat_vp(ta));
        evap = (16.5f * hc * dp * CLOTHED_AREA * h->wet + 0.5f * rad * ABSORB * 0.55f * minf(1.0f, 3.0f * h->wet)) *
               (1.0f - sub);
        h->wet -= evap / LATENT / CLOTHES_WATER * gh * 3600.0f;
        if (h->wet < 0.01f) h->wet = 0.0f;
    }

    float core_out = 0.0f, flow = fclamp(h->co / 3.0f, 0.05f, 1.0f);
    for (int i = 0; i < BP_COUNT; i++) {
        body_part *bp = &h->part[i];
        const float a = NINES[i] * BODY_AREA, si = part_submerged(i, sub);
        const float r_cl = CLO_R[i] * (1.0f - 0.8f * h->wet);
        const float fcl = r_cl <= 0.078f ? 1.0f + 1.29f * r_cl : 1.05f + 0.645f * r_cl; /* ISO 9920 area factor */
        const float r_out = 1.0f / (fcl * hcr);
        const float u_air = 1.0f / (r_cl + r_out);
        /* The share of heat absorbed on the clothing that reaches the skin. */
        const float tau_cl = BARE[i] + (1.0f - BARE[i]) * r_out / (r_cl + r_out);

        /* Skin blood flow. Limbs also constrict with local cold, then open
         * again in cold-induced vasodilation while the core is warm; local
         * heat flushes the skin with blood. */
        float dil = h->vaso, k = 0.0f;
        if (is_limb(i)) {
            dil *= 0.3f + 0.7f * ramp(bp->skin, 8.0f, 26.0f);
            k = K_MIN[i] + (K_MAX[i] - K_MIN[i]) * dil +
                3.0f * ramp(12.0f - bp->skin, 0.0f, 6.0f) * ramp(tc, 36.3f, 37.0f) * ramp(tsk, 20.0f, 28.0f);
        } else {
            k = K_MIN[i] + (K_MAX[i] - K_MIN[i]) * dil;
        }
        k = (k + 250.0f * ramp(bp->skin, 38.0f, 43.0f)) * flow;

        /* Conductances (W/K) to the core, air, water, ground and cooling water. */
        float g_core = k * a, g_air = u_air * a * (1.0f - si);
        float g_water = a * si / (r_cl + 1.0f / (100.0f + 200.0f * sqrtf(v)));
        float g_ground = is_leg(i) ? SHOE_W_K * (1.0f - si) : 0.0f;
        float g_cool = bp->cool_time > 0.0f ? COOL_G * 0.3f * a : 0.0f;
        /* Sources (W): fire, and the part's share of evaporation. */
        float q = rad * ABSORB * PROJ[i] * tau_cl;
        if (e->in_fire) q += FLAMES[i] * a * flame_flux(bp->skin + bp->surface) * (BARE[i] + (1.0f - BARE[i]) * 0.7f);
        q -= sweat * a / BODY_AREA * (1.0f - si);
        q -= 0.6f * evap * (1.0f - BARE[i]) * a / CLOTHED_AREA;
        float c = SHELL_C[i] * a;
        float gsum = g_core + g_air + g_water + g_ground + g_cool;
        float num = c * bp->skin + fdt * (g_core * tc + g_air * ta + (g_water + g_cool) * tw + g_ground * tg + q);
        bp->skin = num / (c + fdt * gsum);
        core_out += g_core * (tc - bp->skin) * (1.0f - si);

        if (bp->cool_time > 0.0f) bp->cool_time = maxf(0.0f, bp->cool_time - fdt);
        if (!e->invulnerable) {
            step_burn_layer(h, i, e, fdt, si, tau_cl);
            step_frost(h, i, fdt);
        } else {
            bp->surface = 0.0f;
        }
    }

    /* The core: 0.8 of metabolic heat reaches the skin; water takes heat
     * straight from the core through the whole-body immersion conductance. */
    float immersion = 25.0f * sub * (tc - tw);
    h->heat_loss = core_out + immersion;
    h->temp += (0.8f * p - core_out - immersion) / HEAT_CAP * fdt;
    /* Fever: chills and shut-down skin raise the core toward a raised set
     * point within hours, which on the survival clock is minutes. (Only
     * the fever's part: this does not rewarm a hypothermic body.) */
    float fever = set - maxf(h->temp, 37.0f);
    if (fever > 0.05f) h->temp += fever * minf(1.0f, gh / 1.5f);
    return sweat / LATENT * fdt;
}

/* Burns on the survival clock: an uncooled burn deepens for the first
 * hours as the zone around it dies; deep burns leak plasma into the
 * tissue in proportion to their area, front-loaded, and past ~20% of the
 * body the leak turns body-wide. Parkland's 4 mL/kg per % of body surface
 * in the first day also refills the swollen tissue; about half of it
 * (2 mL/kg per %) leaves the circulation faster than the interstitium
 * refills it. Burns heal as their wounds close. Frostbite heals here too. */
static void step_burns(health *h, float gh)
{
    float tbsa = 0.0f, leak = 0.0f;
    for (int i = 0; i < BP_COUNT; i++) {
        body_part *bp = &h->part[i];
        if (bp->frost == FROST_SUPERFICIAL) {
            bp->frost_heal += gh / (24.0f * 10.0f);
            if (bp->frost_heal >= 1.0f) bp->frost = FROST_NONE;
        } else if (bp->frost == FROST_DEEP && !part_wound(h, i, WOUND_FROSTBITE)) {
            bp->frost = FROST_NONE;
        }
        if (bp->frost == FROST_NONE && bp->skin > -0.5f) bp->freeze_time = 0.0f;
        if (!bp->burn) continue;
        bp->burn_age += gh;
        if (bp->burn == 2 && !bp->burn_cooled && bp->burn_age < 3.0f) {
            bp->burn_omega = minf(1e6f, bp->burn_omega * expf(0.8f * gh));
            burn_degree(h, i);
        }
        if (bp->burn == 1) {
            if (bp->burn_age > 96.0f) bp->burn = 0; /* peels and heals in a few days */
        } else {
            wound *w = part_wound(h, i, WOUND_BURN);
            if (!w) {
                bp->burn = 0; /* closed (a third-degree scar keeps integrity capped) */
            } else {
                w->area = bp->burn_area;
                float pct = bp->burn_area * NINES[i] * 100.0f * (1.0f - w->closure);
                tbsa += pct;
                leak += 0.15f * pct * expf(-bp->burn_age / 12.0f) / 12.0f; /* L per game hour */
            }
        }
        if (!bp->burn) {
            bp->burn_omega = bp->burn_area = bp->burn_age = 0.0f;
            bp->burn_cooled = 0;
        }
    }
    h->burn_tbsa = tbsa;
    float lost = leak * (0.25f + 0.75f * ramp(tbsa, 10.0f, 25.0f)) * gh;
    h->blood -= lost;                              /* plasma only: the blood thickens */
    h->water -= 0.3f * lost + 0.002f * tbsa * gh; /* and evaporation from the open surface */
}

/* Crush syndrome. A weight lifted off dead muscle lets blood back in, and
 * what the cells held washes out over about an hour: myoglobin (~4 g per
 * kg of muscle) clogs the kidney tubules, worse when dehydrated, and
 * potassium (what gets past the cells' buffering, ~6 mmol per kg, into
 * ~15 L of extracellular fluid) makes the heart irritable. Working kidneys
 * clear both. */
static void step_crush(health *h, float gh, float dehyd)
{
    for (int i = 0; i < BP_COUNT; i++) {
        body_part *bp = &h->part[i];
        float load = bp->crush_next;
        bp->crush_next = 0.0f;
        if (load <= 0.0f && bp->crush_load > 0.0f && bp->crush > bp->crush_released + 0.01f) {
            h->reperfused += (bp->crush - bp->crush_released) * MUSCLE_KG[i];
            bp->crush_released = bp->crush;
            if (bp->crush > 0.1f) health_log(h, "Freed: the crushed %s swells as blood returns", PART_NAMES_LC[i]);
        }
        bp->crush_load = load;
        if (load <= 0.0f && bp->crush > 0.0f) {
            bp->crush = maxf(0.0f, bp->crush - 0.004f * gh); /* muscle regenerates over weeks */
            bp->crush_released = minf(bp->crush_released, bp->crush);
        }
    }
    float kid = h->organ[ORG_KIDNEYS];
    float wash = h->reperfused * minf(1.0f, gh);
    h->reperfused -= wash;
    h->myoglobin += 4.0f * wash;
    h->potassium += 6.0f * wash / 15.0f;
    h->organ[ORG_KIDNEYS] -= 0.0022f * h->myoglobin * gh * (0.6f + 8.0f * dehyd);
    h->myoglobin -= h->myoglobin * minf(1.0f, 0.3f * gh * maxf(kid, 0.1f));
    if (h->potassium > 4.2f) h->potassium -= (h->potassium - 4.2f) * minf(1.0f, 0.25f * gh * kid);
    /* Kidneys that fail stop excreting the potassium every meal brings. */
    if (kid < 0.5f) h->potassium += 0.15f * (0.5f - kid) * gh;
}

/* Bones and bruises heal, splints come off, and each part's pain adds up. */
static void step_parts(health *h, const health_env *e, double dt, float gh, int awake)
{
    const float fdt = (float)dt;
    int moving = e->speed > 0.5;
    for (int i = 0; i < BP_COUNT; i++) {
        body_part *bp = &h->part[i];
        float cap_i = bp->fracture ? 0.8f : 1.0f;
        /* Dead tissue does not bounce back like a bruise. */
        cap_i = minf(cap_i, 1.0f - 0.7f * bp->crush);
        if (bp->burn == 3) cap_i = minf(cap_i, 1.0f - 0.8f * bp->burn_area);
        if (bp->frost == FROST_DEEP) cap_i = minf(cap_i, 0.5f);
        if (bp->integrity < cap_i) bp->integrity = minf(cap_i, bp->integrity + 0.012f * gh);
        if (bp->sprain > 0.0f) bp->sprain = maxf(0.0f, bp->sprain - 0.004f * gh * (bp->splinted ? 1.5f : 1.0f));
        if (bp->fracture) {
            float rate = gh / (24.0f * (bp->splinted ? 42.0f : 70.0f));
            if (bp->fracture == FX_OPEN) {
                int open_wound = 0;
                for (int k = 0; k < h->wound_count; k++)
                    if (h->wounds[k].part == i && h->wounds[k].kind == WOUND_OPEN_FRACTURE) open_wound = 1;
                if (open_wound) rate *= 0.5f;
            }
            bp->fracture_heal += rate * (h->glycogen > 50.0f ? 1.0f : 0.6f);
            /* Walking on an unsplinted broken leg can drive the bone through the skin. */
            if (is_leg(i) && !bp->splinted && bp->fracture == FX_CLOSED && e->speed > 1.5 && frand(h) < 0.005f * fdt)
                break_bone(h, i, 1);
            if (bp->fracture_heal >= 1.0f) {
                bp->fracture = FX_NONE;
                bp->splinted = 0;
                health_log(h, "%s fracture has healed", PART_NAMES[i]);
            }
        } else if (bp->splinted && bp->sprain <= 0.0f) {
            bp->splinted = 0;
        }
        /* Local pain. */
        float pn = 5.0f * (1.0f - bp->integrity);
        if (bp->fracture) {
            float f = bp->fracture == FX_OPEN ? 8.0f : 6.0f;
            if (bp->splinted) f *= 0.5f;
            if (is_leg(i) && moving) f += bp->splinted ? 1.0f : 2.5f;
            pn += f;
        }
        pn += 4.0f * bp->sprain * (is_leg(i) && moving ? 1.5f : 1.0f);
        if (bp->internal > 1.0f && (i == BP_ABDOMEN || i == BP_CHEST)) pn += 3.0f;
        if (bp->dislocated) pn += 7.0f;
        /* Heat hurts from ~43 C (the nociceptors' threshold); a partial-
         * thickness burn hurts most, a full-thickness one kills the nerves
         * at its centre. Cold aches until the skin goes numb. */
        pn += 8.0f * ramp(bp->skin + bp->surface, 42.0f, 48.0f);
        if (bp->burn) pn += (bp->burn == 1 ? 1.5f : (bp->burn == 2 ? 4.0f : 1.5f)) * (0.5f + minf(0.5f, bp->burn_area));
        pn += 2.5f * ramp(-bp->skin, -15.0f, -8.0f) * ramp(bp->skin, 1.0f, 5.0f);
        /* Rewarmed frostbite throbs. */
        if (bp->frost >= FROST_SUPERFICIAL && bp->skin > 10.0f)
            pn += (bp->frost == FROST_DEEP ? 6.0f : 3.0f) * (1.0f - bp->frost_heal);
        if (bp->crush_load > 0.0f) pn += 3.0f + 4.0f * bp->crush;
        else pn += 5.0f * bp->crush;
        bp->pain_spike = maxf(0.0f, bp->pain_spike * (float)exp(-dt / 20.0));
        bp->pain = pn + bp->pain_spike;
    }
    for (int i = 0; i < h->wound_count; i++) {
        const wound *w = &h->wounds[i];
        h->part[w->part].pain += (1.0f + 3.0f * w->depth + (w->kind == WOUND_ABRASION ? 4.0f * w->area : 0.0f)) *
                                     (1.0f - w->closure) +
                                 3.0f * w->infection;
    }
    /* Angina from an ischaemic myocardium; the pounding headache of a
     * hypertensive crisis. */
    h->part[BP_CHEST].pain += 3.0f * h->pneumothorax + 6.0f * ramp(h->ischemia, 0.8f, 2.5f);
    h->part[BP_HEAD].pain += minf(4.0f, h->ich * 0.15f) + (h->confusion > 0.0f ? 1.5f : 0.0f) +
                             3.0f * ramp(h->sbp, 180.0f, 230.0f);
    float sum2 = 0.0f;
    for (int i = 0; i < BP_COUNT; i++) {
        h->part[i].pain = minf(PAIN_SCALE, h->part[i].pain);
        sum2 += h->part[i].pain * h->part[i].pain;
    }
    float pain = minf(PAIN_SCALE, sqrtf(sum2) * h->base.pain_gain) * (h->painkiller > 0.0f ? 0.45f : 1.0f);
    /* Opioids work on top of the painkiller (as in co-codamol); drink
     * dulls a little. */
    float opi = opioid_free(h);
    pain *= (1.0f - 0.7f * opi / (opi + OPI_PAIN_EC50)) * (1.0f - 0.3f * ramp(h->bac, 0.0f, 1.5f));
    /* A sudden jump in pain sets off fight-or-flight: a short adrenaline
     * surge, longer the worse the jump, which then dulls the pain itself
     * (stress-induced analgesia). */
    if (awake && pain - h->pain_ref > 3.0f && h->surge < 5.0f) {
        h->surge = minf(60.0f, 10.0f + 5.0f * (pain - h->pain_ref));
        h->surge_ng = SURGE_NG_S;
        health_log(h, "Adrenaline rush");
        /* In some, the rush overshoots into the opposite reflex: the vagus
         * takes over within seconds and the rush is cut short. */
        if (frand(h) < 0.2f * ramp(pain - h->pain_ref, 4.0f, 8.0f) && start_faint(h)) {
            h->surge = minf(h->surge, 5.0f);
            h->pain_ref = pain; /* spent: the short rush must not fire again for the same jump */
            health_log(h, "The pain turns your stomach; you go pale");
        }
    }
    h->pain_ref = lag(h->pain_ref, pain, dt, 20.0);
    /* Fear does the same without a wound: a predator closing in fast
     * startles a short rush out of the adrenal glands, and as long as it
     * keeps hunting they keep releasing (step_catechol). A sleeper or an
     * unconscious body feels nothing. */
    const float threat = awake ? fclamp(e->fear, 0.0f, 1.0f) : 0.0f;
    h->fear = lag(h->fear, threat, dt, 1.0);
    /* The eyes take a few seconds to register a change of light, so a
     * glance into a hole is not "pitch black" at once. */
    h->dark = lag(h->dark, fclamp(e->dark, 0.0f, 1.0f), dt, 3.0);
    if (h->fear - h->fear_ref > 0.3f && h->surge < 5.0f) {
        /* Sized by what was seen, not by how far the feeling has caught up. */
        h->surge = minf(30.0f, 10.0f + 25.0f * (threat - h->fear_ref));
        h->surge_ng = SURGE_NG_S;
        health_log(h, "Adrenaline rush: something is hunting you");
    }
    /* One startle per fright: while a rush lasts, the fear still climbing
     * as the wolf closes in is part of it. */
    h->fear_ref = h->surge > 0.0f ? maxf(h->fear_ref, h->fear) : lag(h->fear_ref, h->fear, dt, 30.0);
    pain *= 1.0f - 0.4f * catechol_beta(h);
    h->pain = awake ? lag(h->pain, pain, dt, 1.0) : 0.0f;
    h->painkiller = maxf(0.0f, h->painkiller - gh);
    h->pk_load = maxf(0.0f, h->pk_load - gh / 6.0f); /* one dose cleared every 6 game hours */
    if (h->hepatotox > 0.0f) {
        /* The damage unfolds over about a day; the nausea comes first. */
        float d = minf(h->hepatotox, 0.04f * gh);
        if (h->hepatotox > 0.1f && h->organ[ORG_LIVER] > 0.9f && h->stomach_kcal > 50.0f) {
            h->stomach_kcal *= 0.3f;
            h->stomach_water *= 0.3f;
            health_log(h, "Nauseous: you vomit");
        }
        h->organ[ORG_LIVER] = maxf(0.0f, h->organ[ORG_LIVER] - d);
        h->hepatotox -= d;
    }
    if (h->gut_upset > 0.0f) {
        h->water -= 0.08f * gh; /* diarrhoea */
        h->gut_upset = maxf(0.0f, h->gut_upset - gh);
    }
    h->antibiotic = maxf(0.0f, h->antibiotic - gh);
}

/* ------------------------------------------------------ defibrillation */

void health_pads(health *h, int on)
{
    defib_state *d = &h->defib;
    if (!on || h->dead) {
        memset(d, 0, sizeof *d);
        return;
    }
    if (d->phase != DEFIB_OFF) return;
    memset(d, 0, sizeof *d);
    d->seen = -1;
    d->since_shock = 1e9f;
    d->phase = DEFIB_ANALYSE; /* an AED analyses as soon as its pads are on */
    d->timer = DEFIB_ANALYSE_S;
}

void health_arrest(health *h, int rhythm)
{
    if (h->dead || rhythm < 0 || rhythm >= RHYTHM_COUNT || health_rhythm_perfusing(rhythm)) return;
    stop_heart(h, rhythm, AIRWAY_AIR);
}

/* Chance that one shock stops VF and a beat comes back: about 90% in the
 * first seconds, about 10% less for every minute of fibrillation without
 * CPR (Larsen et al. 1993). A heart colder than about 30 C seldom responds
 * until it is rewarmed (ERC 2021). */
static float shock_success(const health *h)
{
    float p = 0.9f - 0.1f * h->arrest_time / 60.0f;
    return fclamp(p * (0.3f + 0.7f * ramp(h->temp, 28.0f, 31.0f)), 0.02f, 0.9f);
}

void health_shock(health *h)
{
    if (h->dead) return;
    /* Current crowds at the pad edges: the skin under them (two pads, ~6%
     * of the chest) reddens after a few shocks and blisters after many. */
    body_part *chest = &h->part[BP_CHEST];
    chest->burn_area = maxf(chest->burn_area, 0.06f);
    chest->burn_omega = minf(1e6f, chest->burn_omega + 0.12f + 0.12f * frand(h));
    burn_degree(h, BP_CHEST);
    chest->pain_spike = PAIN_SCALE; /* every chest muscle jerks */
    if (health_rhythm_shockable(h->rhythm)) {
        float u = frand(h), p = shock_success(h);
        const char *what = h->rhythm == RHYTHM_VF ? "fibrillation" : "tachycardia";
        /* After a few minutes the starved myocardium tends to answer a
         * shock with asystole rather than a beat (Weisfeldt and Becker 2002). */
        float asys = (1.0f - p) * 0.8f * ramp(h->arrest_time, 120.0f, 360.0f);
        if (u < p) {
            h->rhythm = RHYTHM_SINUS;
            h->hr = HR_REST; /* the sinus node restarts; the baroreflex takes it from there */
            health_log(h, "Shock restored a heartbeat");
        } else if (u < p + asys) {
            h->rhythm = RHYTHM_ASYSTOLE;
            health_log(h, "Shock stopped the %s, but no beat: asystole", what);
        } else {
            health_log(h, "Shock did not stop the %s", what);
        }
    } else if (health_rhythm_perfusing(h->rhythm)) {
        /* A beating heart gains nothing. On the upstroke of the T wave, where
         * the ventricle is half recovered, a shock can start VF: why
         * cardioversion is synchronised to the R wave. */
        float qt = sqrtf(60.0f / maxf(h->hr, 1.0f)), since = (float)(h->t - h->last_r);
        h->hurt_flash = 1.0f;
        if (since > 0.2f * qt && since < 0.25f * qt && frand(h) < 0.5f) {
            stop_heart(h, RHYTHM_VF, AIRWAY_AIR);
            h->cause = DEATH_CARDIAC;
        } else if (h->rhythm == RHYTHM_VT) {
            /* VT's T waves fill most of the cycle, so an unsynchronised
             * shock stops it most of the time but lands on one often. */
            float u = frand(h);
            if (u < 0.7f) set_rhythm(h, RHYTHM_SINUS);
            else if (u < 0.85f) {
                stop_heart(h, RHYTHM_VF, AIRWAY_AIR);
                h->cause = DEATH_CARDIAC;
            }
        } else if (h->rhythm == RHYTHM_AF && frand(h) < 0.5f) {
            set_rhythm(h, RHYTHM_SINUS); /* about half of AF converts to a single 150 J biphasic shock */
        }
    }
}

/* The defibrillator on the physics clock. Nobody else is there to press a
 * button, so it works like a fully automatic AED: it watches the rhythm
 * through the pads, analyses a pulseless one, charges for VF and shocks
 * after a countdown, then analyses again. It disarms if the rhythm stops
 * being shockable. Hydrogel pads let go of wet skin, and no AED may shock
 * in water. */
static void step_defib(health *h, const health_env *e, float fdt)
{
    defib_state *d = &h->defib;
    if (d->phase == DEFIB_OFF) return;
    if (part_submerged(BP_CHEST, (float)e->submerged) > 0.5f) {
        health_pads(h, 0);
        health_log(h, "The defibrillator pads came off in the water");
        return;
    }
    int shockable = health_rhythm_shockable(h->rhythm);
    d->since_shock += fdt;
    d->timer -= fdt;
    if (d->phase == DEFIB_MONITOR) {
        if (h->rhythm != RHYTHM_SINUS && h->rhythm != d->seen) {
            d->phase = DEFIB_ANALYSE;
            d->timer = DEFIB_ANALYSE_S;
        }
    } else if (d->phase == DEFIB_ANALYSE) {
        if (d->timer > 0.0f) return;
        d->seen = h->rhythm;
        d->advice = shockable;
        d->phase = shockable ? DEFIB_CHARGE : DEFIB_MONITOR;
        d->timer = DEFIB_CHARGE_S;
    } else if (!shockable) {
        d->phase = DEFIB_MONITOR;
        d->seen = h->rhythm;
        d->advice = 0;
        d->joules = 0.0f;
    } else if (d->phase == DEFIB_CHARGE) {
        d->joules = DEFIB_J * fclamp(1.0f - d->timer / DEFIB_CHARGE_S, 0.0f, 1.0f);
        if (d->timer <= 0.0f) {
            d->phase = DEFIB_CLEAR;
            d->timer = DEFIB_CLEAR_S;
        }
    } else if (d->timer <= 0.0f) {
        health_shock(h);
        d->shocks++;
        d->since_shock = 0.0f;
        d->joules = 0.0f;
        d->phase = DEFIB_ANALYSE;
        d->timer = DEFIB_ANALYSE_S;
    }
}

/* The strain dose: how long the heart has worked against a pressure out
 * of range, in crisis-minutes. A mean pressure well above what exercise
 * explains loads the left ventricle (a mean of 160 mmHg adds about one a
 * minute); a diastolic below ~45 mmHg drops the coronaries under their
 * autoregulation range; a ventricle an arrhythmia drives past 130 bpm
 * never rests. Exercise, with its high systolic, adds nothing. Once all
 * is back in range it wears off over about ten minutes, and a stroke of
 * bad luck aside the heart recovers. */
static void step_strain(health *h, float ex, float fdt)
{
    /* The pressure rises with the will to move, before oxygen uptake
     * catches up, and stays up for minutes after effort while the muscles'
     * metabolites wash out; that is normal too. */
    ex = maxf(ex, maxf(h->ans.command, h->ans.metab));
    float over = fclamp((h->map - h->base.map_set - 30.0f * ex - 20.0f) / 40.0f, 0.0f, 2.0f);
    float under = fclamp((45.0f - h->dbp) / 20.0f, 0.0f, 2.0f);
    float fast = h->rhythm != RHYTHM_SINUS ? fclamp((h->hr - 130.0f) / 60.0f, 0.0f, 1.5f) : 0.0f;
    float load = over + under + fast;
    if (load > 0.0f) h->strain = minf(h->strain + load * fdt / 60.0f, 30.0f);
    else if (h->strain > 0.0f) h->strain *= expf(-fdt / 450.0f);
    /* Hypertensive intracerebral haemorrhage: above a mean pressure of
     * ~140-150 the brain's arterioles can no longer hold back the pressure
     * (the upper limit of autoregulation), and its small arteries give way,
     * more likely the longer the crisis has gone on. */
    float stroke = 3e-4f * ramp(h->map, 140.0f, 190.0f) * minf(h->strain, 10.0f);
    if (stroke > 0.0f && frand(h) < stroke * fdt) {
        h->ich_rate += 1.0f + 3.0f * frand(h);
        health_log(h, "Stroke: bleeding in the brain");
    }
}

/* Arrhythmias of a heart that still beats. Irritability (the strain
 * dose, ischaemia, catecholamines, potassium) raises the hazard of each
 * rhythm starting; they stop again once the cause has gone. One random
 * number a step, and only while some hazard is above zero, so a healthy
 * heart replays exactly as before any of this existed. */
static void step_arrhythmia(health *h, int airway, float fdt, float beta)
{
    float irr = 1.0f * maxf(0.0f, h->strain - 2.0f) + maxf(0.0f, h->ischemia - 1.0f) +
                beta * beta * (0.5f + 0.1f * h->strain) + 0.5f * maxf(0.0f, h->potassium - 6.0f);
    /* Ischaemia of the AV node, hyperkalaemia and deep cold block conduction. */
    float block = 2e-4f * maxf(0.0f, h->ischemia - 1.0f) + 1e-3f * maxf(0.0f, h->potassium - 7.0f) +
                  (h->temp < 30.0f ? 3e-4f : 0.0f);
    float to_af = 0.0f, to_vt = 0.0f, to_vf = 0.0f, to_block = 0.0f, to_sinus = 0.0f, to_stop = 0.0f;
    switch (h->rhythm) {
    case RHYTHM_SINUS:
        /* Stretched, irritable atria fibrillate first; cold below ~32 C
         * brings AF on by itself. */
        to_af = 2e-4f * irr + (h->temp < 32.0f ? 3e-4f : 0.0f);
        to_vt = 5e-5f * irr;
        to_vf = 1e-5f * irr;
        to_block = block;
        break;
    case RHYTHM_AF:
        to_vt = 5e-5f * irr;
        to_vf = 1e-5f * irr;
        /* Paroxysmal AF mostly converts by itself once the trigger is gone. */
        to_sinus = irr < 0.3f && h->temp > 34.0f ? 1.0f / 240.0f : 0.0f;
        break;
    case RHYTHM_VT:
        /* VT too weak to feel at the wrist is pulseless VT: an arrest. */
        if (h->co < 1.5f) {
            stop_heart(h, RHYTHM_PVT, airway);
            return;
        }
        /* Sustained VT degenerates into VF, sooner in an ischaemic heart;
         * VT whose trigger has gone often stops by itself. */
        to_vf = 0.002f + 0.01f * maxf(0.0f, h->ischemia - 1.0f) + 5e-4f * irr;
        to_sinus = irr < 1.0f ? 0.01f : 0.002f;
        break;
    case RHYTHM_AVB3:
        /* The escape focus can fail (a Stokes-Adams attack); a block from a
         * transient cause lifts when the cause clears. */
        to_stop = 5e-4f + 1e-3f * maxf(0.0f, h->ischemia - 1.0f);
        to_sinus = block <= 0.0f && h->strain < 3.0f ? 1.0f / 200.0f : 0.0f;
        break;
    default: return;
    }
    const float hazard[6] = {to_af, to_vt, to_vf, to_block, to_stop, to_sinus};
    static const int NEXT[6] = {RHYTHM_AF, RHYTHM_VT, RHYTHM_VF, RHYTHM_AVB3, RHYTHM_ASYSTOLE, RHYTHM_SINUS};
    if (to_af + to_vt + to_vf + to_block + to_sinus + to_stop <= 0.0f) return;
    float u = frand(h), acc = 0.0f;
    for (int i = 0; i < 6; i++) {
        acc += hazard[i] * fdt;
        if (u >= acc) continue;
        if (health_rhythm_perfusing(NEXT[i])) set_rhythm(h, NEXT[i]);
        else stop_heart(h, NEXT[i], airway);
        return;
    }
}

/* Circulating adrenaline: released while a surge lasts, cleared in
 * minutes by neuronal uptake and COMT/MAO. */
static void step_catechol(health *h, double dt)
{
    if (h->surge > 0.0f) {
        h->surge = maxf(0.0f, h->surge - (float)dt);
        h->catechol += (h->surge_ng > 0.0f ? h->surge_ng : SURGE_NG_S) * (float)dt;
    }
    h->catechol += FEAR_NG_S * h->fear * (float)dt;
    /* Heavy sympathetic traffic also drives the adrenal medulla: hard
     * exercise and shock raise plasma adrenaline to 1-2 ng/mL. */
    h->catechol += 0.06f * maxf(0.0f, h->symp - 0.42f) * (float)dt;
    h->catechol *= (float)exp(-dt * 0.6931472 / (double)ADR_HALF_S);
    /* A rush is paid for afterwards: the longer it ran high, the longer the
     * shaky, tired spell once it wears off. */
    float beta = catechol_beta(h);
    if (beta > 0.4f) h->crash = minf(1.0f, h->crash + (float)dt / 90.0f);
    else if (beta < 0.15f) h->crash = maxf(0.0f, h->crash - (float)dt / 240.0f);
}

/* Absorption and clearance of morphine and alcohol, on the survival clock
 * like the painkiller, and the vomiting they bring on: in someone out
 * cold, what comes up can go down into the lungs. */
static void step_drugs(health *h, float fdt, float gh, int awake)
{
    float a = h->opioid_gut * (1.0f - expf(-gh / OPI_ABS_H));
    h->opioid_gut -= a;
    float half = OPI_HALF_H / maxf(h->organ[ORG_LIVER], 0.2f);
    h->opioid = (h->opioid + a) * expf(-gh * 0.6931472f / half);
    h->naloxone = maxf(0.0f, h->naloxone - gh);
    /* Food in the stomach slows alcohol on its way to the gut. */
    float tau = h->stomach_kcal > 200.0f ? 1.0f : 0.5f;
    float e = h->alcohol_gut * (1.0f - expf(-gh / tau));
    h->alcohol_gut -= e;
    h->bac += e / WIDMARK;
    h->water -= 0.01f * e; /* alcohol holds back ADH: ~10 mL more urine a gram */
    h->drink_load = (h->drink_load + e) * expf(-gh / 48.0f);
    h->bac = maxf(0.0f, h->bac - ALC_ELIM * gh * maxf(h->organ[ORG_LIVER], 0.2f));
    if (h->opioid < 1e-3f && h->opioid_gut < 1e-3f) h->opioid = h->opioid_gut = 0.0f;

    float p = 0.05f * ramp(h->bac, 1.8f, 3.0f) + 0.01f * ramp(opioid_free(h), 15.0f, 40.0f);
    int full = h->stomach_kcal > 20.0f || h->stomach_water > 0.1f || h->alcohol_gut > 1.0f;
    if (p <= 0.0f || !full || frand(h) >= p * fdt) return;
    h->stomach_kcal *= 0.3f;
    h->stomach_water *= 0.3f;
    h->alcohol_gut *= 0.3f;
    if (awake) {
        health_log(h, "You vomit");
        return;
    }
    h->lung_water += 0.12f;
    h->organ[ORG_LUNGS] = maxf(0.0f, h->organ[ORG_LUNGS] - 0.08f);
    health_log(h, "Vomited while unconscious and breathed it in");
}

/* Breathing and the blood gases: the rate the brainstem asks for, what
 * reaches the alveoli, and the oxygen and CO2 that leaves in the blood. */
static void step_breathing(health *h, const health_env *e, int arrest, int awake, float vo2_frac, float hb, double dt)
{
    const float fdt = (float)dt;
    float lung_eff = h->organ[ORG_LUNGS] * (1.0f - h->pneumothorax) * (1.0f - fclamp(h->lung_water / 1.5f, 0.0f, 0.9f));
    int drive_ok = h->organ[ORG_BRAIN] > 0.08f && !(arrest && h->arrest_time > 20.0f);
    /* A weight on the chest (or, less, on the belly, splinting the
     * diaphragm) stops the ribs rising: traumatic asphyxia past ~250 kg. */
    float vent = 1.0f - ramp(h->part[BP_CHEST].crush_load + 0.5f * h->part[BP_ABDOMEN].crush_load, 30.0f, 250.0f);
    int can_air = e->airway == AIRWAY_AIR && vent > 0.08f;
    /* Tissues extract less as the store runs dry and the brain shuts down. */
    h->o2_store = maxf(0.0f, h->o2_store - h->vo2 * fclamp(h->pao2 / 25.0f, 0.3f, 1.0f) / 60.0f * fdt);
    const float dep = health_resp_depression(h), rr_floor = 4.0f * (1.0f - dep);
    if (can_air && drive_ok) {
        h->breathing = 1;
        float rr_need = 12.0f + 26.0f * vo2_frac;
        /* Pain and distress add fast, shallow breaths: they raise the rate
         * far more than they clear CO2. */
        float shallow = minf(0.5f * h->pain, 6.0f) + 18.0f * h->pneumothorax + 10.0f * h->lung_water;
        float rr_t = rr_need + 1.2f * maxf(0.0f, h->paco2 - 40.0f) + 60.0f * maxf(0.0f, 0.93f - h->sao2) + shallow +
                     (h->lactate > 4.0f ? 0.9f * (h->lactate - 4.0f) : 0.0f) + 2.0f * maxf(0.0f, h->temp - 37.5f);
        if (!awake) rr_t = minf(rr_t, 24.0f);
        if (h->organ[ORG_BRAIN] < 0.3f) rr_t *= h->organ[ORG_BRAIN] / 0.3f;
        /* Opioids and alcohol damp the brainstem's answer to CO2 and to low
         * oxygen alike: the rate falls whatever the blood gases say. Past
         * ~80 mmHg CO2 itself sedates the brainstem (CO2 narcosis), so a
         * depressed drive spirals down instead of being rescued. */
        rr_t *= (1.0f - dep) * (1.0f - (dep > 0.0f ? 0.8f * ramp(h->paco2, 80.0f, 110.0f) : 0.0f));
        if (h->base.vary) {
            h->rr_noise = wander(&h->vrng, h->rr_noise, dt, 6.0, 1.0f); /* breaths/min, breath-to-breath jitter */
            rr_t += h->rr_noise;
        }
        h->rr = lag(h->rr, fclamp(rr_t, rr_floor, 55.0f), dt, 4.0);
        float pao2_target = maxf(0.0f, 0.21f * 713.0f - h->paco2 / 0.8f);
        float store_t = O2_VOL * pao2_target / 713.0f;
        h->o2_store = lag(h->o2_store, store_t, dt, 6.0 * 14.0 / (double)maxf(h->rr * vent, 1.0f));
        float alveolar = maxf(h->rr - 0.6f * minf(shallow, maxf(h->rr - rr_need, 0.0f)), maxf(rr_floor, 0.5f)) * vent;
        h->paco2 = lag(h->paco2, 40.0f * rr_need / alveolar, dt, 20.0);
        h->apnea_time = 0.0f;
        h->gasp_timer = 0.0f;
    } else {
        h->breathing = 0;
        h->rr = lag(h->rr, 0.0f, dt, 1.0);
        h->apnea_time += fdt;
        h->paco2 += 0.13f * powf(maxf(h->vo2, 0.1f) / 0.254f, 0.6f) * fdt;
        /* Holding breath ends at the breaking point: an involuntary gasp. */
        int breakpoint = h->paco2 > 52.0f || h->sao2 < 0.65f || !awake;
        if (e->airway == AIRWAY_WATER && drive_ok && breakpoint) {
            h->gasp_timer -= fdt;
            if (h->gasp_timer <= 0.0f) {
                h->gasp_timer = 3.0f;
                h->lung_water += 0.06f;
                h->organ[ORG_LUNGS] = maxf(0.0f, h->organ[ORG_LUNGS] - 0.03f);
                if (h->lung_water < 0.07f) health_log(h, "Inhaled water!");
            }
        }
    }
    h->paco2 = fclamp(h->paco2, 15.0f, 150.0f);
    h->pao2 = h->o2_store / O2_VOL * 713.0f;
    float shunt = 0.02f + 0.6f * (1.0f - lung_eff);
    float delivery = h->co * 13.4f * hb * h->sao2;                /* mL O2/min */
    float svo2 = delivery > 1.0f ? fclamp(h->sao2 * (1.0f - h->vo2 * 1000.0f / delivery), 0.05f, 0.95f) : 0.1f;
    h->sao2 = fclamp((1.0f - shunt) * severinghaus(h->pao2) + shunt * svo2, 0.0f, 1.0f);
    h->spo2_shown = lag(h->spo2_shown, h->sao2, dt, 5.0);
    if (h->base.vary) {
        h->spo2_noise = wander(&h->vrng, h->spo2_noise, dt, 8.0, 0.005f); /* pulse-ox wander, +-0.5% */
        h->spo2_shown = fclamp(h->spo2_shown + h->spo2_noise, 0.0f, 1.0f);
    }
    h->etco2 = h->breathing ? maxf(0.0f, h->paco2 - 3.0f - 25.0f * maxf(0.0f, 1.0f - h->co / 4.0f)) : 0.0f;
}

/* One cause's level, for a quantity whose lower values are worse (negate
 * one whose higher values are): 1 down to `fine`, 0.85 at `confused` (the
 * edge of alert), 0.3 at `out` (where the eyes close) and none at all as
 * far again below. Between them it runs straight. */
static float cons_low(float x, float fine, float confused, float out)
{
    if (x >= fine) return 1.0f;
    if (x >= confused) return 0.85f + 0.15f * (x - confused) / (fine - confused);
    if (x >= out) return 0.3f + 0.55f * (x - out) / (confused - out);
    return maxf(0.0f, 0.3f * (1.0f - (out - x) / (confused - out)));
}

int health_cons_of(float level)
{
    static const float EDGE[CONS_ALERT] = {0.1f, 0.2f, 0.3f, 0.6f, 0.85f}; /* lowest level of each step above */
    int c = CONS_UNCONSCIOUS;
    while (c < CONS_ALERT && level >= EDGE[c]) c++;
    return c;
}

int health_awake(const health *h) { return h->conscious >= CONS_DROWSY; }

int health_impaired(const health *h) { return h->conscious == CONS_DROWSY || h->conscious == CONS_CONFUSED; }

const char *health_cons_name(int cons)
{
    static const char *const NAME[CONS_ALERT + 1] = {"Unresponsive", "Pain only", "Voice only",
                                                     "Drowsy",       "Confused",  "Alert"};
    return cons >= 0 && cons <= CONS_ALERT ? NAME[cons] : "?";
}

/* The brain's oxygen, its injuries, and how awake that leaves the body. */
static void step_brain(health *h, float cao2_rel, float dehyd, int arrest, double dt)
{
    const float fdt = (float)dt;
    /* The brain bed's flow already carries its CO2 and oxygen reactivity
     * (brain_want above) and the pressure behind it. */
    h->brain_o2 = lag(h->brain_o2, h->brain_flow * cao2_rel, dt, 8.0);
    if (h->brain_o2 < 0.3f) h->organ[ORG_BRAIN] -= (0.3f - h->brain_o2) / 0.3f * fdt / 300.0f;
    if (h->icp > 40.0f) h->organ[ORG_BRAIN] -= fdt / 120.0f;
    if (h->temp > 41.0f) h->organ[ORG_BRAIN] -= (h->temp - 41.0f) * fdt / 600.0f;
    h->organ[ORG_BRAIN] = maxf(0.0f, h->organ[ORG_BRAIN]);
    h->concussion = maxf(0.0f, h->concussion - fdt);
    h->confusion = maxf(0.0f, h->confusion - fdt);

    /* The brain extracts more oxygen as delivery falls, so function holds
     * until delivery is roughly halved: confusion near 55%, the eyes
     * closing below about 40%. Extra flow cannot make up for very low
     * arterial oxygen, though: oxygen has to diffuse into the tissue, and
     * acute hypoxaemia blacks people out near SaO2 50% whatever the flow.
     * Each cause gives its own level and the worst one rules: a clouded
     * mind is as clear as its worst insult allows. */
    const float sed = health_sedation(h);
    float want = minf(cons_low(h->brain_o2, 0.7f, 0.55f, 0.4f), cons_low(h->sao2, 0.85f, 0.7f, 0.5f));
    const float bn = health_blood_normal(h);
    want = minf(want, cons_low(h->blood, 0.8f * bn, 0.7f * bn, 0.4f * bn));
    /* Mild hypothermia (32-35 C) already brings confusion and poor
     * judgement; consciousness is lost near 30. */
    want = minf(want, minf(cons_low(h->temp, 35.5f, 34.5f, 30.0f), cons_low(-h->temp, -39.0f, -40.0f, -41.5f)));
    want = minf(want, minf(cons_low(-h->sepsis, -0.5f, -0.7f, -0.92f), cons_low(-sed, -0.3f, -0.45f, -0.85f)));
    want = minf(want, minf(cons_low(h->organ[ORG_BRAIN], 1.0f, 0.6f, 0.3f), cons_low(-h->icp, -20.0f, -28.0f, -35.0f)));
    want = minf(want, minf(cons_low(-dehyd, -0.05f, -0.1f, -0.25f), cons_low(h->fat, 3000.0f, 2000.0f, 500.0f)));
    if (h->confusion > 0.0f) want = minf(want, 0.75f);
    /* Consciousness goes within a second or two of the brain's oxygen
     * failing, and comes back more slowly once it returns; a blow to the
     * head or a stopped heart switches it off at once. */
    h->level = fclamp(lag(h->level, want, dt, want < h->level ? 1.0 : 4.0), 0.0f, 1.0f);
    if (h->concussion > 0.0f || arrest) h->level = 0.0f;
    int cons = health_cons_of(h->level);
    /* A drowsy mind drifts off for a second or three now and then (micro-
     * sleeps, absence-like lapses), more often the deeper it is, and
     * catches itself without falling into a faint's long lie. */
    h->lapse = maxf(0.0f, h->lapse - fdt);
    if (cons < CONS_DROWSY) h->lapse = 0.0f;
    else if (h->lapse <= 0.0f && frand(h) < 0.08f * (1.0f - ramp(h->level, 0.3f, 0.6f)) * fdt)
        h->lapse = 1.0f + 2.0f * frand(h);
    /* Below the reach of the world the eyes still open on it now and then
     * for a moment, as a stuporous patient's do: more often for one that
     * answers to a voice than to pain alone, never past reach. */
    if (cons >= CONS_DROWSY || cons == CONS_UNCONSCIOUS) h->glimpse = 0.0f;
    else if (h->glimpse <= 0.0f && frand(h) < (cons == CONS_VOICE ? 0.12f : 0.04f) * fdt)
        h->glimpse = 0.4f + 0.8f * frand(h);
    else h->glimpse = maxf(0.0f, h->glimpse - fdt); /* closed for a step at least before the next */
    if (cons < CONS_DROWSY && health_awake(h)) health_log(h, "Lost consciousness");
    /* Someone who fainted comes round lying where they fell and stays
     * down a while: lying is what brought the blood back to the brain,
     * and getting up at once would only drop them again. */
    if (cons < CONS_DROWSY) h->down = DOWN_S;
    else h->down = maxf(0.0f, h->down - fdt);
    h->conscious = cons;
}

/* Organs failing under low pressure, dehydration and sepsis (real time). */
static void organ_stress(health *h, float dehyd, float fdt)
{
    if (h->map < 60.0f) h->organ[ORG_KIDNEYS] -= (60.0f - h->map) / 60.0f * fdt / 1800.0f;
    if (dehyd > 0.08f) h->organ[ORG_KIDNEYS] -= (dehyd - 0.08f) * fdt / 600.0f;
    if (h->sepsis > 0.5f) {
        h->organ[ORG_KIDNEYS] -= (h->sepsis - 0.5f) * fdt / 900.0f;
        h->organ[ORG_LIVER] -= (h->sepsis - 0.5f) * fdt / 1500.0f;
    }
    if (h->map < 50.0f) {
        h->organ[ORG_LIVER] -= (50.0f - h->map) / 50.0f * fdt / 2400.0f;
        h->organ[ORG_GUT] -= (50.0f - h->map) / 50.0f * fdt / 1800.0f;
    }
    for (int i = 0; i < ORG_COUNT; i++) h->organ[i] = fclamp(h->organ[i], 0.0f, 1.0f);
}

/* Organs mending on the survival clock. */
static void organ_recover(health *h, float dehyd, float gh)
{
    if (h->brain_o2 > 0.8f) h->organ[ORG_BRAIN] += 0.0005f * gh;
    if (h->ischemia < 0.5f) h->organ[ORG_HEART] += 0.001f * gh;
    h->organ[ORG_LUNGS] += 0.01f * gh;
    h->lung_water = maxf(0.0f, h->lung_water - 0.04f * gh);
    if (h->sepsis < 0.3f) h->organ[ORG_LIVER] += 0.004f * gh;
    if (h->map > 65.0f && dehyd < 0.05f) h->organ[ORG_KIDNEYS] += 0.003f * gh;
    h->organ[ORG_GUT] += 0.005f * gh;
    for (int i = 0; i < ORG_COUNT; i++) h->organ[i] = fclamp(h->organ[i], 0.0f, 1.0f);
    h->pneumothorax = maxf(0.0f, h->pneumothorax - (h->pneumothorax < 0.3f ? 0.01f : 0.005f) * gh);
    h->ich = maxf(0.0f, h->ich - 0.2f * gh);
}

void health_step(health *h, const health_env *e, double dt)
{
    h->t += dt;
    for (int i = 0; i < HEALTH_LOG; i++) h->log_age[i] += (float)dt;
    h->hurt_flash = maxf(0.0f, h->hurt_flash - (float)dt * 0.8f);
    if (h->dead) {
        h->hr = h->rr = 0.0f;
        h->breathing = 0;
        h->map = lag(h->map, 0.0f, dt, 20.0);
        h->sbp = h->dbp = h->map;
        h->co = h->sv = 0.0f;
        h->brain_flow = 0.0f;
        write_samples(h, e, dt, NULL, NULL, 0);
        return;
    }
    step_defib(h, e, (float)dt);
    const float gh = (float)(dt * HEALTH_CLOCK / 3600.0); /* survival-clock hours this step */
    const float fdt = (float)dt;
    const int awake = health_awake(h);
    const float dehyd = dehydration(h);
    const float hb = health_hb(h);
    const float cao2_rel = (hb / 15.0f) * (h->sao2 / 0.975f);
    const int arrest = !health_rhythm_perfusing(h->rhythm);

    /* ---- metabolism: power demand, stamina, VO2, lactate */
    const float bmr = BMR_W * h->base.metab_gain, crit_power = CRIT_POWER * h->base.metab_gain,
                vo2max_w = VO2MAX_W * h->base.metab_gain;
    float p = bmr * (1.0f + 0.1f * maxf(0.0f, h->temp - 37.0f));
    int swimming = e->submerged > 0.5;
    if (awake) {
        if (swimming) p += 60.0f + 450.0f * (float)minf((float)e->speed, 3.0f); /* sculling + strokes */
        else if (e->on_ground) p += 300.0f * minf((float)e->speed, 9.0f);
        else p += 60.0f;
        if (e->jumped) h->wbal -= 1500.0f;
        h->wbal -= 300.0f * (float)e->actions;
    }
    /* Shivering, driven by a cooling core, and by cold skin unless the core
     * is already warm. */
    float skin_drive = maxf(0.0f, 27.0f - health_skin_mean(h)) * fclamp((37.1f - h->temp) / 0.3f, 0.0f, 1.0f);
    p += minf(400.0f, 250.0f * maxf(0.0f, 36.5f - h->base.cold_bias - h->temp) + 20.0f * skin_drive);
    float cap = cao2_rel * h->organ[ORG_HEART] * fclamp(h->blood / health_blood_normal(h), 0.0f, 1.0f) *
                (h->glycogen > 50.0f ? 1.0f : 0.7f) * (1.0f - fclamp(dehyd * 3.0f, 0.0f, 0.4f));
    cap = fclamp(cap, 0.05f, 1.0f);
    float cp = crit_power * cap, aer_max = vo2max_w * cap;
    if (p > cp) {
        h->wbal -= (p - cp) * fdt;
        h->lactate += (p - cp) / 5000.0f * fdt;
    } else {
        float tau = 316.0f + 546.0f * expf(-0.0025f * (cp - p));
        h->wbal = lag(h->wbal, WPRIME, dt, (double)tau);
    }
    h->wbal = fclamp(h->wbal, 0.0f, WPRIME);
    h->power = lag(h->power, minf(p, aer_max), dt, 25.0); /* VO2 on-kinetics */
    h->vo2 = h->power / J_PER_ML_O2 * 0.06f;
    float vo2_frac = h->vo2 / vo2max_l(h->base.metab_gain);
    float do2_rel = (h->co / 4.8f) * cao2_rel;
    if (do2_rel < 0.5f) h->lactate += (0.5f - do2_rel) * 0.05f * fdt; /* shock: tissues starved of oxygen */
    /* Prolonged severe hypoperfusion poisons the heart and vessels
     * (acidosis, inflammatory mediators): past a point, restoring volume
     * no longer restores pressure. Slowly repaid once perfusion returns. */
    if (!arrest && do2_rel < 0.4f) h->shock_debt += (0.4f - do2_rel) / 0.4f * fdt / 60.0f;
    else h->shock_debt = maxf(0.0f, h->shock_debt - fdt / 1800.0f);
    h->lactate -= (h->lactate - 1.0f) * 0.0012f * h->organ[ORG_LIVER] * fdt;
    h->lactate = fclamp(h->lactate, 0.5f, 30.0f);

    /* ---- heat: skin, core, wet clothes, burns and frostbite */
    float sweat = step_heat(h, e, dt, gh, p);

    /* ---- water */
    float absorb = minf(h->stomach_water, 1.2f * gh * h->organ[ORG_GUT]);
    h->stomach_water -= absorb;
    /* A step's sweat is far below the float resolution of the body's water:
     * collect it first. */
    h->sweat_acc += sweat;
    if (h->sweat_acc > 1e-3f) {
        h->water -= h->sweat_acc;
        h->sweat_acc = 0.0f;
    }
    h->water += absorb - 0.104f * gh * (1.0f + 0.1f * maxf(0.0f, h->temp - 37.0f));
    if (h->water > WATER_NORMAL + 0.5f) h->water -= (h->water - WATER_NORMAL) * 0.5f * gh * h->organ[ORG_KIDNEYS];

    /* ---- energy */
    float kcal_basal = bmr * 3600.0f / 4184.0f * gh;
    float kcal_ex = maxf(0.0f, p - bmr) * fdt / 4184.0f;
    float carb = 0.4f * kcal_basal + (0.5f + 0.45f * fclamp(p / crit_power, 0.0f, 1.0f)) * kcal_ex;
    float digest = minf(h->stomach_kcal, 250.0f * gh * h->organ[ORG_GUT]);
    h->stomach_kcal -= digest;
    h->glycogen += digest - carb;
    h->fat -= kcal_basal + kcal_ex - carb;
    if (h->glycogen > GLYCOGEN_MAX) {
        h->fat += h->glycogen - GLYCOGEN_MAX;
        h->glycogen = GLYCOGEN_MAX;
    }
    if (h->glycogen < 0.0f) {
        h->fat += h->glycogen;
        h->glycogen = 0.0f;
    }

    step_drugs(h, fdt, gh, awake);
    step_burns(h, gh);
    step_crush(h, gh, dehyd);

    /* ---- circulation: the nerves, then the heart's timing, then the blood
     * itself (autonomic.c, conduction.c, circulation.c). */
    step_vagal(h, e, dt, awake);
    h->icp = 10.0f + 0.8f * h->ich + 0.012f * h->ich * h->ich;
    const int cushing = h->icp > 30.0f;
    const float chemo = fclamp((0.92f - h->sao2) / 0.3f, 0.0f, 1.0f) +
                        0.6f * fclamp((h->paco2 - 45.0f) / 25.0f, 0.0f, 1.0f);
    /* Exercise intensity for the heart's strain: central command at once,
     * the metabolic part following oxygen uptake. */
    const float ex = fclamp(0.35f * minf(p / vo2max_w, 1.0f) + 0.65f * vo2_frac, 0.0f, 1.0f);
    /* Out cold or swimming, the body lies flat; crouching brings the head
     * nearer the heart and folds the legs up. */
    /* Coming round, a body lies a while, then sits up before it stands. */
    const int lying = !awake || swimming || h->down > SIT_S;
    const int sitting = !lying && h->down > 0.0f;
    const int crouched = (e->crouched || sitting) && !lying;
    /* What the brain's vessels aim for: more flow with CO2 and with low
     * oxygen, and thinner blood flows faster. */
    const float brain_want = fclamp(1.0f + 0.025f * (h->paco2 - 40.0f), 0.6f, 1.6f) *
                             (1.0f + 0.5f * maxf(0.0f, 0.9f - h->sao2)) *
                             fclamp(powf(15.0f / maxf(hb, 3.0f), 0.8f), 1.0f, 1.8f);

    autonomic_input ai = {0};
    /* Each body's baroreceptors hold their own set point. */
    ai.p_ao_mean = h->cv.p_ao_mean + (MAP_SET - h->base.map_set);
    ai.p_ra_mean = h->cv.p_ra_mean;
    /* The carotid sinus sits 25 cm above the heart standing, but the
     * aortic arch receptors sit at heart level and share the work: the
     * pressure the reflex holds is about halfway between. */
    ai.h_neck = lying ? 0.0f : (crouched ? 0.07f : 0.12f);
    ai.effort = awake ? fclamp((p - bmr) / (vo2max_w - bmr), 0.0f, 1.0f) : 0.0f;
    ai.muscle_o2_debt = 1.0f - h->wbal / WPRIME;
    ai.chemo = chemo;
    ai.pain = h->pain / 10.0f;
    ai.fear = h->fear;
    ai.vagal_surge = h->vagal;
    ai.diving = e->airway == AIRWAY_WATER && awake ? 1.0f : 0.0f;
    ai.breath_phase = h->breathing ? h->breath_phase : 0.5f;
    ai.brain_flow = h->brain_flow / brain_want;
    ai.gain = h->base.baro_gain;
    ai.noise_amp = h->base.vary ? h->base.symp_noise : 0.0f;
    ai.brain_ok = h->organ[ORG_BRAIN] >= 0.15f;
    autonomic_out ao = {0};
    auto_step(&h->ans, &ai, dt, &h->vrng, &ao);
    h->symp = ao.symp;
    step_catechol(h, dt);
    const float beta = catechol_beta(h), alpha = catechol_alpha(h);

    /* The sinus node: cold slows it, fever and adrenaline speed it, a
     * crushed brainstem (Cushing) and deep hypoxia brake it, and a damaged
     * heart can't reach its full rate. */
    /* Fever: about 8.5 bpm per degree, 0.12 of a resting rate (Karjalainen
     * and Viitasalo 1986, CREDITS.md). Sepsis's cytokines speed the node
     * beyond what the fever explains (the SIRS heart rate). Adrenaline at
     * a fright's 0.5 ng/mL adds ~10 bpm, ~20 at exercise's 1.5 (Clutter et
     * al. 1980, CREDITS.md). */
    float sa = ao.sa_rate * (1.0f + 0.12f * maxf(0.0f, h->temp - 37.0f)) * (1.0f + 0.15f * h->sepsis) *
               (1.0f + 0.6f * h->catechol / (h->catechol + 1.5f));
    if (h->temp < 35.0f) sa *= fclamp(1.0f - (35.0f - h->temp) * 0.08f, 0.3f, 1.0f);
    if (cushing) sa = minf(sa, 50.0f);
    if (h->sao2 < 0.5f) sa *= 0.35f + 0.65f * h->sao2 / 0.5f;
    sa = minf(sa, h->base.hr_max * (0.55f + 0.45f * h->organ[ORG_HEART]));

    /* Ectopic beats a minute: a healthy heart throws a few an hour; an
     * ischaemic, hyperkalaemic, adrenaline-soaked, acidotic, cold or
     * strained one many more. */
    float irr = 0.05f + 8.0f * maxf(0.0f, h->ischemia - 1.0f) + 2.0f * maxf(0.0f, h->potassium - 5.5f) +
                1.5f * beta * beta + (h->lactate > 10.0f ? 3.5f : 0.0f) +
                (h->organ[ORG_KIDNEYS] < 0.15f ? 4.0f : 0.0f) + (h->temp < 32.0f ? 3.5f : 0.0f) +
                minf(1.4f * h->strain, 14.0f);
    conduction_input ki = {sa, ao.av_delay, irr, h->rhythm};
    double ta_old = h->cond.t_atria;
    /* Ectopic timing is beat-to-beat variability: on its own stream, so a
     * change in heart rate never reshuffles the rolls for injuries. */
    cond_step(&h->cond, &ki, h->t - dt, dt, &h->vrng);
    if (h->cond.t_atria != ta_old) h->atria_prev = ta_old;

    circ_input ci = {0};
    ci.t_atria = h->cond.t_atria;
    ci.t_atria_prev = h->atria_prev;
    ci.t_vent = h->cond.t_vent;
    ci.t_vent_prev = h->cond.prev_vent;
    double beat_rr = h->cond.prev_vent >= 0.0 ? h->cond.t_vent - h->cond.prev_vent : 60.0 / (double)sa;
    /* Sympathetic tone and adrenaline speed relaxation as well as
     * contraction (lusitropy): systole shortens, which is what leaves a
     * racing heart time to fill. */
    ci.rr = (float)fmin(fmax(beat_rr, 0.19), 2.0) * (1.0f - 0.3f * fminf(1.0f, h->symp + 0.5f * beta));
    ci.atria_on = cond_atria_on(h->rhythm);
    /* PEA: the ventricles fire but barely squeeze. */
    ci.vent_on = h->rhythm == RHYTHM_PEA ? 0.1f : cond_vent_on(h->rhythm);
    /* Contractility: the nerves, adrenaline, and a myocardium that is
     * damaged, starved, acidotic, strained or poisoned by long shock. VT's
     * wide beat squeezes out of step; pulseless VT barely at all. */
    float emax = ao.emax * (1.0f + 1.2f * beta) * h->organ[ORG_HEART] * (h->lactate > 8.0f ? 0.8f : 1.0f) *
                 (h->sao2 < 0.6f ? 0.3f + 0.7f * h->sao2 / 0.6f : 1.0f) / (1.0f + 0.02f * h->strain) /
                 (1.0f + 0.25f * h->shock_debt * h->shock_debt);
    if (h->rhythm == RHYTHM_VT) emax *= 0.55f;
    else if (h->rhythm == RHYTHM_PVT) emax *= 0.15f;
    ci.emax_l = ci.emax_r = emax;
    /* Resistance: the nerves; adrenaline (alpha tightens at pressor
     * levels, beta-2 opens vessels, mostly the muscles', so a fright's
     * diastolic pressure falls); sepsis and long shock slacken it; a hot
     * skin opens its vessels (against its resting state, vaso 0.2). */
    float r_scale = (1.0f + 1.5f * alpha) * (1.0f - 0.1f * beta * (1.0f - alpha)) * (1.0f - 0.5f * h->sepsis) /
                    (1.0f + 0.15f * h->shock_debt * h->shock_debt) * (cushing ? 1.3f : 1.0f) *
                    (1.0f + 0.3f * h->pressor);
    float skin = (1.0f + 1.5f * 0.2f) / (1.0f + 1.5f * h->vaso);
    for (int b = 0; b < CB_COUNT; b++) ci.r_bed[b] = ao.r_bed[b] * (b == CB_BRAIN ? 1.0f / brain_want : r_scale);
    float beta2 = 1.0f - 0.35f * beta * (1.0f - alpha);
    ci.r_bed[CB_UPPER] *= skin * beta2;
    ci.r_bed[CB_LEGS] *= skin * beta2;
    /* The veins: tone from the nerves and this body's own, slackened by
     * sepsis and warm skin (fully open, the skin's veins hold ~340 mL more,
     * the central volume heat stress costs), tightened by adrenaline. Hard exercise also
     * squeezes the gut's veins empty, moving about a litre toward the
     * heart. */
    ci.v0_scale = ao.v0_scale * h->base.venous_tone * (1.0f + 0.12f * h->sepsis) * (1.0f - 0.07f * alpha) *
                  (1.0f + 0.12f * (h->vaso - 0.2f)) * (1.0f - 0.2f * ex);
    /* Standing, the head sits 30 cm above the heart and the legs' veins
     * 60 cm below it. */
    if (!lying) {
        ci.h_head = crouched ? 0.2f : 0.3f;
        ci.h_legs = crouched ? -0.3f : -0.6f;
        ci.h_splanch = crouched ? -0.1f : -0.2f;
    }
    /* Each stride squeezes the calves' veins: the muscle pump. A crouch
     * folds the legs, squeezing their veins and kinking their arteries;
     * standing up lets them fill again all at once, which is the dizzy
     * moment after getting up from a squat. */
    if (awake && e->on_ground && !swimming && e->speed > 0.5) {
        float cadence = 0.9f + 0.25f * (float)e->speed;
        ci.leg_squeeze = 60.0f * maxf(0.0f, sinf(6.2831853f * cadence * (float)fmod(h->t, 1000.0)));
    }
    if (crouched && !sitting) {
        ci.leg_squeeze += 25.0f;
        ci.r_bed[CB_LEGS] *= 2.5f;
    }
    /* Breathing in lowers the pressure around the heart; straining against
     * a closed throat raises it (and the belly's), and air trapped in the
     * chest squeezes the heart from outside. */
    float insp = h->breathing && h->breath_phase < 0.4f ? sinf(3.1415927f * h->breath_phase / 0.4f) : 0.0f;
    ci.p_thorax = -4.0f - 4.0f * insp + 40.0f * e->bearing_down + 12.0f * h->pneumothorax * h->pneumothorax;
    ci.p_abdomen = 40.0f * e->bearing_down;

    /* Blood volume changed from outside (a burn's plasma, a debug shock):
     * every compartment takes its share. */
    float vol_now = circ_volume(&h->cv);
    if (vol_now > 1.0f && fabsf(h->blood * 1000.0f - vol_now) > 0.002f) {
        double k = (double)(h->blood * 1000.0f) / (double)vol_now;
        for (int i = 0; i < CC_COUNT; i++) h->cv.v[i] *= k;
    }
    step_bleeding(h, e, dt, gh, &ci);

    /* Step the circuit to each monitor sample time in turn: it integrates
     * absolute time in fixed sub-steps, so the split changes nothing, and
     * the traces get the circuit's real pressure and finger flow. */
    float art_s[64], pleth_s[64]; /* fully written below before any read, up to ns */
    int ns = 0;
    float lost = 0.0f;
    double tc = h->t - dt, q_ao = 0.0, q_brain = 0.0, q_cor = 0.0;
    {
        float acc = h->wave_acc + (float)(dt * HEALTH_WAVE_HZ);
        int kk = 0;
        for (;;) {
            double te = h->t;
            int sample = acc >= 1.0f;
            if (sample) {
                acc -= 1.0f;
                te = fmin(h->t - dt + ++kk / (double)HEALTH_WAVE_HZ, h->t);
            }
            if (te > tc) {
                circ_step(&h->cv, &ci, tc, te - tc);
                lost += h->cv.lost;
                q_ao += (double)h->cv.q_ao * (te - tc);
                q_brain += (double)h->cv.q_bed[CB_BRAIN] * (te - tc);
                q_cor += (double)h->cv.q_cor * (te - tc);
                tc = te;
            }
            if (!sample) break;
            if (ns < 64) {
                art_s[ns] = h->cv.wave_ao;
                pleth_s[ns] = maxf(0.0f, h->cv.wave_finger / Q_FINGER_REST - 0.5f);
                ns++;
            }
        }
        q_ao /= dt;
        q_brain /= dt;
        q_cor /= dt;
    }
    /* The wounds' rates above were priced at the pressures the step
     * started from; what reads out is what the circuit actually lost,
     * shared out in those proportions. */
    float read = h->bleed_ext + h->bleed_int;
    if (read > 0.0f && dt > 0.0) {
        float k = (float)((double)lost / dt * 60.0) / read;
        h->bleed_ext *= k;
        h->bleed_int *= k;
        for (int i = 0; i < h->wound_count; i++) h->wounds[i].bleed *= k;
    }
    account_blood(h, lost, gh, dehyd, (float)dt);

    /* The vitals, from the last completed beat; with no beat for a while
     * (or none that pumps), from the circuit as it is now. */
    if (h->cond.t_vent >= 0.0 && h->cond.t_vent != h->last_r) {
        h->prev_r = h->last_r;
        h->last_r = h->cond.t_vent;
        h->last_beat_pvc = h->cond.last_ectopic;
    }
    const int pumping = health_rhythm_perfusing(h->rhythm);
    const int fresh = pumping && h->cv.beat_len > 0.0f && h->t - h->last_r < 2.5;
    if (pumping) {
        double rr_now = h->prev_r >= 0.0 ? h->last_r - h->prev_r : 60.0 / (double)sa;
        rr_now = fmax(rr_now, h->t - h->last_r); /* a long pause shows as it happens */
        h->hr = lag(h->hr, (float)(60.0 / fmax(rr_now, 0.2)), dt, 1.0); /* a monitor averages a few beats */
        h->next_r = h->last_r + rr_now;
    } else {
        h->hr = 0.0f;
    }
    /* Readings are against the air outside: an emptied, collapsed aorta
     * sits a few mmHg under it, which a monitor shows as zero. */
    if (fresh) {
        h->sbp = maxf(0.0f, h->cv.p_ao_max);
        h->dbp = maxf(0.0f, h->cv.p_ao_min);
        h->map = maxf(0.0f, h->cv.p_ao_mean);
        h->sv = maxf(0.0f, h->cv.edv - h->cv.esv);
        h->co = lag(h->co, h->cv.beat_q_ao * 0.06f, dt, 2.0); /* output is averaged over a few beats */
        q_brain = (double)h->cv.beat_q_bed[CB_BRAIN];
        q_cor = (double)h->cv.beat_q_cor;
    } else {
        h->map = lag(h->map, maxf(0.0f, h->cv.p[CC_AO]), dt, 0.5);
        h->sbp = h->dbp = h->map;
        h->sv = 0.0f;
        h->co = lag(h->co, (float)q_ao * 0.06f, dt, 0.5);
    }
    h->svr = h->map / maxf(h->co, 0.1f);
    h->brain_flow = (float)q_brain / Q_BRAIN_REST;

    /* Myocardial oxygen: rate-pressure product against coronary supply. */
    if (!arrest) {
        /* Catecholamines make the myocardium burn more oxygen than its
         * rate-pressure product accounts for. */
        float demand = h->hr * h->sbp / (h->base.hr_rest * 120.0f) * (1.0f + 0.4f * beta);
        float coronary = (float)q_cor / Q_COR_REST * cao2_rel; /* the circuit's coronary flow, mostly diastolic */
        float supply = 6.0f * coronary * sqrtf(h->organ[ORG_HEART]) / (1.0f + 0.04f * h->strain);
        float ratio = demand / maxf(supply, 0.01f);
        if (ratio > 1.0f) h->ischemia += (ratio - 1.0f) * 0.08f * fdt;
        else h->ischemia = maxf(0.0f, h->ischemia - 0.02f * fdt);
        /* Profoundly hypoxic blood starves the myocardium however slowly
         * it beats: the path from drowning to arrest. */
        if (h->sao2 < 0.3f) h->ischemia += (0.3f - h->sao2) / 0.3f * 0.05f * fdt;
        h->ischemia = minf(h->ischemia, 10.0f);
        if (h->ischemia > 1.5f)
            h->organ[ORG_HEART] = maxf(0.0f, h->organ[ORG_HEART] - 0.0015f * (h->ischemia - 1.5f) * fdt);
        float vf = (h->ischemia > 3.0f ? 0.1f * (h->ischemia - 3.0f) : 0.0f) + (h->temp < 28.0f ? 0.05f : 0.0f) +
                   (h->organ[ORG_KIDNEYS] < 0.05f ? 0.01f : 0.0f) + 0.01f * maxf(0.0f, h->potassium - 7.5f);
        step_strain(h, ex, fdt);
        /* The heart muscle feeds on the pressure it makes: some seconds
         * with next to none starve it, and an empty heart's firing stops
         * making a pulse. A faint's brief dip doesn't. */
        h->empty_t = h->map < 18.0f && h->co < 0.3f ? h->empty_t + fdt : 0.0f;
        if (frand(h) < vf * fdt) stop_heart(h, RHYTHM_VF, e->airway);
        else if (h->hr < 20.0f) stop_heart(h, RHYTHM_ASYSTOLE, e->airway);
        /* An empty or obstructed heart still firing: the arrest of
         * exsanguination and tension pneumothorax. */
        else if (h->empty_t > 10.0f) stop_heart(h, RHYTHM_PEA, e->airway);
        else step_arrhythmia(h, e->airway, fdt, beta);
    } else {
        h->arrest_time += fdt;
        h->hr = 0.0f;
        if (h->rhythm == RHYTHM_VF && h->arrest_time > 300.0f) h->rhythm = RHYTHM_ASYSTOLE;
        /* Pulseless VT degenerates into VF within a minute or two. */
        else if (h->rhythm == RHYTHM_PVT && frand(h) < 0.02f * fdt) {
            h->rhythm = RHYTHM_VF;
            health_log(h, "Ventricular fibrillation!");
        }
    }

    step_breathing(h, e, arrest, awake, vo2_frac, hb, dt);

    step_brain(h, cao2_rel, dehyd, arrest, dt);

    organ_stress(h, dehyd, fdt);

    /* ---- slow processes on the survival clock: infection and healing */
    step_wounds(h, e, gh, dehyd);
    step_parts(h, e, dt, gh, awake);

    organ_recover(h, dehyd, gh);

    /* ---- death */
    /* VF under defibrillator pads is worth waiting for until the brain dies. */
    int shockable = h->defib.phase != DEFIB_OFF && health_rhythm_shockable(h->rhythm);
    if (arrest && !shockable && h->arrest_time >= ARREST_DEATH_S) die(h, h->cause ? h->cause : DEATH_CARDIAC);
    else if (h->organ[ORG_BRAIN] <= 0.0f) die(h, h->ich > 20.0f ? DEATH_HEAD_INJURY : arrest_cause(h, e->airway));
    else if (h->fat <= 0.0f) die(h, DEATH_STARVATION);
    else if (h->temp >= 43.5f) die(h, DEATH_HYPERTHERMIA);
    else if (h->temp <= 22.0f) die(h, DEATH_HYPOTHERMIA);
    else if (dehyd >= 0.2f) die(h, DEATH_DEHYDRATION);

    write_samples(h, e, dt, art_s, pleth_s, ns);
}

/* -------------------------------------------------------------- limits */

health_senses health_get_senses(const health *h)
{
    health_senses v = {0};
    if (h->dead) return v;
    float beta = catechol_beta(h);
    float crash = h->crash * (1.0f - fclamp(beta / 0.4f, 0.0f, 1.0f));
    v.tremor = fclamp(maxf(maxf(0.8f * ramp(beta, 0.3f, 0.9f), 0.7f * crash),
                           maxf(0.5f * ramp(150.0f - h->glycogen, 0.0f, 150.0f), ramp(36.0f - h->temp, 0.0f, 2.0f))),
                      0.0f, 1.0f);
    v.sway = fclamp(1.4f * (1.0f - h->organ[ORG_BRAIN]) + (h->confusion > 0.0f ? 0.5f : 0.0f) +
                        (h->concussion > 0.0f ? 0.3f : 0.0f) + 0.8f * ramp(h->bac, 0.5f, 2.5f) +
                        0.4f * ramp(opioid_free(h), 15.0f, 40.0f),
                    0.0f, 1.0f);
    v.tunnel = maxf(ramp(beta, 0.6f, 0.95f), ramp(h->vagal, 0.3f, 0.8f)); /* greying out */
    v.sway = minf(1.0f, v.sway + 0.4f * h->vagal);
    v.jaundice = fclamp((0.6f - h->organ[ORG_LIVER]) / 0.4f, 0.0f, 1.0f);
    /* Post-concussion vision clears over the last two minutes of the
     * confusion; the eyes stop tracking together (double vision) only in
     * its first minutes, or once the blood alcohol passes about 2 per mille. */
    v.blur = fclamp(0.7f * ramp(h->confusion, 0.0f, 120.0f) + 1.2f * (1.0f - h->organ[ORG_BRAIN]) +
                        0.5f * ramp(h->bac, 1.0f, 3.0f),
                    0.0f, 1.0f);
    v.doubled = fclamp(0.8f * ramp(h->confusion, 60.0f, 300.0f) + 0.6f * ramp(h->bac, 2.0f, 3.5f), 0.0f, 1.0f);
    /* Below alert, reactions slow (to about a third of a second when
     * drowsy), hands miss what they reach for, feet catch, and the body
     * sways. */
    const float fog = 1.0f - ramp(h->level, 0.3f, 0.85f);
    v.lag = 0.35f * fog;
    v.drop = 0.5f * fog;
    v.stumble = 0.5f * fog;
    v.sway = minf(1.0f, v.sway + 0.6f * fog);
    v.blackout = fclamp(h->lapse / 0.3f, 0.0f, 1.0f);
    /* A retina short of oxygen loses colour first and then the edges of
     * the view, before the brain itself goes: the grey-out, tunnel vision
     * and black-out pilots describe under g. Drowsy lids let in less
     * light. */
    v.grey = ramp(0.85f - h->brain_o2, 0.0f, 0.2f);
    v.tunnel = maxf(v.tunnel, ramp(0.75f - h->brain_o2, 0.0f, 0.25f));
    v.dim = maxf(0.4f * fog, v.blackout);
    if (!health_awake(h)) {
        /* Black, but for the glimpses: a dim, grey, blurred slit of the
         * world that closes again over its last fifth of a second. */
        const float open = fclamp(h->glimpse / 0.2f, 0.0f, 1.0f);
        v.dim = 1.0f - 0.6f * open;
        v.grey = maxf(v.grey, 0.7f);
        v.blur = maxf(v.blur, 0.8f);
        v.tunnel = maxf(v.tunnel, 0.5f);
    }
    return v;
}

health_limits health_get_limits(const health *h)
{
    health_limits l = {0.0f, 0, 0, 0, 0};
    if (h->dead || !health_awake(h) || h->lapse > 0.0f) return l;
    float leg[2] = {0};
    for (int k = 0; k < 2; k++) {
        const body_part *p = &h->part[BP_LLEG + k];
        float f = 1.0f;
        if (p->fracture) f = p->splinted ? 0.45f : 0.15f;
        f *= 1.0f - 0.6f * p->sprain * (p->splinted ? 0.5f : 1.0f);
        f *= 0.4f + 0.6f * p->integrity;
        f *= 1.0f - 0.6f * p->crush;
        if (p->burn >= 2) f *= 1.0f - 0.4f * p->burn_area; /* tight, raw skin over the joints */
        leg[k] = f;
    }
    float mob = maxf(sqrtf(leg[0] * leg[1]), 0.12f);
    float sys = 1.0f;
    float dehyd = dehydration(h);
    if (h->wbal < 0.05f * WPRIME) sys *= 0.55f;
    if (h->glycogen < 50.0f) sys *= 0.8f;
    if (dehyd > 0.1f) sys *= 0.6f;
    else if (dehyd > 0.06f) sys *= 0.85f;
    float v = h->blood / health_blood_normal(h);
    if (v < 0.65f) sys *= 0.5f;
    else if (v < 0.8f) sys *= 0.75f;
    if (h->sao2 < 0.85f) sys *= 0.7f;
    if (h->pain > 7.0f) sys *= 0.7f;
    if (health_impaired(h)) sys *= 0.7f;
    if (h->temp < 34.0f) sys *= 0.7f;
    /* A damaged heart cannot raise its output, a failing liver tires, and
     * a rush lends strength the crash after it takes back: to a fresh
     * body, not one whose reserve is already spent (hard exercise raises
     * adrenaline too). */
    float heart = h->organ[ORG_HEART], beta = catechol_beta(h);
    sys *= 0.5f + 0.5f * heart;
    if (h->organ[ORG_LIVER] < 0.5f) sys *= 0.85f;
    sys *= (1.0f - 0.3f * ramp(h->bac, 0.8f, 3.0f)) * (1.0f - 0.3f * ramp(opioid_free(h), 15.0f, 40.0f));
    sys *= 1.0f + 0.12f * beta * fclamp(2.0f * h->wbal / WPRIME - 1.0f, 0.0f, 1.0f) -
           0.2f * h->crash * (1.0f - fclamp(beta / 0.4f, 0.0f, 1.0f));
    l.move_scale = mob * sys;
    l.can_jump = leg[0] >= 0.6f && leg[1] >= 0.6f && h->wbal > 1500.0f && l.move_scale > 0.5f;
    l.can_sprint = mob > 0.85f && sys > 0.8f && h->wbal > 0.1f * WPRIME && h->pain < 7.0f && heart > 0.5f;
    int arms_ok = 0;
    for (int k = BP_LARM; k <= BP_RARM; k++) {
        const body_part *p = &h->part[k];
        arms_ok += !(p->fracture && !p->splinted) && !p->dislocated && p->crush < 0.6f && p->crush_load <= 0.0f;
    }
    l.can_act = arms_ok > 0;
    l.has_control = 1;
    return l;
}

/* -------------------------------------------------- debug injuries */

/* The 10 original --hurt names keep their exact parameters and parts:
 * changing them would change what a saved --hurt command line does.
 * Everything after "infection" is new, for the debug menu and for --hurt
 * to reach conditions that had no direct trigger before. */
const health_debug_kind HEALTH_DEBUG_KINDS[HEALTH_DEBUG_COUNT] = {
    {"bleed", "Bleeding (venous cut)", HDBG_ANY, BP_LLEG, -1},
    {"artery", "Arterial bleed", HDBG_ANY, BP_LLEG, -1},
    {"fracture", "Closed fracture", HDBG_ANY, BP_RARM, -1},
    {"open-fracture", "Open fracture", HDBG_ANY, BP_RLEG, -1},
    {"concussion", "Concussion", HDBG_HEAD, BP_HEAD, ORG_BRAIN},
    {"burn", "Burn (2nd degree)", HDBG_ANY, BP_LARM, -1},
    {"dislocation", "Dislocated shoulder", HDBG_ARM, BP_RARM, -1},
    {"abrasion", "Abrasion (road rash)", HDBG_ANY, BP_LLEG, -1},
    {"crush", "Crush injury", HDBG_ANY, BP_LLEG, -1},
    {"infection", "Infected wound", HDBG_ANY, BP_LARM, -1},
    {"burn1", "Burn (1st degree)", HDBG_ANY, BP_LARM, -1},
    {"burn3", "Burn (3rd degree)", HDBG_ANY, BP_LARM, -1},
    {"frostnip", "Frostnip", HDBG_ANY, BP_LARM, -1},
    {"frostbite", "Frostbite (superficial)", HDBG_ANY, BP_LARM, -1},
    {"deep-frostbite", "Frostbite (deep, frozen)", HDBG_ANY, BP_LARM, -1},
    {"hypothermia", "Hypothermia (core 33" DEG "C)", HDBG_SYSTEMIC, BP_HEAD, -1},
    {"hyperthermia", "Hyperthermia (core 40.5" DEG "C)", HDBG_SYSTEMIC, BP_HEAD, -1},
    {"pain", "Severe local pain", HDBG_ANY, BP_ABDOMEN, -1},
    {"shock", "Hypovolemic shock", HDBG_SYSTEMIC, BP_HEAD, -1},
    {"sepsis", "Sepsis", HDBG_SYSTEMIC, BP_HEAD, -1},
    {"vfib", "Cardiac arrest (V-fib)", HDBG_SYSTEMIC, BP_HEAD, ORG_HEART},
    {"asystole", "Cardiac arrest (asystole)", HDBG_SYSTEMIC, BP_HEAD, ORG_HEART},
    {"afib", "Atrial fibrillation", HDBG_SYSTEMIC, BP_HEAD, ORG_HEART},
    {"vtach", "Ventricular tachycardia", HDBG_SYSTEMIC, BP_HEAD, ORG_HEART},
    {"pvt", "Cardiac arrest (pulseless VT)", HDBG_SYSTEMIC, BP_HEAD, ORG_HEART},
    {"pea", "Cardiac arrest (PEA)", HDBG_SYSTEMIC, BP_HEAD, ORG_HEART},
    {"heart-block", "Complete heart block", HDBG_SYSTEMIC, BP_HEAD, ORG_HEART},
    {"crisis", "Hypertensive crisis (10 min)", HDBG_SYSTEMIC, BP_HEAD, ORG_HEART},
    {"strain", "Strained heart (long crisis)", HDBG_SYSTEMIC, BP_HEAD, ORG_HEART},
    {"drunk", "Drunk (2 g/L)", HDBG_SYSTEMIC, BP_HEAD, -1},
    {"overdose", "Opioid overdose (60 mg)", HDBG_SYSTEMIC, BP_HEAD, -1},
    {"faint", "Vasovagal faint", HDBG_SYSTEMIC, BP_HEAD, -1},
    {"brain-damage", "Brain damage (hypoxic)", HDBG_SYSTEMIC, BP_HEAD, ORG_BRAIN},
    {"heart-damage", "Damaged heart muscle", HDBG_SYSTEMIC, BP_CHEST, ORG_HEART},
    {"lung-damage", "Damaged lungs", HDBG_SYSTEMIC, BP_CHEST, ORG_LUNGS},
    {"liver-failure", "Failing liver", HDBG_SYSTEMIC, BP_ABDOMEN, ORG_LIVER},
    {"kidney-failure", "Failing kidneys", HDBG_SYSTEMIC, BP_ABDOMEN, ORG_KIDNEYS},
    {"gut-damage", "Damaged gut", HDBG_SYSTEMIC, BP_ABDOMEN, ORG_GUT},
};

int health_debug_find(const char *name)
{
    for (int i = 0; i < HEALTH_DEBUG_COUNT; i++)
        if (!strcmp(HEALTH_DEBUG_KINDS[i].name, name)) return i;
    return -1;
}

/* Sets a frostbite stage directly, through the same state step_frost
 * reads (freeze_time), so the result survives the next health_step
 * instead of being read back down to none. */
static void set_frostbite(health *h, int part, int stage)
{
    body_part *bp = &h->part[part];
    bp->freeze_time = stage == FROST_DEEP ? 1000.0f : (stage == FROST_SUPERFICIAL ? 300.0f : 20.0f);
    step_frost(h, part, 0.0f);
}

void health_injure(health *h, int kind, int part)
{
    if (h->dead || kind < 0 || kind >= HEALTH_DEBUG_COUNT) return;
    const health_debug_kind *k = &HEALTH_DEBUG_KINDS[kind];
    int p = k->default_part;
    if ((k->part_kind == HDBG_ANY && part >= 0 && part < BP_COUNT) || (k->part_kind == HDBG_ARM && is_arm(part)))
        p = part;
    switch (kind) {
    case 0: health_cut(h, p, 0.6f, 0, 0.3f); break;                    /* bleed */
    case 1: health_cut(h, p, 0.7f, 1, 0.2f); break;                    /* artery */
    case 2: health_break_bone(h, p, 0); break;                         /* fracture */
    case 3: health_break_bone(h, p, 1); break;                         /* open-fracture */
    case 4: health_blunt(h, BP_HEAD, 12.0); break;                     /* concussion */
    case 5: health_burn(h, p, 5.0); break;                             /* burn (2nd degree) */
    case 6: health_dislocate(h, p); break;                             /* dislocation */
    case 7: health_abrasion(h, p, 0.3f, 0.7f); break;                  /* abrasion */
    case 8: health_crush(h, p, 400.0, 600.0); break;                   /* crush */
    case 9:                                                            /* infection */
        if (h->wound_count < HEALTH_MAX_WOUNDS) {
            health_cut(h, p, 0.5f, 0, 0.9f);
            h->wounds[h->wound_count - 1].infection = 0.45f;
        }
        break;
    case 10: health_burn(h, p, 0.6); break;                            /* burn1 */
    case 11: health_burn(h, p, 2e4); break;                            /* burn3 */
    case 12: set_frostbite(h, p, FROST_NIP); break;                    /* frostnip */
    case 13: set_frostbite(h, p, FROST_SUPERFICIAL); break;            /* frostbite */
    case 14: set_frostbite(h, p, FROST_DEEP); break;                   /* deep-frostbite */
    case 15:                                                           /* hypothermia */
        h->temp = 33.0f;
        health_log(h, "Debug: hypothermia induced");
        break;
    case 16: /* hyperthermia */
        h->temp = 40.5f;
        health_log(h, "Debug: hyperthermia induced");
        break;
    case 17: h->part[p].pain_spike = 10.0f; break;                     /* pain */
    case 18:                                                           /* shock: sudden 45% blood loss */
        h->blood *= 0.55f;
        h->rbc *= 0.55f;
        h->shock_debt = maxf(h->shock_debt, 2.0f);
        health_log(h, "Debug: hypovolemic shock induced");
        break;
    case 19: /* sepsis */
        h->sepsis = maxf(h->sepsis, 0.7f);
        health_log(h, "Debug: sepsis induced");
        break;
    case 20: health_arrest(h, RHYTHM_VF); break;                       /* vfib */
    case 21: health_arrest(h, RHYTHM_ASYSTOLE); break;                 /* asystole */
    case 22: set_rhythm(h, RHYTHM_AF); break;
    case 23: set_rhythm(h, RHYTHM_VT); break;
    case 24: health_arrest(h, RHYTHM_PVT); break;
    case 25: health_arrest(h, RHYTHM_PEA); break;
    case 26: set_rhythm(h, RHYTHM_AVB3); break;
    case 27: /* crisis: a catecholamine surge, as from a phaeochromocytoma */
        h->surge = maxf(h->surge, 600.0f);
        h->surge_ng = CRISIS_NG_S;
        h->catechol = maxf(h->catechol, CRISIS_NG_S * ADR_HALF_S / 0.6931472f); /* the surge's plateau */
        health_log(h, "Debug: catecholamine surge");
        break;
    case 28: /* strain: the dose of about ten minutes of crisis */
        h->strain = maxf(h->strain, 10.0f);
        health_log(h, "Debug: strained heart");
        break;
    case 29: /* drunk: about seven drinks, absorbed */
        h->bac = maxf(h->bac, 2.0f);
        health_log(h, "Debug: drunk");
        break;
    case 30: /* overdose: six tablets' worth in the blood at once */
        h->opioid = maxf(h->opioid, 60.0f);
        health_log(h, "Debug: opioid overdose");
        break;
    case 31: /* faint */
        h->faint_drive = FAINT_S;
        health_log(h, "Debug: vasovagal reflex");
        break;
    case 32: /* brain-damage: what minutes without oxygen leave behind */
        h->organ[ORG_BRAIN] = minf(h->organ[ORG_BRAIN], 0.35f);
        health_log(h, "Debug: brain damage");
        break;
    case 33: /* heart-damage: what a large infarct leaves */
    case 34: /* lung-damage: contusion or smoke */
    case 35: /* liver-failure */
    case 36: /* kidney-failure */
    case 37: /* gut-damage: ischaemic bowel */
    {
        static const float LEFT[5] = {0.45f, 0.45f, 0.25f, 0.15f, 0.35f};
        const health_debug_kind *dk = &HEALTH_DEBUG_KINDS[kind];
        h->organ[dk->organ] = minf(h->organ[dk->organ], LEFT[kind - 33]);
        health_log(h, "Debug: %s", dk->label);
        break;
    }
    default: break;
    }
}

static const char *const DEBUG_TARGETS[HDBG_TARGET_COUNT] = {"Head",      "Chest",     "Abdomen",   "Left arm",
                                                             "Right arm", "Left leg",  "Right leg", "Brain",
                                                             "Heart",     "Lungs",     "Liver",     "Kidneys",
                                                             "Gut",       "Whole body"};

const char *health_debug_target_name(int target)
{
    return target >= 0 && target < HDBG_TARGET_COUNT ? DEBUG_TARGETS[target] : "";
}

int health_debug_fits(int kind, int target)
{
    if (kind < 0 || kind >= HEALTH_DEBUG_COUNT || target < 0 || target >= HDBG_TARGET_COUNT) return 0;
    const health_debug_kind *k = &HEALTH_DEBUG_KINDS[kind];
    if (target < BP_COUNT) {
        if (k->part_kind == HDBG_ANY) return 1;
        if (k->part_kind == HDBG_ARM) return is_arm(target);
        return k->part_kind == HDBG_HEAD && target == BP_HEAD;
    }
    if (target < HDBG_TARGET_BODY) return k->organ == target - HDBG_TARGET_ORGAN;
    return k->part_kind == HDBG_SYSTEMIC && k->organ < 0;
}

int health_debug_target_part(int target) { return target >= 0 && target < BP_COUNT ? target : -1; }

static void cat(char *buf, size_t n, size_t *len, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 4, 5)))
#endif
    ;

void health_debug_describe(const health *h, int target, char *buf, size_t n)
{
    if (!buf || n == 0) return;
    buf[0] = '\0';
    size_t len = 0;
    if (target < 0 || target >= HDBG_TARGET_COUNT) return;
    if (target == HDBG_TARGET_BODY) {
        cat(buf, n, &len, "HR %.0f  BP %.0f/%.0f  SpO2 %d%%  core %.1f" DEG "C  blood %.2f L", (double)h->hr,
            (double)h->sbp, (double)h->dbp, health_spo2_reading(h), (double)h->temp, (double)h->blood);
        return;
    }
    if (target >= HDBG_TARGET_ORGAN) {
        cat(buf, n, &len, "%s: function %.0f%%", DEBUG_TARGETS[target],
            100.0 * (double)h->organ[target - HDBG_TARGET_ORGAN]);
        return;
    }
    const body_part *bp = &h->part[target];
    float bleed = bp->internal;
    for (int i = 0; i < h->wound_count; i++)
        if (h->wounds[i].part == target) bleed += h->wounds[i].bleed;
    if (bleed > 0.5f) cat(buf, n, &len, "bleeding %.0f mL/min  ", (double)bleed);
    if (bp->fracture != FX_NONE) cat(buf, n, &len, "%s fracture  ", bp->fracture == FX_OPEN ? "open" : "closed");
    if (bp->dislocated != DISLOC_NONE) cat(buf, n, &len, "dislocated  ");
    if (bp->integrity < 0.9f) cat(buf, n, &len, "bruised (%.0f%%)  ", 100.0 * (double)bp->integrity);
    if (bp->burn > 0) cat(buf, n, &len, "burn (degree %d)  ", (int)bp->burn);
    if (bp->frost != FROST_NONE) cat(buf, n, &len, "frostbite  ");
    if (bp->crush > 0.05f) cat(buf, n, &len, "crushed  ");
    if (maxf(bp->pain, bp->pain_spike) > 1.0f)
        cat(buf, n, &len, "pain %.0f/10", (double)maxf(bp->pain, bp->pain_spike));
    if (len == 0) cat(buf, n, &len, "intact");
}

int health_debug_severity(const health *h, int target)
{
    if (target < 0 || target >= HDBG_TARGET_COUNT) return 0;
    if (target == HDBG_TARGET_BODY) {
        const float bn = health_blood_normal(h);
        if (!health_rhythm_perfusing(h->rhythm) || h->blood < bn * 0.7f) return 3;
        if (h->blood < bn * 0.85f || h->temp < 35.0f || h->temp > 39.0f || h->sepsis > 0.3f) return 2;
        return h->sepsis > 0.05f || h->bac > 0.5f || h->opioid > 10.0f || h->rhythm != RHYTHM_SINUS ? 1 : 0;
    }
    if (target >= HDBG_TARGET_ORGAN) {
        float f = h->organ[target - HDBG_TARGET_ORGAN];
        return f < 0.3f ? 3 : f < 0.6f ? 2 : f < 0.9f ? 1 : 0;
    }
    const body_part *bp = &h->part[target];
    float bleed = bp->internal;
    for (int i = 0; i < h->wound_count; i++)
        if (h->wounds[i].part == target) bleed += h->wounds[i].bleed;
    /* An arterial bleed empties the body in minutes; deep frostbite and
     * an open fracture cost the limb. */
    if (bleed > 50.0f || bp->fracture == FX_OPEN || bp->burn >= 3 || bp->frost == FROST_DEEP || bp->crush > 0.3f)
        return 3;
    if (bleed > 5.0f || bp->fracture != FX_NONE || bp->dislocated != DISLOC_NONE || bp->burn >= 2 ||
        bp->frost != FROST_NONE || bp->crush > 0.05f)
        return 2;
    return bleed > 0.5f || bp->burn > 0 || maxf(bp->pain, bp->pain_spike) > 1.0f || bp->integrity < 0.9f ? 1 : 0;
}

/* ------------------------------------------------------------- display */

int health_spo2_reading(const health *h)
{
    if (h->dead || !health_rhythm_perfusing(h->rhythm) || h->sbp - h->dbp < 10.0f || h->map < 35.0f) return -1;
    return (int)lroundf(fclamp(h->spo2_shown, 0.0f, 1.0f) * 100.0f);
}

float health_hydration(const health *h) { return fclamp(1.0f - dehydration(h) / 0.15f, 0.0f, 1.0f); }

float health_hunger(const health *h)
{
    return fclamp(1.5f * (1.0f - h->glycogen / GLYCOGEN_MAX) - h->stomach_kcal / 800.0f, 0.0f, 1.0f);
}

float health_stamina(const health *h) { return h->wbal / WPRIME; }

static void cat(char *buf, size_t n, size_t *len, const char *fmt, ...)
{
    if (*len >= n) return;
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(buf + *len, n - *len, fmt, ap);
    va_end(ap);
    if (w > 0) *len += (size_t)w < n - *len ? (size_t)w : n - *len - 1;
}

/* A status line built from comma-separated findings; sev is the worst. */
typedef struct {
    char *buf;
    size_t n, len;
    int sev;
} status_text;

static void status_add(status_text *s, int sev, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

static void status_add(status_text *s, int sev, const char *fmt, ...)
{
    if (s->len) cat(s->buf, s->n, &s->len, ", ");
    char item[96] = {0};
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(item, sizeof item, fmt, ap);
    va_end(ap);
    cat(s->buf, s->n, &s->len, "%s", item);
    if (sev > s->sev) s->sev = sev;
}

/* Bones, joints and weight on the part. */
static void status_structure(const health *h, int part, status_text *s)
{
    const body_part *p = &h->part[part];
    if (p->crush_load > 0.0f) status_add(s, part == BP_CHEST ? 3 : 2, "pinned under %.0f kg", (double)p->crush_load);
    if (p->fracture)
        status_add(s, 2, "%s fracture%s", p->fracture == FX_OPEN ? "open" : "closed", p->splinted ? " (splinted)" : "");
    if (p->dislocated) status_add(s, 2, "dislocated %s", p->dislocated == DISLOC_ELBOW ? "elbow" : "shoulder");
    if (p->sprain > 0.05f) status_add(s, 1, "sprain%s", p->splinted ? " (braced)" : "");
}

/* Cuts, grazes, burns, cold injury and infection. */
static void status_wounds(const health *h, int part, status_text *s)
{
    const body_part *p = &h->part[part];
    float bleed = 0.0f, inf = 0.0f;
    int wounds = 0, grazes = 0, bandaged = 0, arterial = 0, burn_dressed = 0;
    for (int i = 0; i < h->wound_count; i++) {
        const wound *w = &h->wounds[i];
        if (w->part != part) continue;
        inf = maxf(inf, w->infection);
        if (w->kind == WOUND_BURN || w->kind == WOUND_FROSTBITE) {
            burn_dressed |= w->kind == WOUND_BURN && w->bandage != 0;
            continue; /* named below */
        }
        wounds++;
        grazes += w->kind == WOUND_ABRASION;
        bleed += w->bleed;
        bandaged += w->bandage != 0;
        arterial |= w->arterial && w->bleed > 20.0f;
    }
    if (p->burn)
        status_add(s, p->burn == 3 || h->burn_tbsa > 15.0f ? 3 : p->burn, "%s-degree burn, %.0f%% of body%s",
                   ordinal(p->burn), (double)fmaxf(1.0f, p->burn_area * NINES[part] * 100.0f),
                   burn_dressed ? " (dressed)" : "");
    static const char *const FROST[] = {"", "frostnip", "frostbite", "deep frostbite"};
    if (p->frost) status_add(s, p->frost, "%s", FROST[p->frost]);
    else if (p->skin < 12.0f) status_add(s, 1, "numb with cold (%.0f" DEG "C)", (double)p->skin);
    if (wounds) {
        char what[48] = {0}, extra[48] = "";
        if (wounds == 1) snprintf(what, sizeof what, "%s", arterial ? "cut artery" : (grazes ? "abrasion" : "wound"));
        else if (grazes == wounds) snprintf(what, sizeof what, "%d abrasions", wounds);
        else snprintf(what, sizeof what, "%d wounds%s", wounds, arterial ? ", artery" : "");
        if (bleed >= 1.0f) snprintf(extra, sizeof extra, " bleeding %.0f mL/min", (double)bleed);
        status_add(s, bleed > 30.0f || arterial ? 3 : 1, "%s%s%s", what, extra,
                   bandaged ? (bandaged == wounds ? " (dressed)" : " (partly dressed)") : "");
    }
    if (inf > 0.1f)
        status_add(s, inf > 0.6f ? 3 : 2, "%s infection", inf > 0.6f ? "severe" : (inf > 0.3f ? "spreading" : "early"));
}

/* Bleeding inside, crushed muscle and bruising. */
static void status_tissue(const health *h, int part, status_text *s)
{
    const body_part *p = &h->part[part];
    if (p->internal > 1.0f)
        status_add(s, p->internal > 20.0f ? 3 : 2, "internal bleeding %.0f mL/min", (double)p->internal);
    if (p->crush > 0.05f) status_add(s, p->crush > 0.5f ? 3 : 2, "crushed muscle %.0f%%", (double)(p->crush * 100.0f));
    else if (p->integrity < 0.95f)
        status_add(s, p->integrity < 0.3f ? 2 : 1, "%s", p->integrity < 0.5f ? "badly bruised" : "bruised");
}

int health_part_status(const health *h, int part, char *buf, size_t n)
{
    status_text s = {buf, n, 0, 0};
    buf[0] = 0;
    status_structure(h, part, &s);
    status_wounds(h, part, &s);
    status_tissue(h, part, &s);
    if (!s.len) cat(buf, n, &s.len, "OK");
    return s.sev;
}

int health_organ_status(const health *h, int organ, char *buf, size_t n)
{
    float f = h->organ[organ];
    const char *s = NULL;
    int sev = f > 0.85f ? 0 : (f > 0.6f ? 1 : (f > 0.3f ? 2 : 3));
    switch (organ) {
    case ORG_BRAIN:
        if (h->icp > 25.0f) {
            s = "raised pressure";
            sev = 3;
        } else if (!health_awake(h)) {
            s = "unresponsive";
            sev = sev > 2 ? sev : 2;
        } else if (health_impaired(h)) {
            s = h->concussion > 0 || h->confusion > 0 ? "concussed" : "confused";
            sev = sev > 1 ? sev : 1;
        } else s = f > 0.85f ? "normal" : "damaged";
        break;
    case ORG_HEART:
        /* 14 characters at most: the panel's organ column. */
        if (h->rhythm == RHYTHM_VF) {
            s = "FIBRILLATING";
            sev = 3;
        } else if (h->rhythm == RHYTHM_ASYSTOLE) {
            s = "STOPPED";
            sev = 3;
        } else if (h->rhythm == RHYTHM_PVT) {
            s = "PULSELESS VT";
            sev = 3;
        } else if (h->rhythm == RHYTHM_PEA) {
            s = "NO OUTPUT: PEA";
            sev = 3;
        } else if (h->rhythm == RHYTHM_VT) {
            s = "V-tachycardia";
            sev = 3;
        } else if (h->rhythm == RHYTHM_AVB3) {
            s = "heart block";
            sev = sev > 2 ? sev : 2;
        } else if (h->rhythm == RHYTHM_AF) {
            s = "irregular (AF)";
            sev = sev > 2 ? sev : 2;
        } else if (h->ischemia > 1.0f) {
            s = "ischaemic";
            sev = sev > 2 ? sev : 2;
        } else if (h->potassium > 6.0f) {
            s = "high potassium";
            sev = sev > 2 ? sev : 2;
        } else if (h->strain > 4.0f) {
            s = "strained";
            sev = sev > 1 ? sev : 1;
        } else s = f > 0.85f ? "sinus rhythm" : "weakened";
        break;
    case ORG_LUNGS:
        if (h->pneumothorax > 0.05f) {
            s = "partly collapsed";
            sev = sev > 2 ? sev : 2;
        } else if (h->lung_water > 0.05f) {
            s = "water inhaled";
            sev = sev > 2 ? sev : 2;
        } else if (!h->breathing) {
            s = "not breathing";
            sev = 3;
        } else s = f > 0.85f ? "clear" : "contused";
        break;
    case ORG_KIDNEYS:
        if (h->myoglobin > 2.0f) {
            s = f > 0.3f ? "myoglobin, dark urine" : "failing (myoglobin)";
            sev = sev > 2 ? sev : 2;
        } else s = f > 0.85f ? "normal" : (f > 0.3f ? "injured" : "failing");
        break;
    default: s = f > 0.85f ? "normal" : (f > 0.3f ? "injured" : "failing"); break;
    }
    snprintf(buf, n, "%3.0f%%  %s", (double)(f * 100.0f), s);
    return sev;
}

/* ---------------------------------------------------------------- network */

#define H(f) WIRE_F32(health, f)
#define HD(f) WIRE_F64(health, f)
#define HI(f) WIRE_I32(health, f)
static const wire_field HEALTH_FIELDS[] = {
    H(organ[0]),
    H(organ[1]),
    H(organ[2]),
    H(organ[3]),
    H(organ[4]),
    H(organ[5]),
    WIRE_I32R(health, wound_count, 0, HEALTH_MAX_WOUNDS),
    H(blood),
    H(rbc),
    H(water),
    H(stomach_water),
    H(stomach_kcal),
    H(glycogen),
    H(fat),
    H(bleed_ext),
    H(bleed_int),
    H(hr),
    H(sv),
    H(co),
    H(svr),
    H(map),
    H(sbp),
    H(dbp),
    H(symp),
    H(ischemia),
    H(strain),
    H(catechol),
    H(surge),
    H(surge_ng),
    WIRE_I32R(health, rhythm, 0, RHYTHM_COUNT - 1),
    H(arrest_time),
    HI(last_beat_pvc),
    HD(last_r),
    HD(prev_r),
    HD(next_r),
    WIRE_I32R(health, defib.phase, DEFIB_OFF, DEFIB_CLEAR),
    WIRE_I32R(health, defib.seen, -1, RHYTHM_COUNT - 1),
    HI(defib.advice),
    HI(defib.shocks),
    H(defib.timer),
    H(defib.joules),
    H(defib.since_shock),
    H(brain_flow),
    H(plasma_deficit),
    H(rr),
    H(breath_phase),
    HI(breathing),
    H(o2_store),
    H(pao2),
    H(sao2),
    H(spo2_shown),
    H(paco2),
    H(etco2),
    H(lung_water),
    H(pneumothorax),
    H(apnea_time),
    H(gasp_timer),
    H(power),
    H(vo2),
    H(wbal),
    H(lactate),
    H(shock_debt),
    H(temp),
    H(vaso),
    H(pressor),
    H(sweat_w),
    H(sweat_acc),
    H(wet),
    H(heat_loss),
    H(burn_tbsa),
    H(air_temp),
    H(radiant),
    HI(in_fire),
    H(reperfused),
    H(myoglobin),
    H(potassium),
    H(brain_o2),
    H(ich),
    H(ich_rate),
    H(icp),
    H(concussion),
    H(confusion),
    H(pain),
    H(painkiller),
    H(pk_load),
    H(hepatotox),
    H(gut_upset),
    H(crash),
    H(pain_ref),
    H(opioid_gut),
    H(opioid),
    H(naloxone),
    H(alcohol_gut),
    H(bac),
    H(drink_load),
    H(vagal),
    H(faint_drive),
    H(blood_seen),
    H(down),
    H(level),
    H(lapse),
    H(glimpse),
    H(empty_t),
    H(fear),
    H(fear_ref),
    H(dark),
    H(antibiotic),
    H(sepsis),
    WIRE_I32R(health, conscious, CONS_UNCONSCIOUS, CONS_ALERT),
    HI(dead),
    WIRE_I32R(health, cause, DEATH_NONE, DEATH_OVERDOSE),
    H(hurt_flash),
    HD(t),
    WIRE_U32(health, rng),
    H(base.hr_rest),
    H(base.map_set),
    H(base.pain_gain),
    H(base.clot_gain),
    H(base.metab_gain),
    H(base.cold_bias),
    H(base.heart_size),
    H(base.art_stiff),
    H(base.blood_per_kg),
    H(base.baro_gain),
    H(base.venous_tone),
    H(base.symp_noise),
    H(base.hr_max),
    HI(base.vary),
    WIRE_U32(health, vrng),
    H(rr_noise),
    H(spo2_noise),
    WIRE_I32R(health, wave_pos, 0, HEALTH_WAVE_LEN - 1),
    H(wave_acc),
    H(log_age[0]),
    H(log_age[1]),
    H(log_age[2]),
    H(log_age[3]),
    H(log_age[4]),
};
#undef H
#undef HD
#undef HI

#define P(f) WIRE_F32(body_part, f)
static const wire_field PART_FIELDS[] = {
    P(integrity),
    WIRE_U8R(body_part, fracture, FX_NONE, FX_OPEN),
    WIRE_U8(body_part, splinted),
    WIRE_U8R(body_part, dislocated, DISLOC_NONE, DISLOC_ELBOW),
    P(fracture_heal),
    P(sprain),
    P(internal),
    P(pain),
    P(pain_spike),
    P(skin),
    P(surface),
    P(burn_omega),
    P(burn_area),
    P(burn_age),
    P(cool_time),
    WIRE_U8R(body_part, burn, 0, 3),
    WIRE_U8(body_part, burn_cooled),
    WIRE_U8R(body_part, frost, FROST_NONE, FROST_DEEP),
    P(freeze_time),
    P(frost_heal),
    P(crush),
    P(crush_released),
    P(crush_load),
    P(crush_next),
};
#undef P

#define W(f) WIRE_F32(wound, f)
static const wire_field WOUND_FIELDS[] = {
    WIRE_U8R(wound, part, 0, BP_COUNT - 1),
    WIRE_U8R(wound, kind, WOUND_CUT, WOUND_FROSTBITE),
    WIRE_U8R(wound, bandage, BANDAGE_NONE, BANDAGE_IMPROVISED),
    WIRE_U8(wound, arterial),
    W(depth),
    W(bleed0),
    W(bleed),
    W(clot),
    W(contamination),
    W(infection),
    W(closure),
    W(bandage_age),
    W(area),
};
#undef W

#define NFIELDS(t) (sizeof(t) / sizeof((t)[0]))

/* The newest HEALTH_NET_WAVE samples of a trace, oldest first. */
static void put_wave(wire_w *w, const float *ring, int pos)
{
    for (int i = HEALTH_NET_WAVE; i > 0; i--) wire_f32(w, ring[(pos - i + HEALTH_WAVE_LEN) % HEALTH_WAVE_LEN]);
}

static void get_wave(wire_r *r, float *ring, int pos)
{
    for (int i = HEALTH_NET_WAVE; i > 0; i--) ring[(pos - i + HEALTH_WAVE_LEN) % HEALTH_WAVE_LEN] = wire_get_f32(r);
}

size_t health_encode(const health *h, uint8_t *out, size_t cap)
{
    wire_w w = wire_writer(out, cap);
    wire_put_fields(&w, h, HEALTH_FIELDS, NFIELDS(HEALTH_FIELDS));
    for (int i = 0; i < BP_COUNT; i++) wire_put_fields(&w, &h->part[i], PART_FIELDS, NFIELDS(PART_FIELDS));
    for (int i = 0; i < h->wound_count && i < HEALTH_MAX_WOUNDS; i++)
        wire_put_fields(&w, &h->wounds[i], WOUND_FIELDS, NFIELDS(WOUND_FIELDS));
    for (int i = 0; i < HEALTH_LOG; i++) wire_str(&w, h->log[i], sizeof h->log[i] - 1);
    put_wave(&w, h->ecg, h->wave_pos);
    put_wave(&w, h->art, h->wave_pos);
    put_wave(&w, h->pleth, h->wave_pos);
    put_wave(&w, h->capno, h->wave_pos);
    return w.bad ? 0 : w.len;
}

int health_decode(health *h, const uint8_t *in, size_t len)
{
    /* Too big for the stack; the older trace samples carry over from h. */
    health *t = mem_alloc(sizeof *t);
    memcpy(t, h, sizeof *t);
    memset(t->wounds, 0, sizeof t->wounds);
    wire_r r = wire_reader(in, len);
    wire_get_fields(&r, t, HEALTH_FIELDS, NFIELDS(HEALTH_FIELDS));
    for (int i = 0; i < BP_COUNT; i++) wire_get_fields(&r, &t->part[i], PART_FIELDS, NFIELDS(PART_FIELDS));
    for (int i = 0; i < t->wound_count && !r.bad; i++)
        wire_get_fields(&r, &t->wounds[i], WOUND_FIELDS, NFIELDS(WOUND_FIELDS));
    for (int i = 0; i < HEALTH_LOG; i++) wire_get_str(&r, t->log[i], sizeof t->log[i]);
    if (!r.bad) {
        get_wave(&r, t->ecg, t->wave_pos);
        get_wave(&r, t->art, t->wave_pos);
        get_wave(&r, t->pleth, t->wave_pos);
        get_wave(&r, t->capno, t->wave_pos);
    }
    int ok = !r.bad && r.pos == len;
    if (ok) memcpy(h, t, sizeof *h);
    mem_free(t);
    return ok ? 0 : -1;
}

float health_blood_normal(const health *h) { return h->base.blood_per_kg * 75.0f / 1000.0f; }
