/* Health: a lumped physiological model of the player's body.
 *
 * The body is simulated, not scored. There is no hit-point bar: injuries
 * change tissues and organs, and what the player feels (heart rate, blood
 * pressure, oxygen saturation, consciousness, how fast they can move) falls
 * out of the model.
 *
 * - Circulation: blood volume drives preload (Frank-Starling), the heart
 *   pumps HR x stroke volume, and mean arterial pressure is cardiac output
 *   times vascular resistance. A baroreflex raises heart rate and
 *   constricts vessels when pressure falls, so haemorrhage walks through
 *   the textbook shock classes by itself. A myocardium starved of oxygen
 *   throws ectopic beats and can fibrillate.
 * - Respiration: an oxygen store drained by metabolism and refilled by
 *   breathing, arterial saturation from the Severinghaus dissociation
 *   curve, a pulmonary shunt for damaged or flooded lungs, CO2 retention
 *   when holding breath and an involuntary gasp (aspiration) when drowning.
 * - Metabolism: running costs about 4 J/kg/m, swimming more. A critical
 *   power model (aerobic ceiling plus a finite anaerobic reserve) gives
 *   stamina; lactate, glycogen, fat, body water and core temperature
 *   follow from the same energy budget.
 * - Injuries: bruising, sprains, closed and open fractures, cuts (venous
 *   or arterial) that clot or keep bleeding, internal bleeding, concussion
 *   and intracranial bleeding, pneumothorax, wound contamination,
 *   infection and sepsis.
 *
 * Fast processes (heartbeats, bleeding, breathing, oxygen) run in real
 * time. Slow ones (digestion, dehydration, healing, infection) run on the
 * survival clock, HEALTH_CLOCK times faster: one in-game day is 20 real
 * minutes, as in Minecraft. */
#ifndef MC_HEALTH_H
#define MC_HEALTH_H

#include <stddef.h>
#include <stdint.h>

#define HEALTH_CLOCK 72.0     /* survival-clock speed-up for slow processes */
#define HEALTH_WAVE_HZ 240    /* monitor waveform sample rate */
#define HEALTH_WAVE_LEN 1440  /* 6 s of waveform history */
#define HEALTH_MAX_WOUNDS 24
#define HEALTH_LOG 5

#define BLOOD_NORMAL 5.25f    /* L, 70 mL/kg for 75 kg */
#define WATER_NORMAL 42.0f    /* L of total body water */
#define GLYCOGEN_MAX 2000.0f  /* kcal */
#define FAT_NORMAL 90000.0f   /* kcal of mobilisable fat */

typedef enum { BP_HEAD, BP_CHEST, BP_ABDOMEN, BP_LARM, BP_RARM, BP_LLEG, BP_RLEG, BP_COUNT } body_part_id;
typedef enum { ORG_BRAIN, ORG_HEART, ORG_LUNGS, ORG_LIVER, ORG_KIDNEYS, ORG_GUT, ORG_COUNT } organ_id;
typedef enum { FX_NONE, FX_CLOSED, FX_OPEN } fracture_kind;
typedef enum { RHYTHM_SINUS, RHYTHM_VF, RHYTHM_ASYSTOLE } rhythm_kind;
typedef enum { AIRWAY_AIR, AIRWAY_WATER, AIRWAY_BLOCKED } airway_state;
typedef enum { CONS_UNCONSCIOUS, CONS_CONFUSED, CONS_ALERT } consciousness;
typedef enum { WOUND_CUT, WOUND_OPEN_FRACTURE } wound_kind;
typedef enum { BANDAGE_NONE, BANDAGE_STERILE, BANDAGE_IMPROVISED } bandage_kind;

typedef enum {
    DEATH_NONE,
    DEATH_BLOOD_LOSS,
    DEATH_DROWNING,
    DEATH_SUFFOCATION,
    DEATH_HEAD_INJURY,
    DEATH_CARDIAC,
    DEATH_SEPSIS,
    DEATH_DEHYDRATION,
    DEATH_STARVATION,
    DEATH_HYPOTHERMIA,
    DEATH_HYPERTHERMIA,
    DEATH_TRAUMA,
} death_cause;

typedef enum {
    TREAT_BANDAGE,
    TREAT_SPLINT,
    TREAT_DISINFECT,
    TREAT_PAINKILLER,
    TREAT_ANTIBIOTIC,
    TREAT_EAT,
    TREAT_DRINK,
    TREAT_COUNT
} treatment;

typedef struct {
    uint8_t part, kind, bandage, arterial;
    float depth;          /* 0..1 */
    float bleed0;         /* mL/min when fresh and uncompressed */
    float bleed;          /* mL/min right now */
    float clot;           /* 0..1 */
    float contamination;  /* 0..1 */
    float infection;      /* 0..1 */
    float closure;        /* 0..1 healed */
    float bandage_age;    /* game hours */
} wound;

typedef struct {
    float integrity;      /* 0..1, soft tissue (bruising, crush) */
    uint8_t fracture;     /* fracture_kind */
    uint8_t splinted;
    uint8_t dislocated;
    float fracture_heal;  /* 0..1 */
    float sprain;         /* 0..1 severity, heals to 0 */
    float internal;       /* internal bleeding, mL/min */
    float pain;           /* 0..10 local */
} body_part;

typedef struct {
    int bandages, fibre, splints, sticks, antiseptic, painkillers, antibiotics, apples;
} survival_items;

/* What the rest of the game tells the body each step. */
typedef struct {
    double speed;         /* horizontal speed, m/s */
    double submerged;     /* 0..1 of the body in water */
    int on_ground;
    int jumped;           /* a jump started this step */
    int actions;          /* blocks broken or placed this step */
    int airway;           /* airway_state */
    int invulnerable;     /* fly mode: vitals still run, no new injuries */
    double water_temp;    /* degrees C */
} health_env;

