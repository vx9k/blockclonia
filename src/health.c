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

static float clampf_(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static float maxf(float a, float b) { return a > b ? a : b; }
static float minf(float a, float b) { return a < b ? a : b; }

/* 0 below a, 1 above b, smooth in between. */
static float ramp(float v, float a, float b)
{
    float t = clampf_((v - a) / (b - a), 0.0f, 1.0f);
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
    double p = po2;
    return (float)(1.0 / (23400.0 / (p * p * p + 150.0 * p) + 1.0));
}

static float dehydration(const health *h) { return maxf(0.0f, 1.0f - h->water / WATER_NORMAL); }
static float hct(const health *h) { return h->blood > 0.05f ? h->rbc / h->blood : 0.0f; }
float health_hb(const health *h) { return hct(h) * 33.3f; }
static float vo2max_l(void) { return VO2MAX_W / J_PER_ML_O2 * 0.06f; }

static int is_limb(int part) { return part >= BP_LARM; }
static int is_leg(int part) { return part == BP_LLEG || part == BP_RLEG; }

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

static const char *PART_NAMES[BP_COUNT] = {"Head", "Chest", "Abdomen", "Left arm", "Right arm", "Left leg",
                                           "Right leg"};
static const char *PART_NAMES_LC[BP_COUNT] = {"head", "chest", "abdomen", "left arm", "right arm", "left leg",
                                              "right leg"};
static const char *BONE_NAMES[BP_COUNT] = {"skull", "ribs", "pelvis", "left forearm", "right forearm",
                                           "left tibia", "right tibia"};
static const char *ORGAN_NAMES[ORG_COUNT] = {"Brain", "Heart", "Lungs", "Liver", "Kidneys", "Gut"};

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
    default: return "Alive";
    }
}

const char *health_treat_name(int what)
{
    static const char *n[TREAT_COUNT] = {"Bandage", "Splint", "Disinfect", "Painkiller", "Antibiotics",
                                         "Eat apple", "Drink"};
    return what >= 0 && what < TREAT_COUNT ? n[what] : "?";
}

/* --------------------------------------------------------------- init */

