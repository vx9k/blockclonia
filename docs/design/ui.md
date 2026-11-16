# User interface

The "field kit" redesign of every screen.

What is done and what is next lives in [TODO.md](../../TODO.md); this page
holds the reasoning and the detail.

**Status:** designed, not started. Every screen today is the same
thin-outlined black box with a green accent, and the health panel shows
everything at once. The new look, "field kit", was chosen over a light
"trail signage" alternative on 2026-11-16. Mock-ups of every screen are
on the design canvas (private to the maintainer).

**The look.**

- *Plates:* panels are olive enamel plates: a flat fill, a 3 px light
  bevel on the top and left edges, a 3 px dark bevel on the bottom and
  right, a 4 px rivet 9 px in from each corner and a 4 px shadow under
  the plate. Slots and inputs are sunk: the bevels swapped. All of it is
  flat quads in the `ui` batch, no textures.
- *Palette:* plate `#353b2a`, light bevel `#4d553b`, dark bevel
  `#21251a`, sunk `#191c13`, bone text `#ece4c8`, dim text `#a7a38a`,
  disabled `#6c6f5c`. Injury colours only: minor `#e3b23c`, serious
  `#ec7a2c`, critical (to be lightened, see Accessibility). The vitals
  readout keeps the monitor colours (ECG green, arterial red, pleth
  blue) on a dark glass inset.
- *Type:* a new bitmap font in three roles: a chunky display face for
  titles and item names, a pixel body face for anything read, and a
  small capitals face for tags, counts and units. Baked from C tables
  at start-up like the textures, since the project ships no image
  files.
- *Pieces:* label tape for section names (a bone strip with dark
  capitals), keycaps for every key hint, segmented sliders (ten blocks),
  two-part On/Off switches, and pixel item icons drawn from palette
  tables, as the mobs are, instead of letters.
- *Selection* inverts a plate to bone with dark text. There is no
  accent colour; yellow, orange and red mean something is wrong with
  the body and nothing else.

**Screens.**

- *Title:* the logo large over the world, the tagline on tape, a plain
  menu where only the selected entry sits on a plate.
- *HUD:* nothing but the crosshair and the belt (the hotbar) while
  healthy. When hurt, a small body figure appears top left with one
  short line per problem, worst first ("Left leg broken", "Bleeding")
  and a `Tab` keycap; each line goes once treated. The held item is
  raised and named on a tape label. The inner voice stays a subtitle.
- *Character panel* on `Tab` (`I` and `H` open it too), replacing the
  separate inventory and health screens:
  - Body: the figure with Skin, Bone, Organs and Blood layers. Hovering
    or focusing a limb shows a tag with short facts ("Shin broken",
    "Knee scraped"); no prose summaries. A vitals readout (HR, BP,
    SpO2, a trace, then blood, pain and core temperature) under it.
  - Bag (9×3) and belt (9), with counts and wear bars. Items that help
    the selected limb get an orange corner. The held item's card has
    one line of text and a "Use on left leg" button; dragging the item
    onto the body does the same.
  - Make: recipes as icon rows with ingredient icons and counts (short
    ones in red), the station they need, and a Ready/All switch.
  - When nothing is wrong the body side folds to a strip reading "All
    fine" and the bag and recipes take the room; it opens on its own
    when hurt.
- *Pause and settings:* a large "Paused" heading with a short menu;
  settings on folder tabs with segmented sliders, switches and a
  "restart" tag where a change needs one.

**Accessibility.** These are requirements, not extras. Contrast
follows WCAG 2.2 level AA, measured with the WCAG relative-luminance
formula against the colour the element actually sits on.

- *Text contrast* (WCAG 1.4.3): at least 4.5:1, or 3:1 for large text
  (24 px and up at 1080p). Measured on the palette above: bone on plate
  9.1:1, bone on sunk 13.6:1, dark text on a selected bone plate
  14.2:1, dim on plate 4.6:1. Dim on the light bevel colour (hover) is
  only 3.1:1, so hovered rows switch their text to bone.
- *Non-text contrast* (WCAG 1.4.11): slot borders, the slider fill,
  switches, focus outlines, injury marks and severity squares reach
  3:1 against what is next to them. Critical red on plate is 2.7:1 and
  fails, so lighten it (or set it on the dark sunk colour, where it is
  4.1:1). Disabled controls (2.2:1) are exempt but must also look
  disabled by shape, not only by being faint.
- *Text over the world* never sits on the bare scene: it goes on a
  plate or tape, or gets a 2 px dark outline, because the sky and snow
  behind it can be any brightness.
- *Not colour alone* (WCAG 1.4.1): every injury also has a word and a
  mark (the break line, the bandage, blinking for active bleeding), and
  minor, serious and critical differ in lightness as well as hue so
  they stay apart under protanopia, deuteranopia and tritanopia. Check
  the figure and alerts through a colour-blindness simulation before
  merging.
- *A palette test:* `tests/test_visual.c` computes the ratios for every
  text and mark pair the UI uses and fails below the limits above, so a
  later colour change cannot quietly break them.
- *Size:* a UI scale setting (75–200 %) apart from the window size; no
  body text under 16 px at 1080p; click targets at least 24×24 px
  (WCAG 2.5.8; slots are 56 px). Check the Raspberry Pi's 720p.
- *Focus and input:* every control can be reached and used with the
  keyboard (and later a gamepad), with a visible bone focus outline
  (WCAG 2.4.7). Dragging always has a non-drag way: pick the item, pick
  the limb, press Enter (WCAG 2.5.7). No hover-only information: what a
  limb tag shows on hover also shows on focus.
- *Motion and flashing:* nothing flashes more than three times a second
  (WCAG 2.3.1; the bleeding blink is 1 Hz). A "reduce motion" setting
  turns off blinking, the red screen edge, the wavy and glitching voice
  text and the view sway, and leaves the information in place.
- *Time:* no message disappears before the player could read it;
  injury alerts stay until the injury is treated, and the voice line
  stays on screen for at least 1 s plus 60 ms per character.
- *Sound cues:* important sounds with no picture (a wolf growling out
  of view, a heartbeat in arrhythmia) get optional captions.
- *Words:* short and plain, verbs on buttons, units on every number.

**Steps.** One pull request each:

1. The font and the plate, tape, keycap, slot, slider and switch
   helpers, the palette test, and the UI scale setting; title, pause and
   settings in the new look.
2. The HUD: the belt, the injury alerts, the reduce-motion setting.
3. Pixel item icons for every item and block.
4. The character panel: bag, belt and making on one screen on `Tab`.
5. The body side: the figure with hover and focus tags, treatment by
   item and by dragging, the vitals readout.
6. The Bone, Organs and Blood layers.
