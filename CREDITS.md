# Credits

The people whose work blockclonia builds on. The libraries it uses are
listed under [Dependencies](BUILDING.md#dependencies) in BUILDING.md, with
their pinned versions in `cmake/Dependencies.cmake`.

## Research

Papers and standards whose methods the game implements. Each entry gives
the authors, title, venue, year and a link, what was taken from it, and
which file uses it. The code that implements a method carries a short
comment pointing here. An entry is added in the pull request that
implements the method, not for work that was only read.

### Health: the circulation and its control

- Thomas Heldt, Eun B. Shim, Roger D. Kamm and Roger G. Mark,
  "Computational modeling of cardiovascular response to orthostatic
  stress", *Journal of Applied Physiology* 92(3):1239–1254, 2002.
  <https://doi.org/10.1152/japplphysiol.00241.2001>. The lumped
  compartments, their resistances and compliances, the veins that stiffen
  as they fill (arctangent law) and the hydrostatic columns of a standing
  body. `src/health/circulation.c`.
- Nikos Stergiopulos, Jean-Jacques Meister and Nico Westerhof,
  "Determinants of stroke volume and systolic and diastolic aortic
  pressure", *American Journal of Physiology* 270(6):H2050–H2059, 1996.
  <https://doi.org/10.1152/ajpheart.1996.270.6.H2050>. The double Hill
  curve for the chambers' time-varying elastance.
  `src/health/circulation.c`.
- Mauro Ursino, "Interaction between carotid baroregulation and the
  pulsating heart: a mathematical model", *American Journal of
  Physiology* 275(5):H1733–H1747, 1998.
  <https://doi.org/10.1152/ajpheart.1998.275.5.H1733>. The carotid
  afferent, the sympathetic and vagal curves, and each effector's gain,
  delay and time constant (Tables 1 and 2), including the splanchnic
  veins' larger share. `src/health/autonomic.c`, `src/health/circulation.c`.
- Elisa Magosso and Mauro Ursino, "Cardiovascular response to dynamic
  aerobic exercise: a mathematical model", *Medical & Biological
  Engineering & Computing* 40(6):660–674, 2002.
  <https://doi.org/10.1007/BF02345305>. Central command and the muscles'
  metabolic dilation during exercise. `src/health/autonomic.c`.
- Rune Aaslid, Karl-Fredrik Lindegaard, Wenche Sorteberg and Helge
  Nornes, "Cerebral autoregulation dynamics in humans", *Stroke*
  20(1):45–52, 1989. <https://doi.org/10.1161/01.STR.20.1.45>. The
  brain's arterioles restoring its flow within about 5 s of a pressure
  step. `src/health/autonomic.c`.
- William E. Clutter, Dennis M. Bier, Suresh D. Shah and Philip E. Cryer,
  "Epinephrine plasma metabolic clearance rates and physiologic
  thresholds for metabolic and hemodynamic actions in man", *Journal of
  Clinical Investigation* 66(1):94–101, 1980.
  <https://doi.org/10.1172/JCI109840>. Adrenaline's plasma levels and
  their thresholds: heart rate from ~0.5 ng/mL, vasoconstriction only at
  pressor levels. `src/health/health.c`, `tests/test_body_risks.c`.
- Julie C. Schadt and John Ludbrook, "Hemodynamic and neurohumoral
  responses to acute hypovolemia in conscious mammals", *American Journal
  of Physiology* 260(2):H305–H318, 1991.
  <https://doi.org/10.1152/ajpheart.1991.260.2.H305>. Vasopressin and
  angiotensin II released as the blood volume falls, holding the vessels'
  tone through the later stage of a bleed. `src/health/health.c`.
- Jouko Karjalainen and Matti Viitasalo, "Fever and cardiac rhythm",
  *Archives of Internal Medicine* 146(6):1169–1171, 1986.
  <https://doi.org/10.1001/archinte.1986.00360180179026>. About 8.5 bpm
  of heart rate per degree of fever. `src/health/health.c`.
- Mary P. Larsen, Mickey S. Eisenberg, Richard O. Cummins and Alfred P.
  Hallstrom, "Predicting survival from out-of-hospital cardiac arrest: a
  graphic model", *Annals of Emergency Medicine* 22(11):1652–1658, 1993.
  <https://doi.org/10.1016/S0196-0644(05)81302-2>. Survival falling by
  minutes to CPR and to defibrillation. `src/health/health.c`.
- Howard R. Guly, Omar Bouamra, Mark Spiers, Paul Dark, Tim Coats and
  Fiona E. Lecky, "Vital signs and estimated blood loss in patients with
  major trauma: testing the validity of the ATLS classification of
  hypovolaemic shock", *Resuscitation* 82(5):556–559, 2011.
  <https://doi.org/10.1016/j.resuscitation.2011.01.013>. The pulse rising
  more slowly with blood loss than the teaching classes say, which the
  haemorrhage calibration follows. `tests/test_circulation.c`.

The sound overhaul adds the next entries
([docs/design/sound.md](docs/design/sound.md)).