void health_init(health *h, uint32_t seed)
{
    memset(h, 0, sizeof *h);
    h->rng = seed ? seed : 0x9E3779B9u;
    for (int i = 0; i < BP_COUNT; i++) h->part[i].integrity = 1.0f;
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
    h->next_r = 60.0 / HR_REST;
    h->prev_r = -60.0 / HR_REST;
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

    h->brain_o2 = 1.0f;
    h->icp = 10.0f;
    h->conscious = CONS_ALERT;

    h->items = (survival_items){.bandages = 3, .splints = 1, .antiseptic = 2, .painkillers = 4,
                                .antibiotics = 1, .apples = 3};
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
    w->depth = clampf_(depth, 0.0f, 1.0f);
    w->arterial = (uint8_t)arterial;
    /* Capillary/venous ooze grows with depth; a severed artery pours, and
     * deeper cuts reach bigger arteries (a femoral bleed tops 1 L/min). */
    w->bleed0 = arterial ? 150.0f + 850.0f * w->depth * w->depth : 3.0f + 60.0f * w->depth * w->depth;
    w->bleed = w->bleed0;
    w->contamination = clampf_(contamination, 0.0f, 1.0f);
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

static void limb_injury(health *h, int part, float s)
{
    if (s < 0.1f) return;
    body_part *p = &h->part[part];
    p->integrity = maxf(0.0f, p->integrity - 0.6f * minf(s, 1.0f));
    if (frand(h) < ramp(s, 0.3f, 0.9f)) break_bone(h, part, s > 0.8f && frand(h) < 0.5f);
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
        p->integrity = maxf(0.0f, p->integrity - clampf_((v - 5.0f) * 0.05f, 0.0f, 0.9f));
        float t = clampf_((v - 6.5f) / 6.0f, 0.0f, 1.0f);
        if (frand(h) < t * t) {
            break_bone(h, leg, v > 11.0f && frand(h) < 0.6f * ramp(v, 11.0f, 16.0f));
        } else if (frand(h) < 0.6f * ramp(v, 5.5f, 9.0f)) {
            p->sprain = maxf(p->sprain, 0.3f + 0.5f * frand(h));
            health_log(h, "%s sprained", PART_NAMES[leg]);
        }
    }
    /* Arms brace the fall (wrist fractures). */
    for (int arm = BP_LARM; arm <= BP_RARM; arm++) {
        h->part[arm].integrity = maxf(0.0f, h->part[arm].integrity - clampf_((v - 7.0f) * 0.04f, 0.0f, 0.6f));
        if (frand(h) < 0.5f * ramp(v, 8.0f, 14.0f)) break_bone(h, arm, 0);
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
    double e = 0.5 * mass * speed * speed * (1.0 - clampf_((float)softness, 0.0f, 0.98f));
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
    if (arterial) health_log(h, "Artery cut on the %s: %.0f mL/min", PART_NAMES_LC[part], w->bleed0);
    else health_log(h, "%s cut on the %s", severity > 0.5f ? "Deep" : "Shallow", PART_NAMES_LC[part]);
}

void health_break_bone(health *h, int part, int open)
{
    if (h->dead || part < 0 || part >= BP_COUNT) return;
    break_bone(h, part, open);
    h->hurt_flash = minf(1.0f, h->hurt_flash + 0.6f);
}

/* ------------------------------------------------------------ treatment */

int health_treat(health *h, int part, int what, int water_nearby, char *msg, size_t n)
{
    survival_items *it = &h->items;
    if (h->dead) { snprintf(msg, n, "You are dead"); return 0; }
    if (h->conscious == CONS_UNCONSCIOUS) { snprintf(msg, n, "You are unconscious"); return 0; }
    if (part < 0 || part >= BP_COUNT) part = BP_CHEST;
    const char *pn = PART_NAMES_LC[part];

    switch (what) {
    case TREAT_BANDAGE: {
        int need = 0;
        for (int i = 0; i < h->wound_count; i++) {
            wound *w = &h->wounds[i];
            if (w->part == part && (!w->bandage || w->bandage_age > 24.0f || w->bleed > 5.0f)) need = 1;
        }
        if (!need) { snprintf(msg, n, "No open wound to dress on the %s", pn); return 0; }
        int kind;
        if (it->bandages > 0) { it->bandages--; kind = BANDAGE_STERILE; }
        else if (it->fibre >= 2) { it->fibre -= 2; kind = BANDAGE_IMPROVISED; }
        else { snprintf(msg, n, "No bandages (2 plant fibre make one)"); return 0; }
        for (int i = 0; i < h->wound_count; i++) {
            wound *w = &h->wounds[i];
            if (w->part != part) continue;
            w->bandage = (uint8_t)kind;
            w->bandage_age = 0.0f;
            if (kind == BANDAGE_IMPROVISED) w->contamination = minf(1.0f, w->contamination + 0.08f);
        }
        snprintf(msg, n, "Pressure dressing on the %s (%s)", pn, kind == BANDAGE_STERILE ? "sterile" : "improvised");
        return 1;
    }
    case TREAT_SPLINT: {
        body_part *p = &h->part[part];
        if (!is_limb(part) && part != BP_ABDOMEN) { snprintf(msg, n, "The %s can't be splinted", pn); return 0; }
        if ((!p->fracture && p->sprain < 0.05f) || p->splinted) {
            snprintf(msg, n, p->splinted ? "The %s is already splinted" : "Nothing to splint on the %s", pn);
            return 0;
        }
        if (it->splints > 0) it->splints--;
        else if (it->sticks >= 2 && it->fibre >= 1) { it->sticks -= 2; it->fibre -= 1; }
        else { snprintf(msg, n, "No splint (2 sticks + 1 fibre make one)"); return 0; }
        p->splinted = 1;
        snprintf(msg, n, part == BP_ABDOMEN ? "Pelvic binder tied" : "Splinted the %s", pn);
        return 1;
    }
    case TREAT_DISINFECT: {
        int any = 0;
        for (int i = 0; i < h->wound_count; i++) any |= h->wounds[i].part == part;
        if (!any) { snprintf(msg, n, "No wound on the %s", pn); return 0; }
        float c_mul, i_mul;
        if (it->antiseptic > 0) { it->antiseptic--; c_mul = 0.15f; i_mul = 0.5f; }
        else if (water_nearby) { c_mul = 0.5f; i_mul = 1.0f; }
        else { snprintf(msg, n, "No antiseptic, and no water to rinse with"); return 0; }
        for (int i = 0; i < h->wound_count; i++) {
            wound *w = &h->wounds[i];
            if (w->part != part) continue;
            w->contamination *= c_mul;
            /* Surface antisepsis only reaches an early, shallow infection. */
            w->infection *= w->infection < 0.35f ? i_mul : 0.9f;
        }
        h->part[part].pain = minf(PAIN_SCALE, h->part[part].pain + 2.0f);
        snprintf(msg, n, c_mul < 0.2f ? "Cleaned the %s wounds with antiseptic" : "Rinsed the %s wounds with water", pn);
        return 1;
    }
    case TREAT_PAINKILLER:
        if (it->painkillers <= 0) { snprintf(msg, n, "No painkillers left"); return 0; }
        it->painkillers--;
        /* Doubling up on paracetamol-type drugs strains the liver. */
        if (h->painkiller > 3.0f) {
            h->organ[ORG_LIVER] = maxf(0.0f, h->organ[ORG_LIVER] - 0.08f);
            health_log(h, "Too many painkillers: liver strain");
        }
        h->painkiller = 6.0f;
        snprintf(msg, n, "Took a painkiller (6 h)");
        return 1;
    case TREAT_ANTIBIOTIC:
        if (it->antibiotics <= 0) { snprintf(msg, n, "No antibiotics left"); return 0; }
        it->antibiotics--;
        h->antibiotic = 7.0f * 24.0f;
        snprintf(msg, n, "Started a 7-day course of antibiotics");
        return 1;
    case TREAT_EAT:
        if (it->apples <= 0) { snprintf(msg, n, "No food (apples drop from leaves)"); return 0; }
        if (h->stomach_kcal > 1200.0f) { snprintf(msg, n, "Too full to eat"); return 0; }
        it->apples--;
        h->stomach_kcal += 95.0f;
        h->stomach_water += 0.085f;
        snprintf(msg, n, "Ate an apple (95 kcal)");
        return 1;
    case TREAT_DRINK:
        if (!water_nearby) { snprintf(msg, n, "No water within reach"); return 0; }
        if (h->stomach_water > 1.5f) { snprintf(msg, n, "Too full to drink"); return 0; }
        h->stomach_water += 0.25f;
        snprintf(msg, n, "Drank 250 mL of water");
        return 1;
    default:
        snprintf(msg, n, "?");
        return 0;
    }
}

void health_scavenge(health *h, uint8_t block)
{
    survival_items *it = &h->items;
    if (block == B_LEAVES) {
        if (frand(h) < 0.5f) it->fibre++;
        if (frand(h) < 1.0f / 6.0f) {
            it->apples++;
            health_log(h, "Found an apple");
        }
    } else if (block == B_LOG) {
        it->sticks += 2;
    } else if (block == B_PLANKS) {
        if (frand(h) < 0.5f) it->sticks++;
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
            h->beat_phase += (float)(ds * h->hr / 60.0);
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
                float p = 0.004f + clampf_((h->ischemia - 0.5f) * 0.1f, 0.0f, 0.4f) +
                          (h->lactate > 10.0f ? 0.05f : 0.0f) + (h->organ[ORG_KIDNEYS] < 0.15f ? 0.06f : 0.0f) +
                          (h->temp < 32.0f ? 0.05f : 0.0f);
                if (!pvc && frand(h) < p) h->pvc_pending = 1;
            }
            h->next_r = ts + (1.0f - h->beat_phase) * rr;
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
               (frand(h) - 0.5f) * (0.02f + 0.03f * (float)clampf_((float)e->speed, 0.0f, 8.0f));

        int pulse = h->rhythm == RHYTHM_SINUS && !h->dead;
        if (pulse) {
            float t = (float)(ts - h->last_r) - 0.06f;
            float beat_rr = (float)(h->last_r - h->prev_r);
            if (t < 0.0f) t += beat_rr;
            art = h->beat_dbp + h->beat_pp * pulse_wave(t, maxf(rr, beat_rr), 0.09f);
            float tp = (float)(ts - h->last_r) - 0.22f;
            if (tp < 0.0f) tp += beat_rr;
            /* Vasoconstriction flattens the finger's pulse. */
            float pi = clampf_(h->beat_pp / 40.0f, 0.0f, 1.5f) * (1.1f - 0.7f * h->symp);
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
            h->breath_phase += (float)(ds * h->rr / 60.0);
            if (h->breath_phase >= 1.0f) h->breath_phase -= 1.0f;
        }
    }
}

