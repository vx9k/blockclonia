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
 * - Injuries: bruising, sprains, closed and open fractures, dislocated
 *   shoulders and elbows, cuts (venous or arterial) that clot or keep
 *   bleeding, abrasions, internal bleeding, concussion and intracranial
 *   bleeding, pneumothorax, crush injury and crush syndrome, wound
 *   contamination, infection and sepsis.
 * - Resuscitation: an automated defibrillator's pads on the chest watch
 *   the rhythm and shock ventricular fibrillation by themselves; the
 *   chance that a shock brings a heartbeat back falls with every minute
 *   of arrest. Nothing restarts asystole.
 * - Heat and cold: a skin temperature per body part between the core and
 *   the surroundings (convection, radiation, clothing that stops
 *   insulating when wet, evaporation, the ground under the feet), with
 *   vasomotor control; frostbite where tissue freezes and burns from the
 *   Henriques-Moritz damage integral where it overheats.
 *
 * Fast processes (heartbeats, bleeding, breathing, oxygen) run in real
 * time. Slow ones (digestion, dehydration, healing, infection) run on the
 * survival clock, HEALTH_CLOCK times faster: one in-game day is 20 real
 * minutes, as in Minecraft. */
#ifndef MC_HEALTH_H
#define MC_HEALTH_H

#include "inventory.h"
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
typedef enum { WOUND_CUT, WOUND_OPEN_FRACTURE, WOUND_ABRASION, WOUND_BURN, WOUND_FROSTBITE } wound_kind;
typedef enum { DISLOC_NONE, DISLOC_SHOULDER, DISLOC_ELBOW } dislocation_kind;
typedef enum { FROST_NONE, FROST_NIP, FROST_SUPERFICIAL, FROST_DEEP } frostbite_stage;
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
    DEATH_BURNS,
} death_cause;

typedef enum {
    TREAT_BANDAGE,
    TREAT_SPLINT,
    TREAT_DISINFECT,
    TREAT_PAINKILLER,
    TREAT_ANTIBIOTIC,
    TREAT_EAT,
    TREAT_DRINK,
    TREAT_COOL,           /* cool a burn under water (needs water nearby) */
    TREAT_REDUCE,         /* pop a dislocated joint back in */
    TREAT_DEFIB,          /* defibrillator pads on the chest, or off again */
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
    float area;           /* 0..1 of the part (abrasions, burns) */
} wound;

typedef struct {
    float integrity;      /* 0..1, soft tissue (bruising, crush) */
    uint8_t fracture;     /* fracture_kind */
    uint8_t splinted;
    uint8_t dislocated;   /* dislocation_kind */
    float fracture_heal;  /* 0..1 */
    float sprain;         /* 0..1 severity, heals to 0 */
    float internal;       /* internal bleeding, mL/min */
    float pain;           /* 0..10 local */
    float pain_spike;     /* 0..10, decays: a joint going out or being put back */

    /* Heat and cold. For the limbs the shell is the hand or the foot,
     * where cold bites first. */
    float skin;           /* degrees C, shell (skin and the tissue under it) */
    float surface;        /* K above the shell at the basal layer: the fast burn layer */
    float burn_omega;     /* Henriques-Moritz damage integral at the basal layer */
    float burn_area;      /* 0..1 of the part that took the heat */
    float burn_age;       /* game hours since the burn */
    float cool_time;      /* s of water cooling left (TREAT_COOL) */
    uint8_t burn;         /* degree 0..3 */
    uint8_t burn_cooled;  /* cooled in time: the burn stops deepening */
    uint8_t frost;        /* frostbite_stage */
    float freeze_time;    /* s of tissue freezing (weighted by how cold) */
    float frost_heal;     /* 0..1 */

    /* Crush: a weight resting on the part. */
    float crush;          /* 0..1 of the part's muscle necrotic */
    float crush_released; /* part of `crush` already reperfused */
    float crush_load;     /* kg on it during the last step */
    float crush_next;     /* kg reported for the coming step (health_crush) */
} body_part;

/* An automated external defibrillator with its pads on the chest. */
typedef enum { DEFIB_OFF, DEFIB_MONITOR, DEFIB_ANALYSE, DEFIB_CHARGE, DEFIB_CLEAR } defib_phase;

