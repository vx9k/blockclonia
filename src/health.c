#include "health.h"
#include "block.h"

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
#define HR_MAX 190.0f
#define MAP_SET 93.0f
#define O2_VOL 10.0f         /* L: lungs after a breath plus blood and myoglobin stores */
#define HEAT_CAP (75.0f * 3500.0f)
#define RVR0 1.25f           /* resistance to venous return, mmHg min/L */
#define SVR0 18.75f          /* systemic vascular resistance at rest */
#define CSYS 0.2f            /* L/mmHg, systemic compliance */
#define SV_MAX 207.8f        /* mL; SV = SV_MAX * contractility * (RAP+1)/(RAP+4) */
#define ARREST_DEATH_S 60.0f /* no CPR or defibrillator in this world */

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

static float frand(health *h)
{
    uint32_t x = h->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    h->rng = x;
    return (float)(x >> 8) * (1.0f / 16777216.0f);
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
static float vo2max_l(void) { return VO2MAX_W / J_PER_ML_O2 * 0.06f; }

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
}

static const char *const PART_NAMES[BP_COUNT] = {"Head",      "Chest",    "Abdomen",  "Left arm",
                                                 "Right arm", "Left leg", "Right leg"};
static const char *const PART_NAMES_LC[BP_COUNT] = {"head",      "chest",    "abdomen",  "left arm",
                                                    "right arm", "left leg", "right leg"};
static const char *const BONE_NAMES[BP_COUNT] = {"skull",         "ribs",       "pelvis",     "left forearm",
                                                 "right forearm", "left tibia", "right tibia"};
static const char *const ORGAN_NAMES[ORG_COUNT] = {"Brain", "Heart", "Lungs", "Liver", "Kidneys", "Gut"};

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
    default: return "Alive";
    }
}

/* --------------------------------------------------------------- init */

void health_init(health *h, uint32_t seed)
{
    memset(h, 0, sizeof *h);
    h->rng = seed ? seed : 0x9E3779B9u;
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

    h->hr = HR_REST;
    h->sv = 75.0f;
    h->co = HR_REST * 75.0f / 1000.0f;
    h->svr = SVR0;
    h->map = MAP_SET;
    h->sbp = 120.0f;
    h->dbp = 80.0f;
    h->symp = 0.15f;
    h->rhythm = RHYTHM_SINUS;
    h->next_r = 60.0 / (double)HR_REST;
    h->prev_r = -60.0 / (double)HR_REST;
    h->beat_pp = 40.0f;
    h->beat_dbp = 80.0f;

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

    for (int i = 0; i < HEALTH_WAVE_LEN; i++) {
        h->art[i] = 80.0f;
        h->pleth[i] = 0.0f;
    }
    for (int i = 0; i < HEALTH_LOG; i++) h->log_age[i] = 1e9f;
}

/* ------------------------------------------------------------ injuries */

static wound *add_wound(health *h, int part, int kind, float depth, int arterial, float contamination)
{
    wound *w;
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
    int part;
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

int health_treat(health *h, inventory *inv, int part, int what, int water_nearby, char *msg, size_t msg_size)
{
    if (h->dead) {
        snprintf(msg, msg_size, "You are dead");
        return 0;
    }
    if (h->conscious == CONS_UNCONSCIOUS) {
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
        int kind;
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
        float c_mul, i_mul;
        if (inv_take(inv, I_ANTISEPTIC, 1)) {
            c_mul = 0.15f;
            i_mul = 0.5f;
        } else if (water_nearby) {
            c_mul = 0.5f;
            i_mul = 1.0f;
        } else {
            snprintf(msg, msg_size, "No antiseptic, and no water to rinse with");
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
                 c_mul < 0.2f ? "Cleaned the %s wounds with antiseptic" : "Rinsed the %s wounds with water", pn);
        return 1;
    }
    case TREAT_PAINKILLER:
        if (!inv_take(inv, I_PAINKILLER, 1)) {
            snprintf(msg, msg_size, "No painkillers left");
            return 0;
        }
        /* Doubling up on paracetamol-type drugs strains the liver. */
        if (h->painkiller > 3.0f) {
            h->organ[ORG_LIVER] = maxf(0.0f, h->organ[ORG_LIVER] - 0.08f);
            health_log(h, "Too many painkillers: liver strain");
        }
        h->painkiller = 6.0f;
        snprintf(msg, msg_size, "Took a painkiller (6 h)");
        return 1;
    case TREAT_ANTIBIOTIC:
        if (!inv_take(inv, I_ANTIBIOTIC, 1)) {
            snprintf(msg, msg_size, "No antibiotics left");
            return 0;
        }
        h->antibiotic = 7.0f * 24.0f;
        snprintf(msg, msg_size, "Started a 7-day course of antibiotics");
        return 1;
    case TREAT_EAT:
        if (inv_count(inv, I_APPLE) <= 0) {
            snprintf(msg, msg_size, "No food (apples drop from leaves)");
            return 0;
        }
        if (h->stomach_kcal > 1200.0f) {
            snprintf(msg, msg_size, "Too full to eat");
            return 0;
        }
        inv_take(inv, I_APPLE, 1);
        h->stomach_kcal += 95.0f;
        h->stomach_water += 0.085f;
        snprintf(msg, msg_size, "Ate an apple (95 kcal)");
        return 1;
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
    default: snprintf(msg, msg_size, "?"); return 0;
    }
}