/* ---------------------------------------------------------------- step */

static int arrest_cause(const health *h, int airway)
{
    if (airway == AIRWAY_WATER && h->sao2 < 0.7f) return DEATH_DROWNING;
    if (airway == AIRWAY_BLOCKED && h->sao2 < 0.7f) return DEATH_SUFFOCATION;
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
    if (h->temp < 36.5f) p += minf(400.0f, 250.0f * (36.5f - h->temp)); /* shivering */
    float cap = cao2_rel * h->organ[ORG_HEART] * clampf_(h->blood / BLOOD_NORMAL, 0.0f, 1.0f) *
                (h->glycogen > 50.0f ? 1.0f : 0.7f) * (1.0f - clampf_(dehyd * 3.0f, 0.0f, 0.4f));
    cap = clampf_(cap, 0.05f, 1.0f);
    float cp = CRIT_POWER * cap, aer_max = VO2MAX_W * cap;
    if (p > cp) {
        h->wbal -= (p - cp) * fdt;
        h->lactate += (p - cp) / 5000.0f * fdt;
    } else {
        float tau = 316.0f + 546.0f * expf(-0.0025f * (cp - p));
        h->wbal = lag(h->wbal, WPRIME, dt, tau);
    }
    h->wbal = clampf_(h->wbal, 0.0f, WPRIME);
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
    h->lactate = clampf_(h->lactate, 0.5f, 30.0f);

    /* ---- core temperature */
    float fever_set = 37.0f + 2.5f * h->sepsis;
    float ctrl = clampf_(1000.0f * (fever_set - h->temp), -900.0f, 60.0f); /* sweating .. vasoconstriction */
    float immersion = 25.0f * (float)e->submerged * (h->temp - (float)e->water_temp);
    h->temp += (0.8f * p - 68.0f - immersion + ctrl) / HEAT_CAP * fdt;
    /* Fever: chills and shut-down skin raise the set point within hours,
     * which on the survival clock is minutes. */
    if (fever_set > h->temp + 0.05f) h->temp += (fever_set - h->temp) * minf(1.0f, gh / 1.5f);
    float sweat = maxf(0.0f, -ctrl) / 2.43e6f * fdt;

    /* ---- water */
    float absorb = minf(h->stomach_water, 1.2f * gh * h->organ[ORG_GUT]);
    h->stomach_water -= absorb;
    h->water += absorb - sweat - 0.104f * gh * (1.0f + 0.1f * maxf(0.0f, h->temp - 37.0f));
    if (h->water > WATER_NORMAL + 0.5f) h->water -= (h->water - WATER_NORMAL) * 0.5f * gh * h->organ[ORG_KIDNEYS];

    /* ---- energy */
    float kcal_basal = BMR_W * 3600.0f / 4184.0f * gh;
    float kcal_ex = maxf(0.0f, p - BMR_W) * fdt / 4184.0f;
    float carb = 0.4f * kcal_basal + (0.5f + 0.45f * clampf_(p / CRIT_POWER, 0.0f, 1.0f)) * kcal_ex;
    float digest = minf(h->stomach_kcal, 250.0f * gh * h->organ[ORG_GUT]);
    h->stomach_kcal -= digest;
    h->glycogen += digest - carb;
    h->fat -= kcal_basal + kcal_ex - carb;
    if (h->glycogen > GLYCOGEN_MAX) { h->fat += h->glycogen - GLYCOGEN_MAX; h->glycogen = GLYCOGEN_MAX; }
    if (h->glycogen < 0.0f) { h->fat += h->glycogen; h->glycogen = 0.0f; }

    /* ---- bleeding */
    float coag = sqrtf(h->organ[ORG_LIVER]) * (h->temp < 35.0f ? clampf_(1.0f - (35.0f - h->temp) * 0.15f, 0.3f, 1.0f) : 1.0f) *
                 clampf_(hct(h) / 0.3f, 0.3f, 1.0f) * (1.0f - 0.5f * h->sepsis);
    float pressure = clampf_(h->map / MAP_SET, 0.0f, 1.5f);
    float ext = 0.0f;
    for (int i = 0; i < h->wound_count; i++) {
        wound *w = &h->wounds[i];
        float bf = 1.0f, tau;
        if (w->bandage == BANDAGE_STERILE) bf = w->arterial ? 0.35f : 0.08f;
        else if (w->bandage == BANDAGE_IMPROVISED) bf = w->arterial ? 0.5f : 0.15f;
        if (w->arterial) tau = w->bandage ? 600.0f : 1e9f;
        else tau = (60.0f + 400.0f * w->depth) / maxf(coag, 0.05f) / (w->bandage ? 2.0f : 1.0f);
        w->clot += (1.0f - w->clot) * (float)(1.0 - exp(-dt / tau));
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
        bp->internal *= (float)exp(-dt * coag / (300.0 + 3.0 * bp->internal));
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

    /* ---- circulation */
    float veff = h->blood * (1.0f - 0.6f * dehyd);
    /* Exercise drive: central command reacts at once, the metabolic part
     * follows oxygen uptake. */
    float ex = clampf_(0.35f * minf(p / VO2MAX_W, 1.0f) + 0.65f * vo2_frac, 0.0f, 1.0f);
    float b = clampf_((MAP_SET + 30.0f * ex - h->map) / 30.0f, -1.0f, 1.0f); /* baroreflex resets upward in exercise */
    float chemo = clampf_((0.92f - h->sao2) / 0.3f, 0.0f, 1.0f) + 0.6f * clampf_((h->paco2 - 45.0f) / 25.0f, 0.0f, 1.0f);
    int diving = e->airway == AIRWAY_WATER;
    h->icp = 10.0f + 0.8f * h->ich + 0.012f * h->ich * h->ich;
    int cushing = h->icp > 30.0f;

    float symp_t = 0.15f + 1.0f * maxf(b, 0.0f) + 0.4f * chemo + 0.03f * h->pain + (diving ? 0.2f : 0.0f) +
                   (cushing ? 0.5f : 0.0f) - 0.45f * maxf(-b, 0.0f);
    if (h->organ[ORG_BRAIN] < 0.15f) symp_t = 0.05f; /* autonomic failure */
    h->symp = lag(h->symp, clampf_(symp_t, 0.0f, 1.0f), dt, 4.0);

    float hr_max = HR_MAX * (0.55f + 0.45f * h->organ[ORG_HEART]);
    /* Chemoreceptors speed the heart only while the lungs inflate; in
     * apnoea the same reflex slows it (the diving response). */
    float drive = 0.95f * ex + 0.85f * maxf(b, 0.0f) + (h->breathing ? 0.5f : -0.2f) * chemo + 0.025f * h->pain +
                  0.07f * maxf(0.0f, h->temp - 37.0f) + 0.25f * h->sepsis - 0.4f * maxf(-b, 0.0f) +
                  (h->lactate > 4.0f ? 0.02f * (h->lactate - 4.0f) : 0.0f);
    float hr_rest = HR_REST * (h->temp < 35.0f ? clampf_(1.0f - (35.0f - h->temp) * 0.08f, 0.3f, 1.0f) : 1.0f);
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
        float diast = clampf_(1.2f - h->hr / 300.0f, 0.3f, 1.0f) / 0.987f;
        float coronary = clampf_((h->dbp - 5.0f) / 73.0f, 0.0f, 2.0f) * cao2_rel * diast;
        float supply = 6.0f * coronary * sqrtf(h->organ[ORG_HEART]);
        float ratio = demand / maxf(supply, 0.01f);
        if (ratio > 1.0f) h->ischemia += (ratio - 1.0f) * 0.08f * fdt;
        else h->ischemia = maxf(0.0f, h->ischemia - 0.02f * fdt);
        /* Profoundly hypoxic blood starves the myocardium however slowly
         * it beats: the path from drowning to arrest. */
        if (h->sao2 < 0.3f) h->ischemia += (0.3f - h->sao2) / 0.3f * 0.05f * fdt;
        h->ischemia = minf(h->ischemia, 10.0f);
        if (h->ischemia > 1.5f) h->organ[ORG_HEART] = maxf(0.0f, h->organ[ORG_HEART] - 0.0015f * (h->ischemia - 1.5f) * fdt);
        float vf = (h->ischemia > 3.0f ? 0.1f * (h->ischemia - 3.0f) : 0.0f) + (h->temp < 28.0f ? 0.05f : 0.0f) +
                   (h->organ[ORG_KIDNEYS] < 0.05f ? 0.01f : 0.0f);
        if (frand(h) < vf * fdt) stop_heart(h, RHYTHM_VF, e->airway);
        else if (h->hr < 20.0f || (h->map < 18.0f && h->co < 0.3f)) stop_heart(h, RHYTHM_ASYSTOLE, e->airway);
    } else {
        h->arrest_time += fdt;
        h->hr = 0.0f;
        if (h->rhythm == RHYTHM_VF && h->arrest_time > 300.0f) h->rhythm = RHYTHM_ASYSTOLE;
    }

    /* ---- breathing */
    float lung_eff = h->organ[ORG_LUNGS] * (1.0f - h->pneumothorax) * (1.0f - clampf_(h->lung_water / 1.5f, 0.0f, 0.9f));
    int drive_ok = h->organ[ORG_BRAIN] > 0.08f && !(arrest && h->arrest_time > 20.0f);
    int can_air = e->airway == AIRWAY_AIR;
    /* Tissues extract less as the store runs dry and the brain shuts down. */
    h->o2_store = maxf(0.0f, h->o2_store - h->vo2 * clampf_(h->pao2 / 25.0f, 0.3f, 1.0f) / 60.0f * fdt);
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
        h->rr = lag(h->rr, clampf_(rr_t, 4.0f, 55.0f), dt, 4.0);
        float pao2_target = 0.21f * 713.0f - h->paco2 / 0.8f;
        float store_t = O2_VOL * pao2_target / 713.0f;
        h->o2_store = lag(h->o2_store, store_t, dt, 6.0 * 14.0 / maxf(h->rr, 4.0f));
        float alveolar = maxf(h->rr - 0.6f * minf(shallow, maxf(h->rr - rr_need, 0.0f)), 4.0f);
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
    h->paco2 = clampf_(h->paco2, 15.0f, 150.0f);
    h->pao2 = h->o2_store / O2_VOL * 713.0f;
    float shunt = 0.02f + 0.6f * (1.0f - lung_eff);
    float delivery = h->co * 13.4f * hb * h->sao2;                /* mL O2/min */
    float svo2 = delivery > 1.0f ? clampf_(h->sao2 * (1.0f - h->vo2 * 1000.0f / delivery), 0.05f, 0.95f) : 0.1f;
    h->sao2 = clampf_((1.0f - shunt) * severinghaus(h->pao2) + shunt * svo2, 0.0f, 1.0f);
    h->spo2_shown = lag(h->spo2_shown, h->sao2, dt, 5.0);
    h->etco2 = h->breathing ? maxf(0.0f, h->paco2 - 3.0f - 25.0f * maxf(0.0f, 1.0f - h->co / 4.0f)) : 0.0f;

    /* ---- brain */
    float cpp = h->map - h->icp;
    float cbf = cpp >= 55.0f ? 1.0f : maxf(0.0f, cpp / 55.0f);
    cbf *= clampf_(1.0f + 0.025f * (h->paco2 - 40.0f), 0.6f, 1.6f) * (1.0f + 0.5f * maxf(0.0f, 0.9f - h->sao2));
    cbf *= clampf_(powf(15.0f / maxf(hb, 3.0f), 0.8f), 1.0f, 1.8f); /* thinner blood flows faster */
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
    if (bo < 0.55f || h->sao2 < 0.7f || h->blood < 0.7f * BLOOD_NORMAL || h->confusion > 0.0f || h->temp < 33.0f || h->temp > 40.0f || h->sepsis > 0.7f ||
        h->organ[ORG_BRAIN] < 0.6f || dehyd > 0.1f || h->fat < 2000.0f)
        cons = CONS_CONFUSED;
    int out = h->conscious == CONS_UNCONSCIOUS; /* hysteresis */
    if (bo < (out ? 0.45f : 0.4f) || h->sao2 < (out ? 0.55f : 0.5f) || h->concussion > 0.0f || h->temp < 30.0f || h->temp > 41.5f || h->organ[ORG_BRAIN] < 0.3f ||
        h->icp > 35.0f || h->sepsis > 0.92f || arrest)
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
    for (int i = 0; i < ORG_COUNT; i++) h->organ[i] = clampf_(h->organ[i], 0.0f, 1.0f);

    /* ---- slow processes on the survival clock: infection and healing */
    float max_inf = 0.0f;
    float immune = 0.04f * (h->glycogen < 50.0f ? 0.5f : 1.0f) * (1.0f - clampf_(dehyd * 4.0f, 0.0f, 0.5f)) *
                   (1.0f - 0.5f * h->sepsis);
    float abx = h->antibiotic > 0.0f ? 0.3f : 0.0f;
    for (int i = 0; i < h->wound_count; i++) {
        wound *w = &h->wounds[i];
        w->bandage_age += gh;
        if (!w->bandage) w->contamination += 0.01f * gh;
        else if (w->bandage == BANDAGE_IMPROVISED) w->contamination += 0.004f * gh;
        else if (w->bandage_age > 24.0f) w->contamination += 0.01f * gh;
        if (e->submerged > 0.3) w->contamination += 0.2f * gh;
        w->contamination = clampf_(w->contamination, 0.0f, 1.0f);
        float seed = w->contamination > 0.1f ? w->contamination * 0.02f : 0.0f;
        float grow = 0.12f * w->infection * (1.0f - w->infection) * clampf_(w->contamination / 0.3f, 0.0f, 1.5f);
        w->infection = clampf_(w->infection + (seed + grow - (immune + abx) * w->infection) * gh, 0.0f, 1.0f);
        if (w->infection > 0.3f && w->infection - (seed + grow) * gh <= 0.3f) health_log(h, "Wound on the %s is infected", PART_NAMES_LC[w->part]);
        max_inf = maxf(max_inf, w->infection);
        if (w->bleed < 1.0f)
            w->closure += gh / (24.0f * (3.0f + 7.0f * w->depth)) * (w->infection < 0.3f ? 1.0f : 0.3f) *
                          (w->bandage ? 1.2f : 1.0f) * (w->kind == WOUND_OPEN_FRACTURE ? 0.5f : 1.0f);
        if (w->closure >= 1.0f) {
            h->wounds[i--] = h->wounds[--h->wound_count];
        }
    }
    float sepsis_t = clampf_((max_inf - 0.45f) / 0.4f, 0.0f, 1.0f);
    h->sepsis = clampf_(h->sepsis + (sepsis_t - h->sepsis) * minf(1.0f, 0.1f * gh), 0.0f, 1.0f);

    int moving = e->speed > 0.5;
    for (int i = 0; i < BP_COUNT; i++) {
        body_part *bp = &h->part[i];
        float cap_i = bp->fracture ? 0.8f : 1.0f;
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
        bp->pain = pn;
    }
    for (int i = 0; i < h->wound_count; i++) {
        wound *w = &h->wounds[i];
        h->part[w->part].pain += (1.0f + 3.0f * w->depth) * (1.0f - w->closure) + 3.0f * w->infection;
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

    /* Recovery. */
    if (h->brain_o2 > 0.8f) h->organ[ORG_BRAIN] += 0.0005f * gh;
    if (h->ischemia < 0.5f) h->organ[ORG_HEART] += 0.001f * gh;
    h->organ[ORG_LUNGS] += 0.01f * gh;
    h->lung_water = maxf(0.0f, h->lung_water - 0.04f * gh);
    if (h->sepsis < 0.3f) h->organ[ORG_LIVER] += 0.004f * gh;
    if (h->map > 65.0f && dehyd < 0.05f) h->organ[ORG_KIDNEYS] += 0.003f * gh;
    h->organ[ORG_GUT] += 0.005f * gh;
    for (int i = 0; i < ORG_COUNT; i++) h->organ[i] = clampf_(h->organ[i], 0.0f, 1.0f);
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
    for (int k = BP_LARM; k <= BP_RARM; k++) arms_ok += !(h->part[k].fracture && !h->part[k].splinted);
    l.can_act = arms_ok > 0;
    l.has_control = 1;
    return l;
}

/* ------------------------------------------------------------- display */

int health_spo2_reading(const health *h)
{
    if (h->dead || h->rhythm != RHYTHM_SINUS || h->beat_pp < 10.0f || h->map < 35.0f) return -1;
    return (int)lroundf(clampf_(h->spo2_shown, 0.0f, 1.0f) * 100.0f);
}

float health_hydration(const health *h) { return clampf_(1.0f - dehydration(h) / 0.15f, 0.0f, 1.0f); }

float health_hunger(const health *h)
{
    return clampf_(1.5f * (1.0f - h->glycogen / GLYCOGEN_MAX) - h->stomach_kcal / 800.0f, 0.0f, 1.0f);
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

int health_part_status(const health *h, int part, char *buf, size_t n)
{
    const body_part *p = &h->part[part];
    size_t len = 0;
    int sev = 0;
    buf[0] = 0;
#define SEP() cat(buf, n, &len, len ? ", " : "")
    if (p->fracture) {
        SEP();
        cat(buf, n, &len, "%s fracture%s", p->fracture == FX_OPEN ? "open" : "closed",
            p->splinted ? " (splinted)" : "");
        sev = sev > 2 ? sev : 2;
    }
    if (p->sprain > 0.05f) {
        SEP();
        cat(buf, n, &len, "sprain%s", p->splinted ? " (braced)" : "");
        sev = sev > 1 ? sev : 1;
    }
    float bleed = 0.0f, inf = 0.0f;
    int wounds = 0, bandaged = 0, arterial = 0;
    for (int i = 0; i < h->wound_count; i++) {
        const wound *w = &h->wounds[i];
        if (w->part != part) continue;
        wounds++;
        bleed += w->bleed;
        inf = maxf(inf, w->infection);
        bandaged += w->bandage != 0;
        arterial |= w->arterial && w->bleed > 20.0f;
    }
    if (wounds) {
        SEP();
        if (wounds == 1) cat(buf, n, &len, arterial ? "cut artery" : "wound");
        else cat(buf, n, &len, "%d wounds%s", wounds, arterial ? ", artery" : "");
        if (bleed >= 1.0f) cat(buf, n, &len, " bleeding %.0f mL/min", bleed);
        if (bandaged) cat(buf, n, &len, bandaged == wounds ? " (dressed)" : " (partly dressed)");
        sev = sev > 1 ? sev : 1;
        if (bleed > 30.0f || arterial) sev = 3;
    }
    if (inf > 0.1f) {
        SEP();
        cat(buf, n, &len, "%s infection", inf > 0.6f ? "severe" : (inf > 0.3f ? "spreading" : "early"));
        sev = sev > (inf > 0.6f ? 3 : 2) ? sev : (inf > 0.6f ? 3 : 2);
    }
    if (p->internal > 1.0f) {
        SEP();
        cat(buf, n, &len, "internal bleeding %.0f mL/min", p->internal);
        sev = p->internal > 20.0f ? 3 : (sev > 2 ? sev : 2);
    }
    if (p->integrity < 0.95f) {
        SEP();
        cat(buf, n, &len, "%s", p->integrity < 0.5f ? "badly bruised" : "bruised");
        if (!sev) sev = 1;
        if (p->integrity < 0.3f && sev < 2) sev = 2;
    }
    if (!len) cat(buf, n, &len, "OK");
#undef SEP
    return sev;
}

int health_organ_status(const health *h, int organ, char *buf, size_t n)
{
    float f = h->organ[organ];
    const char *s;
    int sev = f > 0.85f ? 0 : (f > 0.6f ? 1 : (f > 0.3f ? 2 : 3));
    switch (organ) {
    case ORG_BRAIN:
        if (h->icp > 25.0f) { s = "raised pressure"; sev = 3; }
        else if (h->conscious == CONS_UNCONSCIOUS) { s = "unresponsive"; sev = sev > 2 ? sev : 2; }
        else if (h->conscious == CONS_CONFUSED) { s = h->concussion > 0 || h->confusion > 0 ? "concussed" : "confused"; sev = sev > 1 ? sev : 1; }
        else s = f > 0.85f ? "normal" : "damaged";
        break;
    case ORG_HEART:
        if (h->rhythm == RHYTHM_VF) { s = "FIBRILLATING"; sev = 3; }
        else if (h->rhythm == RHYTHM_ASYSTOLE) { s = "STOPPED"; sev = 3; }
        else if (h->ischemia > 1.0f) { s = "ischaemic"; sev = sev > 2 ? sev : 2; }
        else s = f > 0.85f ? "sinus rhythm" : "weakened";
        break;
    case ORG_LUNGS:
        if (h->pneumothorax > 0.05f) { s = "partly collapsed"; sev = sev > 2 ? sev : 2; }
        else if (h->lung_water > 0.05f) { s = "water inhaled"; sev = sev > 2 ? sev : 2; }
        else if (!h->breathing) { s = "not breathing"; sev = 3; }
        else s = f > 0.85f ? "clear" : "contused";
        break;
    case ORG_KIDNEYS: s = f > 0.85f ? "normal" : (f > 0.3f ? "injured" : "failing"); break;
    case ORG_LIVER: s = f > 0.85f ? "normal" : (f > 0.3f ? "injured" : "failing"); break;
    default: s = f > 0.85f ? "normal" : (f > 0.3f ? "injured" : "failing"); break;
    }
    snprintf(buf, n, "%3.0f%%  %s", f * 100.0f, s);
    return sev;
}