typedef struct {
    int phase;            /* defib_phase; DEFIB_OFF while the pads are off */
    int seen;             /* rhythm found by the last analysis, -1 before one */
    int advice;           /* last analysis: 1 shock advised, 0 no shock advised */
    int shocks;           /* delivered since the pads went on */
    float timer;          /* s left in the phase */
    float joules;         /* charge stored for the next shock */
    float since_shock;    /* s since the last shock */
} defib_state;

/* What the rest of the game tells the body each step. */
/* Individual constitution, drawn once per body (health_init_varied) from
 * the seed, within a healthy adult's normal range; health_init leaves it
 * at the population mean instead, so the reference body tests check exact
 * numbers against never changes. `vary` also turns on the small
 * beat-to-beat and breath-to-breath noise health_step adds when set. */
typedef struct {
    float hr_rest;    /* bpm, resting heart rate: 55..85, mean 64 */
    float map_set;    /* mmHg, mean arterial pressure set-point: 85..101, mean 93 */
    float pain_gain;  /* x perceived pain: 0.8..1.2 */
    float clot_gain;  /* x clotting speed: 0.75..1.35 */
    float metab_gain; /* x BMR, VO2max and critical power: 0.92..1.1 */
    float cold_bias;  /* degrees C added to the shiver threshold: -0.4..0.4 */
    int vary;
} health_baseline;

typedef struct {
    double speed;         /* horizontal speed, m/s */
    double submerged;     /* 0..1 of the body in water */
    int on_ground;
    int jumped;           /* a jump started this step */
    int actions;          /* blocks broken or placed this step */
    int airway;           /* airway_state */
    int invulnerable;     /* fly mode: vitals still run, no new injuries */
    double water_temp;    /* degrees C */
    /* The thermal surroundings (thermo.c). */
    double air_temp;      /* degrees C of the air around the body */
    double radiant;       /* W/m^2 of radiant heat reaching the skin from fires */
    double contact_temp;  /* degrees C of the surface under the feet */
    int in_fire;          /* the body is standing in flames */
    double wind;          /* m/s of air moving past besides the body's own speed */
} health_env;