/* ------------------------------------------------------- waveforms */

static float gauss(float t, float mu, float sigma)
{
    float d = (t - mu) / sigma;
    return expf(-0.5f * d * d);
}

/* One heartbeat's ECG (lead II, mV) at time t relative to its R peak. */
static float ecg_beat(float t, float rr, int pvc, float st_shift)
{
    if (t < -0.3f || t > 0.9f) return 0.0f;
    if (pvc) {
        /* Ventricular ectopic: no P wave, wide bizarre QRS, discordant T. */
        return 1.4f * gauss(t, 0.0f, 0.03f) - 0.7f * gauss(t, 0.07f, 0.03f) - 0.45f * gauss(t, 0.3f, 0.06f);
    }
    float qt = sqrtf(rr);
    float v = 0.15f * gauss(t, -0.16f, 0.025f) - 0.12f * gauss(t, -0.035f, 0.01f) + 1.2f * gauss(t, 0.0f, 0.012f) -
              0.3f * gauss(t, 0.035f, 0.012f) + 0.33f * gauss(t, 0.25f * qt, 0.045f * qt);
    if (st_shift != 0.0f && t > 0.05f && t < 0.2f * qt + 0.1f) v += st_shift;
    return v;
}

/* Arterial pulse contour, 0 at the foot and ~1 at the systolic peak. */
static float pulse_shape(float t, float up)
{
    if (t <= 0.0f) return 0.0f;
    if (t < up) return sinf(1.5707963f * t / up);
    const float notch = 0.3f;
    if (t < notch) {
        float k = (t - up) / (notch - up);
        return 1.0f - 0.42f * k - 0.06f * k * k;
    }
    float d = t - notch;
    return 0.52f * expf(-d / 0.5f) + 0.06f * gauss(d, 0.04f, 0.02f);
}

static float pulse_wave(float t, float rr, float up)
{
    if (rr < 0.2f) rr = 0.2f;
    if (t > rr) t = rr;
    return maxf(0.0f, pulse_shape(t, up) - pulse_shape(rr, up) * (t / rr));
}