typedef struct health {
    body_part part[BP_COUNT];
    float organ[ORG_COUNT];           /* function 0..1 */
    wound wounds[HEALTH_MAX_WOUNDS];
    int wound_count;
    survival_items items;

    /* Fluids and energy. */
    float blood, rbc;                 /* L of whole blood and of red cells */
    float water;                      /* L total body water */
    float stomach_water, stomach_kcal;
    float glycogen, fat;              /* kcal */
    float bleed_ext, bleed_int;       /* mL/min, for display */

    /* Circulation. */
    float hr, sv, co, svr, map, sbp, dbp;
    float symp;                       /* sympathetic tone 0..1 */
    float ischemia;                   /* myocardial oxygen debt */
    int rhythm;
    float arrest_time;                /* s without circulation */
    float beat_phase;
    int pvc_pending, last_beat_pvc;
    double last_r, prev_r, next_r;    /* beat times, s */
    float beat_pp, beat_dbp;          /* pulse pressure and diastolic of the last beat */

    /* Respiration. */
    float rr, breath_phase;
    int breathing;
    float o2_store;                   /* L of O2 in lungs + blood reserve */
    float pao2, sao2, spo2_shown, paco2, etco2;
    float lung_water;                 /* L aspirated */
    float pneumothorax;               /* 0..1 lung collapsed */
    float apnea_time, gasp_timer;

    /* Metabolism and temperature. */
    float power;                      /* W metabolic */
    float vo2;                        /* L/min */
    float wbal;                       /* J of anaerobic reserve */
    float lactate;                    /* mmol/L */
    float shock_debt;                 /* minutes of starved tissue: past ~3, shock turns irreversible */
    float temp;                       /* core, degrees C */

    /* Nervous system. */
    float brain_o2;                   /* 0..1 of normal cerebral oxygen delivery */
    float ich;                        /* mL intracranial blood */
    float ich_rate;                   /* mL/min */
    float icp;                        /* mmHg */
    float concussion;                 /* s of unconsciousness left */
    float confusion;                  /* s of confusion left */
    float pain;                       /* 0..10 perceived */
    float painkiller, antibiotic;     /* game hours of effect left */
    float sepsis;                     /* 0..1 */
    int conscious;                    /* consciousness */

    int dead, cause;
    float hurt_flash;                 /* 0..1, for the screen */
    double t;                         /* s since spawn */
    uint32_t rng;

    /* Monitor traces, ring buffers written at HEALTH_WAVE_HZ. */
    float ecg[HEALTH_WAVE_LEN];       /* mV */
    float art[HEALTH_WAVE_LEN];       /* mmHg */
    float pleth[HEALTH_WAVE_LEN];     /* 0..1 */
    float capno[HEALTH_WAVE_LEN];     /* mmHg */
    int wave_pos;
    float wave_acc;

    /* Recent events for the HUD. */
    char log[HEALTH_LOG][72];
    float log_age[HEALTH_LOG];
} health;

void health_init(health *h, uint32_t seed);

/* Advances the body by dt seconds of real time. */
void health_step(health *h, const health_env *e, double dt);

/* Scales and filters movement input by what the body can do. */
typedef struct {
    float move_scale;     /* multiply requested speed */
    int can_jump, can_sprint, can_act, has_control;
} health_limits;
health_limits health_get_limits(const health *h);

/* ----------------------------------------------------------- injuries */

/* Feet-first landing: dv is the speed lost in the impact, cushion scales
 * it for soft ground (leaves 0.35 .. stone 1). */
void health_fall(health *h, double dv, double cushion);

/* Blunt hit on one part (sideways collision, thrown object). dv in m/s of
 * velocity change of that part. */
void health_blunt(health *h, int part, double dv);

/* A mass (kg) moving at speed (m/s) relative to the player hits it at a
 * height 0 (feet) .. 1 (top of head). softness 0..1 lowers the energy
 * delivered (leaves are mostly air). */
void health_struck(health *h, double mass, double speed, double height, double softness);

/* A cut. severity 0..1; arterial cuts do not clot on their own. */
void health_cut(health *h, int part, float severity, int arterial, float contamination);

/* Breaks the main bone of a part outright (open: through the skin). */
void health_break_bone(health *h, int part, int open);

/* ------------------------------------------------------- treatments */

/* Applies a treatment to a part (ignored for systemic ones). Uses up
 * supplies. Returns 1 if something was done; msg explains either way. */
int health_treat(health *h, int part, int what, int water_nearby, char *msg, size_t msg_size);

/* Adds scavenged items (breaking leaves and wood). */
void health_scavenge(health *h, uint8_t id);

/* ---------------------------------------------------------- display */

const char *health_part_name(int part);
const char *health_bone_name(int part); /* "right forearm", "skull" */
const char *health_organ_name(int organ);
const char *health_death_text(int cause);

/* One-line status for a part, e.g. "Closed fracture (splinted), bleeding
 * 12 mL/min". Returns a severity 0 (fine) .. 3 (critical). */
int health_part_status(const health *h, int part, char *buf, size_t n);
int health_organ_status(const health *h, int organ, char *buf, size_t n);

/* Monitor readouts; spo2 is -1 when the oximeter has no pulse signal. */
int health_spo2_reading(const health *h);
float health_hydration(const health *h);  /* 0..1, 1 = fully hydrated */
float health_hunger(const health *h);     /* 0..1, 1 = starving */
float health_stamina(const health *h);    /* 0..1 anaerobic reserve */
float health_hb(const health *h);         /* g/dL */

void health_log(health *h, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

#endif