typedef struct health {
    body_part part[BP_COUNT];
    float organ[ORG_COUNT];           /* function 0..1 */
    wound wounds[HEALTH_MAX_WOUNDS];
    int wound_count;

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
    defib_state defib;

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
    float vaso; /* 0 vasoconstricted .. 1 dilated skin */
    float sweat_w; /* W of sweat being evaporated */
    float sweat_acc; /* L of sweat not yet taken from body water */
    float wet; /* 0..1 clothing soaked */
    float heat_loss; /* W from the core to the skin and water, for display */
    float burn_tbsa; /* % of body surface with 2nd/3rd degree burns */
    /* The surroundings the body felt on its last step (HUD). */
    float air_temp, radiant; /* degrees C, W/m^2 */
    int in_fire;

    /* Crush syndrome. */
    float reperfused; /* kg of dead muscle whose contents are washing out */
    float myoglobin; /* g in the plasma */
    float potassium; /* mmol/L in the plasma */

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

    health_baseline base; /* this body's constitution */
    uint32_t vrng; /* short-term variability, its own stream so it never shifts an injury */
    float hr_noise, map_noise, rr_noise, spo2_noise; /* wandering state for `base.vary` */

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

/* A healthy, rested and fed adult, at the population mean (HR_REST,
 * MAP_SET, ... in health.c) with no short-term variability: the reference
 * body the tests check exact numbers against. seed drives the model's own
 * random numbers (injuries, arrhythmia), so a run with the same inputs
 * replays exactly. */
void health_init(health *h, uint32_t seed);

/* As health_init, but also draws this body's constitution (health_baseline)
 * from seed and turns on short-term variability, both reproducible from
 * seed alone. What the game spawns and respawns players with. */
void health_init_varied(health *h, uint32_t seed);

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

/* Dislocates the shoulder of an arm (arms only; ignored for a broken or
 * already dislocated one). */
void health_dislocate(health *h, int part);

/* Road rash: skin scraped off area_fraction (0..1) of a part, with grit
 * ground in (contamination 0..1). */
void health_abrasion(health *h, int part, float area_fraction, float contamination);

/* A weight of mass_kg resting on a part for dt seconds. Call it every step
 * the weight is there: muscle under pressure dies with time, a weight on
 * the chest stops breathing, and releasing a crushed limb washes
 * myoglobin and potassium into the blood (crush syndrome). */
void health_crush(health *h, int part, double mass_kg, double dt);

/* A burn by its Henriques-Moritz damage integral omega (0.53 first
 * degree, 1 second, 10^4 third), over the half of the part facing the
 * heat unless the part already has a larger burn. */
void health_burn(health *h, int part, double omega);

/* Stops the heart in the given rhythm (RHYTHM_VF or RHYTHM_ASYSTOLE),
 * as if it had happened on its own; exposed for the debug menu and for
 * items (a taser, a lightning strike) that need to cause an arrest
 * directly instead of waiting for ischemia to. */
void health_arrest(health *h, int rhythm);

/* -------------------------------------------------- debug injuries */

/* One entry per condition the model can be put into directly: --hurt and
 * the in-game debug menu (F3, K) both drive health_injure from this same
 * table, so the two stay in sync. part_kind says what `part` in
 * health_injure means for that entry: HDBG_ANY lets the caller choose any
 * body part, HDBG_ARM and HDBG_HEAD restrict or force it the way the
 * underlying injury does (a dislocated shoulder, a concussion), and
 * HDBG_SYSTEMIC ignores it (whole-body conditions: temperature, shock,
 * sepsis, cardiac rhythm). default_part is used when the caller doesn't
 * choose one (--hurt never does), or when it chooses one HDBG_ARM /
 * HDBG_HEAD can't use. */
typedef enum { HDBG_ANY, HDBG_ARM, HDBG_HEAD, HDBG_SYSTEMIC } health_debug_part_kind;

typedef struct {
    const char *name;                    /* --hurt token, e.g. "bleed" */
    const char *label;                   /* shown in the debug menu */
    health_debug_part_kind part_kind;
    uint8_t default_part;                /* body_part_id */
} health_debug_kind;

extern const health_debug_kind HEALTH_DEBUG_KINDS[];
#define HEALTH_DEBUG_COUNT 22

/* Looks a --hurt token up in HEALTH_DEBUG_KINDS; returns -1 if there is
 * no such name. */
int health_debug_find(const char *name);

/* Applies HEALTH_DEBUG_KINDS[kind] to the body. part chooses the body
 * part where part_kind is HDBG_ANY; it is ignored (or clamped to a valid
 * one) otherwise. Does nothing if kind is out of range or the body is
 * already dead. */
void health_injure(health *h, int kind, int part);

/* ------------------------------------------------------- treatments */

/* Applies a treatment to a part (ignored for systemic ones), using up
 * supplies from the inventory. Returns 1 if something was done; msg
 * explains either way. */
int health_treat(health *h, inventory *inv, int part, int what, int water_nearby, char *msg, size_t msg_size);

/* ---------------------------------------------------- defibrillation */

/* Puts a defibrillator's pads on the chest (on = 1) or takes them off.
 * With the pads on it runs by itself, like a fully automatic AED: it
 * analyses a pulseless rhythm, charges for VF and shocks after a
 * countdown, and never shocks a rhythm it did not find shockable. While
 * it can still shock VF, the arrest is not called after a minute. */
void health_pads(health *h, int on);

/* One 150 J biphasic shock through the pads. VF may stop and a beat
 * return, less likely the longer the heart has fibrillated; asystole
 * stays; a beating heart gains nothing and fibrillates if the shock lands
 * on the T wave. The skin under the pads burns a little each time. */
void health_shock(health *h);

/* ---------------------------------------------------------- display */

/* Display names (static strings). */
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
float health_skin_mean(const health *h); /* degrees C, area-weighted */
float health_part_area(int part); /* share of the body surface (rule of nines) */

/* Adds a printf-style line to the event log the HUD shows; the oldest
 * line drops off. */
void health_log(health *h, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

#endif