static void write_samples(health *h, const health_env *e, double dt)
{
    h->wave_acc += (float)(dt * HEALTH_WAVE_HZ);
    const double ds = 1.0 / HEALTH_WAVE_HZ;
    double t0 = h->t - dt;
    int k = 0;
    while (h->wave_acc >= 1.0f) {
        h->wave_acc -= 1.0f;
        double ts = t0 + ds * ++k;
        if (ts > h->t) ts = h->t;
        float ecg = 0.0f, art, pleth, capno;
        float rr = 60.0f / maxf(h->hr, 1.0f);

        if (h->rhythm == RHYTHM_SINUS && !h->dead) {
            h->beat_phase += (float)(ds * (double)h->hr / 60.0);
            int fire = 0, pvc = 0;
            if (h->pvc_pending && h->beat_phase >= 0.62f) {
                fire = pvc = 1;
                h->pvc_pending = 0;
                h->beat_phase = -0.38f; /* compensatory pause */
            } else if (h->beat_phase >= 1.0f) {
                fire = 1;
                h->beat_phase -= 1.0f;
            }
            if (fire) {
                h->prev_r = h->last_r;
                h->last_r = ts;
                h->last_beat_pvc = pvc;
                h->beat_pp = (h->sbp - h->dbp) * (pvc ? 0.35f : 1.0f);
                h->beat_dbp = h->dbp;
                /* Ischaemic, acidotic, cold or hyperkalaemic hearts throw ectopics. */
                float p = 0.004f + fclamp((h->ischemia - 0.5f) * 0.1f, 0.0f, 0.4f) +
                          (h->lactate > 10.0f ? 0.05f : 0.0f) + (h->organ[ORG_KIDNEYS] < 0.15f ? 0.06f : 0.0f) +
                          (h->temp < 32.0f ? 0.05f : 0.0f) + 0.04f * maxf(0.0f, h->potassium - 6.0f);
                if (!pvc && frand(h) < p) h->pvc_pending = 1;
            }
            h->next_r = ts + (double)((1.0f - h->beat_phase) * rr);
            float st = h->ischemia > 1.0f ? -0.08f * minf(h->ischemia, 3.0f) : 0.0f;
            ecg = ecg_beat((float)(ts - h->last_r), rr, h->last_beat_pvc, st);
            if (!h->pvc_pending) ecg += ecg_beat((float)(ts - h->next_r), rr, 0, 0.0f);
        } else if (h->rhythm == RHYTHM_VF && !h->dead) {
            /* Coarse VF fading to fine VF as the myocardium runs out of energy. */
            float amp = 0.55f * expf(-h->arrest_time / 240.0f) + 0.08f;
            float s = (float)ts;
            ecg = amp * (0.6f * sinf(25.8f * s + 1.3f * sinf(1.7f * s)) + 0.4f * sinf(33.3f * s + 0.7f) +
                         0.3f * sinf(41.9f * s + 2.0f * sinf(0.9f * s)));
        }
        /* Baseline wander with breathing, and motion artefact when moving. */
        ecg += 0.04f * sinf(6.2831853f * h->breath_phase) +
               (frand(h) - 0.5f) * (0.02f + 0.03f * (float)fclamp((float)e->speed, 0.0f, 8.0f));

        int pulse = h->rhythm == RHYTHM_SINUS && !h->dead;
        if (pulse) {
            float t = (float)(ts - h->last_r) - 0.06f;
            float beat_rr = (float)(h->last_r - h->prev_r);
            if (t < 0.0f) t += beat_rr;
            art = h->beat_dbp + h->beat_pp * pulse_wave(t, maxf(rr, beat_rr), 0.09f);
            float tp = (float)(ts - h->last_r) - 0.22f;
            if (tp < 0.0f) tp += beat_rr;
            /* Vasoconstriction flattens the finger's pulse. */
            float pi = fclamp(h->beat_pp / 40.0f, 0.0f, 1.5f) * (1.1f - 0.7f * h->symp);
            pleth = pi * pulse_wave(tp, maxf(rr, beat_rr), 0.15f);
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
    if (h->burn_tbsa > 10.0f && h->blood < BLOOD_NORMAL * 0.8f) return DEATH_BURNS;
    if (h->ich > 25.0f || h->icp > 40.0f) return DEATH_HEAD_INJURY;
    if (h->blood < BLOOD_NORMAL * 0.72f) return DEATH_BLOOD_LOSS;
    if (h->sepsis > 0.6f) return DEATH_SEPSIS;
    if (h->temp < 30.0f) return DEATH_HYPOTHERMIA;
    if (h->temp > 41.5f) return DEATH_HYPERTHERMIA;
    if (dehydration(h) > 0.12f) return DEATH_DEHYDRATION;
    if (h->lung_water > 0.3f) return DEATH_DROWNING;
    return DEATH_CARDIAC;
}

static void die(health *h, int cause)
{
    if (h->dead) return;
    h->dead = 1;
    h->cause = cause;
    h->rhythm = RHYTHM_ASYSTOLE;
    h->conscious = CONS_UNCONSCIOUS;
    health_log(h, "%s", health_death_text(cause));
}

static void stop_heart(health *h, int rhythm, int airway)
{
    if (h->rhythm != RHYTHM_SINUS) return;
    h->rhythm = rhythm;
    h->arrest_time = 0.0f;
    h->cause = arrest_cause(h, airway);
    health_log(h, rhythm == RHYTHM_VF ? "Ventricular fibrillation!" : "Heart stopped (asystole)");
}

/* Venous return meets the heart's pumping curve: solve for right atrial
 * pressure where HR * SV(RAP) = (Pms - RAP) / Rvr (Guyton). */
static float solve_output(float hr, float contract, float pms, float rvr, float rap_extra, float *rap_out)
{
    if (pms <= rap_extra || hr <= 0.0f || contract <= 0.0f) {
        *rap_out = maxf(pms, 0.0f);
        return 0.0f;
    }
    float lo = 0.0f, hi = pms;
    for (int it = 0; it < 24; it++) {
        float rap = 0.5f * (lo + hi);
        float pump = hr * SV_MAX * contract * (rap + 1.0f) / (rap + 4.0f) * 0.001f;
        float ret = (pms - rap - rap_extra) / rvr;
        if (pump > ret) hi = rap;
        else lo = rap;
    }
    float rap = 0.5f * (lo + hi);
    *rap_out = rap;
    return maxf(0.0f, (pms - rap - rap_extra) / rvr);
}

/* Wounds bleed (less as they clot and when dressed), broken bones and torn
 * organs bleed inside, and plasma and red cells are replaced. */
static void step_bleeding(health *h, const health_env *e, double dt, float gh, float dehyd)
{
    const float fdt = (float)dt;
    float coag = sqrtf(h->organ[ORG_LIVER]) *
                 (h->temp < 35.0f ? fclamp(1.0f - (35.0f - h->temp) * 0.15f, 0.3f, 1.0f) : 1.0f) *
                 fclamp(hct(h) / 0.3f, 0.3f, 1.0f) * (1.0f - 0.5f * h->sepsis);
    float pressure = fclamp(h->map / MAP_SET, 0.0f, 1.5f);
    float ext = 0.0f;
    for (int i = 0; i < h->wound_count; i++) {
        wound *w = &h->wounds[i];
        float bf = 1.0f, tau;
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
        w->bleed = w->bleed0 * (1.0f - w->clot) * bf * pressure * (1.0f - w->closure);
        ext += w->bleed;
    }
    float internal = 0.0f;
    for (int i = 0; i < BP_COUNT; i++) {
        body_part *bp = &h->part[i];
        if (bp->internal <= 0.0f) continue;
        /* Tamponade and clotting slowly contain bleeding into tissue. */
        bp->internal *= (float)exp(-dt * (double)coag / (300.0 + 3.0 * (double)bp->internal));
        if (is_leg(i) && bp->fracture && !bp->splinted && e->speed > 0.5) bp->internal += 0.3f * (float)e->speed * fdt;
        if (bp->internal < 0.05f) bp->internal = 0.0f;
        internal += bp->internal * pressure;
    }
    h->bleed_ext = ext;
    h->bleed_int = internal;
    float lost = (ext + internal) * fdt / 60000.0f;
    h->rbc -= lost * hct(h);
    h->blood -= lost;
    h->water -= lost * 0.8f;
    h->ich += h->ich_rate * fdt / 60.0f;
    h->ich_rate *= (float)exp(-dt / 600.0);
    if (h->blood < 0.5f) h->blood = 0.5f;
    if (h->rbc < 0.05f) h->rbc = 0.05f;
    /* Plasma refills from the interstitium; red cells regrow over weeks. */
    float deficit = BLOOD_NORMAL - h->blood;
    if (deficit > 0.0f && dehyd < 0.15f) {
        /* Transcapillary refill, in real time (time constant ~10 min). */
        float refill = minf(deficit, deficit * fdt / 600.0f);
        h->blood += refill;
        h->water -= refill * 0.2f; /* the interstitium pays, which shows up as thirst */
    }
    if (h->rbc < BLOOD_NORMAL * 0.45f) h->rbc += 0.02f / 24.0f * gh * h->organ[ORG_KIDNEYS];
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
    float vaso_t = fclamp(0.1f + 2.5f * (tc - set) + 0.06f * (tsk - 32.0f), 0.0f, 1.0f);
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
        float dil = h->vaso, k;
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
    h->part[BP_CHEST].pain += 3.0f * h->pneumothorax;
    h->part[BP_HEAD].pain += minf(4.0f, h->ich * 0.15f) + (h->confusion > 0.0f ? 1.5f : 0.0f);
    float sum2 = 0.0f;
    for (int i = 0; i < BP_COUNT; i++) {
        h->part[i].pain = minf(PAIN_SCALE, h->part[i].pain);
        sum2 += h->part[i].pain * h->part[i].pain;
    }
    float pain = minf(PAIN_SCALE, sqrtf(sum2)) * (h->painkiller > 0.0f ? 0.45f : 1.0f);
    h->pain = awake ? lag(h->pain, pain, dt, 1.0) : 0.0f;
    h->painkiller = maxf(0.0f, h->painkiller - gh);
    h->antibiotic = maxf(0.0f, h->antibiotic - gh);
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
        write_samples(h, e, dt);
        return;
    }
    const float gh = (float)(dt * HEALTH_CLOCK / 3600.0); /* survival-clock hours this step */
    const float fdt = (float)dt;
    const int awake = h->conscious != CONS_UNCONSCIOUS;
    const float dehyd = dehydration(h);
    const float hb = health_hb(h);
    const float cao2_rel = (hb / 15.0f) * (h->sao2 / 0.975f);
    const int arrest = h->rhythm != RHYTHM_SINUS;

    /* ---- metabolism: power demand, stamina, VO2, lactate */
    float p = BMR_W * (1.0f + 0.1f * maxf(0.0f, h->temp - 37.0f));
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
    p += minf(400.0f, 250.0f * maxf(0.0f, 36.5f - h->temp) + 20.0f * skin_drive);
    float cap = cao2_rel * h->organ[ORG_HEART] * fclamp(h->blood / BLOOD_NORMAL, 0.0f, 1.0f) *
                (h->glycogen > 50.0f ? 1.0f : 0.7f) * (1.0f - fclamp(dehyd * 3.0f, 0.0f, 0.4f));
    cap = fclamp(cap, 0.05f, 1.0f);
    float cp = CRIT_POWER * cap, aer_max = VO2MAX_W * cap;
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
    float vo2_frac = h->vo2 / vo2max_l();
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
    float kcal_basal = BMR_W * 3600.0f / 4184.0f * gh;
    float kcal_ex = maxf(0.0f, p - BMR_W) * fdt / 4184.0f;
    float carb = 0.4f * kcal_basal + (0.5f + 0.45f * fclamp(p / CRIT_POWER, 0.0f, 1.0f)) * kcal_ex;
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

    step_burns(h, gh);
    step_crush(h, gh, dehyd);
    step_bleeding(h, e, dt, gh, dehyd);

    /* ---- circulation */
    float veff = h->blood * (1.0f - 0.6f * dehyd);
    /* Exercise drive: central command reacts at once, the metabolic part
     * follows oxygen uptake. */
    float ex = fclamp(0.35f * minf(p / VO2MAX_W, 1.0f) + 0.65f * vo2_frac, 0.0f, 1.0f);
    float b = fclamp((MAP_SET + 30.0f * ex - h->map) / 30.0f, -1.0f, 1.0f); /* baroreflex resets upward in exercise */
    float chemo = fclamp((0.92f - h->sao2) / 0.3f, 0.0f, 1.0f) + 0.6f * fclamp((h->paco2 - 45.0f) / 25.0f, 0.0f, 1.0f);
    int diving = e->airway == AIRWAY_WATER;
    h->icp = 10.0f + 0.8f * h->ich + 0.012f * h->ich * h->ich;
    int cushing = h->icp > 30.0f;

    float symp_t = 0.15f + 1.0f * maxf(b, 0.0f) + 0.4f * chemo + 0.03f * h->pain + (diving ? 0.2f : 0.0f) +
                   (cushing ? 0.5f : 0.0f) - 0.45f * maxf(-b, 0.0f);
    if (h->organ[ORG_BRAIN] < 0.15f) symp_t = 0.05f; /* autonomic failure */
    h->symp = lag(h->symp, fclamp(symp_t, 0.0f, 1.0f), dt, 4.0);

    float hr_max = HR_MAX * (0.55f + 0.45f * h->organ[ORG_HEART]);
    /* Chemoreceptors speed the heart only while the lungs inflate; in
     * apnoea the same reflex slows it (the diving response). */
    float drive = 0.95f * ex + 0.85f * maxf(b, 0.0f) + (h->breathing ? 0.5f : -0.2f) * chemo + 0.025f * h->pain +
                  0.07f * maxf(0.0f, h->temp - 37.0f) + 0.25f * h->sepsis - 0.4f * maxf(-b, 0.0f) +
                  (h->lactate > 4.0f ? 0.02f * (h->lactate - 4.0f) : 0.0f);
    float hr_rest = HR_REST * (h->temp < 35.0f ? fclamp(1.0f - (35.0f - h->temp) * 0.08f, 0.3f, 1.0f) : 1.0f);
    /* Above rest the drive spends the heart-rate reserve; below it, vagal
     * slowing scales the resting rate. */
    float hr_t = drive >= 0.0f ? hr_rest + (hr_max - hr_rest) * minf(drive, 1.0f)
                               : hr_rest * (1.0f + 0.8f * maxf(drive, -0.6f));
    if (diving && awake) hr_t *= 0.85f;                       /* diving reflex */
    if (cushing) hr_t = minf(hr_t, 50.0f);                     /* Cushing reflex */
    if (h->sao2 < 0.5f) hr_t *= 0.35f + 0.65f * h->sao2 / 0.5f; /* hypoxic bradycardia */
    if (h->organ[ORG_BRAIN] < 0.15f) hr_t = minf(hr_t, 50.0f);
    if (!arrest) h->hr = lag(h->hr, hr_t, dt, hr_t > h->hr ? 2.5 : 4.0);

    float contract = h->organ[ORG_HEART] * (0.85f + 0.35f * minf(1.0f, h->symp + ex)) *
                     (h->lactate > 8.0f ? 0.8f : 1.0f) * (h->sao2 < 0.6f ? 0.3f + 0.7f * h->sao2 / 0.6f : 1.0f);
    contract *= h->hr > 160.0f ? maxf(0.4f, 1.0f - (h->hr - 160.0f) / 200.0f) : 1.0f; /* short diastole */
    contract /= 1.0f + 0.25f * h->shock_debt * h->shock_debt;
    float vu = 4.05f - 1.3f * h->symp + 0.5f * h->sepsis;    /* unstressed volume, venoconstriction */
    float pms = maxf(0.0f, (veff - vu) / CSYS) * (1.0f + 1.3f * ex); /* muscle pump */
    float rvr = RVR0 * (1.0f - 0.4f * ex);
    float rap, co = 0.0f;
    if (!arrest) co = solve_output(h->hr, contract, pms, rvr, 12.0f * h->pneumothorax * h->pneumothorax, &rap);
    else rap = pms / (1.0f + 0.0f);
    h->co = co;
    h->sv = h->hr > 1.0f ? co * 1000.0f / h->hr : 0.0f;
    h->svr = SVR0 * (0.85f + 0.9f * h->symp) / 0.985f * (1.0f - 0.6f * ex) * (1.0f - 0.5f * h->sepsis) /
             (1.0f + 0.15f * h->shock_debt * h->shock_debt);
    float map_t = co * h->svr + rap;
    h->map = lag(h->map, map_t, dt, 1.0);
    float pp = arrest ? 0.0f : h->sv / 1.9f * (1.0f + 0.6f * ex + maxf(0.0f, (h->map - MAP_SET) / 150.0f));
    h->sbp = h->map + pp * 2.0f / 3.0f;
    h->dbp = h->map - pp / 3.0f;

    /* Myocardial oxygen: rate-pressure product against coronary supply. */
    if (!arrest) {
        float demand = h->hr * h->sbp / (HR_REST * 120.0f);
        float diast = fclamp(1.2f - h->hr / 300.0f, 0.3f, 1.0f) / 0.987f;
        float coronary = fclamp((h->dbp - 5.0f) / 73.0f, 0.0f, 2.0f) * cao2_rel * diast;
        float supply = 6.0f * coronary * sqrtf(h->organ[ORG_HEART]);
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
        if (frand(h) < vf * fdt) stop_heart(h, RHYTHM_VF, e->airway);
        else if (h->hr < 20.0f || (h->map < 18.0f && h->co < 0.3f)) stop_heart(h, RHYTHM_ASYSTOLE, e->airway);
    } else {
        h->arrest_time += fdt;
        h->hr = 0.0f;
        if (h->rhythm == RHYTHM_VF && h->arrest_time > 300.0f) h->rhythm = RHYTHM_ASYSTOLE;
    }

    /* ---- breathing */
    float lung_eff = h->organ[ORG_LUNGS] * (1.0f - h->pneumothorax) * (1.0f - fclamp(h->lung_water / 1.5f, 0.0f, 0.9f));
    int drive_ok = h->organ[ORG_BRAIN] > 0.08f && !(arrest && h->arrest_time > 20.0f);
    /* A weight on the chest (or, less, on the belly, splinting the
     * diaphragm) stops the ribs rising: traumatic asphyxia past ~250 kg. */
    float vent = 1.0f - ramp(h->part[BP_CHEST].crush_load + 0.5f * h->part[BP_ABDOMEN].crush_load, 30.0f, 250.0f);
    int can_air = e->airway == AIRWAY_AIR && vent > 0.08f;
    /* Tissues extract less as the store runs dry and the brain shuts down. */
    h->o2_store = maxf(0.0f, h->o2_store - h->vo2 * fclamp(h->pao2 / 25.0f, 0.3f, 1.0f) / 60.0f * fdt);
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
        h->rr = lag(h->rr, fclamp(rr_t, 4.0f, 55.0f), dt, 4.0);
        float pao2_target = maxf(0.0f, 0.21f * 713.0f - h->paco2 / 0.8f);
        float store_t = O2_VOL * pao2_target / 713.0f;
        h->o2_store = lag(h->o2_store, store_t, dt, 6.0 * 14.0 / (double)maxf(h->rr * vent, 1.0f));
        float alveolar = maxf(h->rr - 0.6f * minf(shallow, maxf(h->rr - rr_need, 0.0f)), 4.0f) * vent;
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
    h->etco2 = h->breathing ? maxf(0.0f, h->paco2 - 3.0f - 25.0f * maxf(0.0f, 1.0f - h->co / 4.0f)) : 0.0f;

    /* ---- brain */
    float cpp = h->map - h->icp;
    float cbf = cpp >= 55.0f ? 1.0f : maxf(0.0f, cpp / 55.0f);
    cbf *= fclamp(1.0f + 0.025f * (h->paco2 - 40.0f), 0.6f, 1.6f) * (1.0f + 0.5f * maxf(0.0f, 0.9f - h->sao2));
    cbf *= fclamp(powf(15.0f / maxf(hb, 3.0f), 0.8f), 1.0f, 1.8f); /* thinner blood flows faster */
    h->brain_o2 = lag(h->brain_o2, cbf * cao2_rel, dt, 8.0);
    if (h->brain_o2 < 0.3f) h->organ[ORG_BRAIN] -= (0.3f - h->brain_o2) / 0.3f * fdt / 300.0f;
    if (h->icp > 40.0f) h->organ[ORG_BRAIN] -= fdt / 120.0f;
    if (h->temp > 41.0f) h->organ[ORG_BRAIN] -= (h->temp - 41.0f) * fdt / 600.0f;
    h->organ[ORG_BRAIN] = maxf(0.0f, h->organ[ORG_BRAIN]);
    h->concussion = maxf(0.0f, h->concussion - fdt);
    h->confusion = maxf(0.0f, h->confusion - fdt);

    /* The brain extracts more oxygen as delivery falls, so function holds
     * until delivery is roughly halved: confusion near 55%, fainting
     * below about 40%. Extra flow cannot make up for very low arterial
     * oxygen, though: oxygen has to diffuse into the tissue, and acute
     * hypoxaemia blacks people out near SaO2 50% whatever the flow. */
    float bo = h->brain_o2;
    int cons = CONS_ALERT;
    if (bo < 0.55f || h->sao2 < 0.7f || h->blood < 0.7f * BLOOD_NORMAL || h->confusion > 0.0f || h->temp < 33.0f ||
        h->temp > 40.0f || h->sepsis > 0.7f || h->organ[ORG_BRAIN] < 0.6f || dehyd > 0.1f || h->fat < 2000.0f)
        cons = CONS_CONFUSED;
    int out = h->conscious == CONS_UNCONSCIOUS; /* hysteresis */
    if (bo < (out ? 0.45f : 0.4f) || h->sao2 < (out ? 0.55f : 0.5f) || h->concussion > 0.0f || h->temp < 30.0f ||
        h->temp > 41.5f || h->organ[ORG_BRAIN] < 0.3f || h->icp > 35.0f || h->sepsis > 0.92f || arrest)
        cons = CONS_UNCONSCIOUS;
    if (cons == CONS_UNCONSCIOUS && h->conscious != CONS_UNCONSCIOUS) health_log(h, "Lost consciousness");
    h->conscious = cons;

    /* ---- organs under stress (real time) */
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

    /* ---- slow processes on the survival clock: infection and healing */
    step_wounds(h, e, gh, dehyd);
    step_parts(h, e, dt, gh, awake);

    /* Recovery. */
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

    /* ---- death */
    if (arrest && h->arrest_time >= ARREST_DEATH_S) die(h, h->cause ? h->cause : DEATH_CARDIAC);
    else if (h->organ[ORG_BRAIN] <= 0.0f) die(h, h->ich > 20.0f ? DEATH_HEAD_INJURY : arrest_cause(h, e->airway));
    else if (h->fat <= 0.0f) die(h, DEATH_STARVATION);
    else if (h->temp >= 43.5f) die(h, DEATH_HYPERTHERMIA);
    else if (h->temp <= 22.0f) die(h, DEATH_HYPOTHERMIA);
    else if (dehyd >= 0.2f) die(h, DEATH_DEHYDRATION);

    write_samples(h, e, dt);
}

/* -------------------------------------------------------------- limits */

health_limits health_get_limits(const health *h)
{
    health_limits l = {0.0f, 0, 0, 0, 0};
    if (h->dead || h->conscious == CONS_UNCONSCIOUS) return l;
    float leg[2];
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
    float v = h->blood / BLOOD_NORMAL;
    if (v < 0.65f) sys *= 0.5f;
    else if (v < 0.8f) sys *= 0.75f;
    if (h->sao2 < 0.85f) sys *= 0.7f;
    if (h->pain > 7.0f) sys *= 0.7f;
    if (h->conscious == CONS_CONFUSED) sys *= 0.7f;
    if (h->temp < 34.0f) sys *= 0.7f;
    l.move_scale = mob * sys;
    l.can_jump = leg[0] >= 0.6f && leg[1] >= 0.6f && h->wbal > 1500.0f && l.move_scale > 0.5f;
    l.can_sprint = mob > 0.85f && sys > 0.8f && h->wbal > 0.1f * WPRIME && h->pain < 7.0f;
    int arms_ok = 0;
    for (int k = BP_LARM; k <= BP_RARM; k++) {
        const body_part *p = &h->part[k];
        arms_ok += !(p->fracture && !p->splinted) && !p->dislocated && p->crush < 0.6f && p->crush_load <= 0.0f;
    }
    l.can_act = arms_ok > 0;
    l.has_control = 1;
    return l;
}

/* ------------------------------------------------------------- display */

int health_spo2_reading(const health *h)
{
    if (h->dead || h->rhythm != RHYTHM_SINUS || h->beat_pp < 10.0f || h->map < 35.0f) return -1;
    return (int)lroundf(fclamp(h->spo2_shown, 0.0f, 1.0f) * 100.0f);
}

float health_hydration(const health *h) { return fclamp(1.0f - dehydration(h) / 0.15f, 0.0f, 1.0f); }

float health_hunger(const health *h)
{
    return fclamp(1.5f * (1.0f - h->glycogen / GLYCOGEN_MAX) - h->stomach_kcal / 800.0f, 0.0f, 1.0f);
}

float health_stamina(const health *h) { return h->wbal / WPRIME; }

__attribute__((format(printf, 4, 5))) static void cat(char *buf, size_t n, size_t *len, const char *fmt, ...)
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

__attribute__((format(printf, 3, 4))) static void status_add(status_text *s, int sev, const char *fmt, ...)
{
    if (s->len) cat(s->buf, s->n, &s->len, ", ");
    char item[96];
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
        char what[48], extra[48] = "";
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
    const char *s;
    int sev = f > 0.85f ? 0 : (f > 0.6f ? 1 : (f > 0.3f ? 2 : 3));
    switch (organ) {
    case ORG_BRAIN:
        if (h->icp > 25.0f) {
            s = "raised pressure";
            sev = 3;
        } else if (h->conscious == CONS_UNCONSCIOUS) {
            s = "unresponsive";
            sev = sev > 2 ? sev : 2;
        } else if (h->conscious == CONS_CONFUSED) {
            s = h->concussion > 0 || h->confusion > 0 ? "concussed" : "confused";
            sev = sev > 1 ? sev : 1;
        } else s = f > 0.85f ? "normal" : "damaged";
        break;
    case ORG_HEART:
        if (h->rhythm == RHYTHM_VF) {
            s = "FIBRILLATING";
            sev = 3;
        } else if (h->rhythm == RHYTHM_ASYSTOLE) {
            s = "STOPPED";
            sev = 3;
        } else if (h->ischemia > 1.0f) {
            s = "ischaemic";
            sev = sev > 2 ? sev : 2;
        } else if (h->potassium > 6.0f) {
            s = "high potassium";
            sev = sev > 2 ? sev : 2;
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
