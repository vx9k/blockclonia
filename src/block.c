#include "block.h"
#include "item.h"

#define ALL(t) {t, t, t, t, t, t}
#define SIDES_TOP_BOTTOM(s, t, b) {s, s, t, b, s, s}

/* Densities are real-world figures. Spans are how many blocks a material
 * can cantilever sideways before its own weight snaps it; they are a
 * stand-in for tensile strength (stone and timber strong, soil weak,
 * granular media zero). Friction coefficients are rubber-sole-on-material
 * estimates; ice is the notable outlier. Restitution is how much of an
 * impact speed a falling block keeps in the bounce; hardness is how long a
 * bare hand takes to break it; heat capacity and conductivity are handbook
 * values for the dry material (soil and snow vary a lot with water).
 * Cushion is how much of a landing's speed the ground takes out gently
 * (leaves are mostly air and springy twigs); grit is how much a surface
 * grinds into skin it scrapes (none from snow, ice, leaves or water). */
const block_def g_blocks[B_COUNT] = {
    [B_AIR] = {"air", 0, ALL(0), 0, 1.225f, 0.0f, 0.0f, 0.0f, 1005.0f, 0.025f, BM_NONE, 1.0f, 0.0f, 0, 0},
    [B_BEDROCK] = {"bedrock", BF_SOLID | BF_OPAQUE | BF_ANCHOR, ALL(T_BEDROCK), 127, 3000.0f, 0.7f, 0.25f, -1.0f,
                   800.0f, 3.0f, BM_STONE, 1.0f, 0.4f, 0, 0},
    [B_STONE] = {"stone", BF_SOLID | BF_OPAQUE, ALL(T_STONE), 7, 2600.0f, 0.7f, 0.25f, 6.0f, 790.0f, 2.5f, BM_STONE,
                 1.0f, 0.4f, 0, 0},
    [B_DIRT] = {"dirt", BF_SOLID | BF_OPAQUE, ALL(T_DIRT), 1, 1500.0f, 0.6f, 0.05f, 0.75f, 1300.0f, 1.0f, BM_DIRT,
                0.92f, 0.55f, 0, 0},
    [B_GRASS] = {"grass", BF_SOLID | BF_OPAQUE, SIDES_TOP_BOTTOM(T_GRASS_SIDE, T_GRASS_TOP, T_DIRT), 1, 1400.0f, 0.6f,
                 0.05f, 0.9f, 1400.0f, 0.8f, BM_GRASS, 0.92f, 0.55f, 0, 0},
    [B_SAND] = {"sand", BF_SOLID | BF_OPAQUE | BF_GRANULAR, ALL(T_SAND), 0, 1600.0f, 0.5f, 0.02f, 0.6f, 830.0f, 0.3f,
                BM_SAND, 0.85f, 0.6f, 0, 0},
    [B_GRAVEL] = {"gravel", BF_SOLID | BF_OPAQUE | BF_GRANULAR, ALL(T_GRAVEL), 0, 1800.0f, 0.6f, 0.1f, 0.9f, 840.0f,
                  0.7f, BM_GRAVEL, 0.85f, 0.85f, 0, 0},
    [B_LOG] = {"log", BF_SOLID | BF_OPAQUE, SIDES_TOP_BOTTOM(T_LOG_SIDE, T_LOG_TOP, T_LOG_TOP), 6, 600.0f, 0.6f, 0.3f,
               3.0f, 1700.0f, 0.15f, BM_WOOD, 1.0f, 0.3f, 0, 0},
    [B_LEAVES] = {"leaves", BF_SOLID | BF_OPAQUE, ALL(T_LEAVES), 3, 150.0f, 0.4f, 0.1f, 0.25f, 2000.0f, 0.1f, BM_LEAVES,
                  0.35f, 0.0f, 0, 0},
    [B_PLANKS] = {"planks", BF_SOLID | BF_OPAQUE, ALL(T_PLANKS), 6, 500.0f, 0.6f, 0.35f, 2.5f, 1600.0f, 0.12f, BM_WOOD,
                  1.0f, 0.3f, 0, 0},
    [B_GLASS] = {"glass", BF_SOLID | BF_TRANSLUCENT | BF_BRITTLE, ALL(T_GLASS), 2, 2500.0f, 0.4f, 0.5f, 0.4f, 840.0f,
                 1.0f, BM_GLASS, 1.0f, 0.0f, 0, 0},
    [B_BRICK] = {"brick", BF_SOLID | BF_OPAQUE, ALL(T_BRICK), 5, 1900.0f, 0.7f, 0.2f, 7.0f, 840.0f, 0.7f, BM_STONE,
                 1.0f, 0.4f, 0, 0},
    [B_SNOW] = {"snow", BF_SOLID | BF_OPAQUE, ALL(T_SNOW), 1, 400.0f, 0.3f, 0.01f, 0.25f, 2100.0f, 0.15f, BM_SNOW, 0.6f,
                0.0f, 0, 0},
    [B_ICE] = {"ice", BF_SOLID | BF_TRANSLUCENT | BF_BRITTLE, ALL(T_ICE), 3, 917.0f, 0.03f, 0.3f, 0.75f, 2090.0f, 2.2f,
               BM_ICE, 1.0f, 0.0f, 0, 0},
    [B_WATER] = {"water", BF_FLUID | BF_TRANSLUCENT, ALL(T_WATER), 0, 1000.0f, 0.0f, 0.0f, 0.0f, 4186.0f, 0.6f,
                 BM_WATER, 1.0f, 0.0f, 0, 0},
    [B_CAMPFIRE] = {"campfire",
                    BF_SOLID | BF_TRANSLUCENT | BF_BURNING,
                    {T_FIRE0, T_FIRE1, T_FIRE2, T_CAMPFIRE_BASE, T_FIRE3, T_FIRE1},
                    0,
                    500.0f,
                    0.5f,
                    0.1f,
                    0.8f,
                    1700.0f,
                    0.15f,
                    BM_WOOD,
                    1.0f,
                    0.0f,
                    0,
                    0},
    [B_ASH] = {"ash", BF_SOLID | BF_OPAQUE | BF_GRANULAR, ALL(T_ASH), 0, 600.0f, 0.6f, 0.02f, 0.3f, 800.0f, 0.1f,
               BM_DIRT, 1.0f, 0.55f, 0, 0},
    /* Ores are host rock with a share of mineral: denser and harder than
     * plain stone, more so the more metal they carry. Like rock they are
     * part of the mass around them, and never come loose as blocks. */
    [B_COAL_ORE] = {"coal ore", BF_SOLID | BF_OPAQUE, ALL(T_COAL_ORE), 15, 2200.0f, 0.7f, 0.25f, 7.0f, 900.0f, 1.5f,
                    BM_STONE, 1.0f, 0.4f, 0, 0},
    [B_COPPER_ORE] = {"copper ore", BF_SOLID | BF_OPAQUE, ALL(T_COPPER_ORE), 15, 3000.0f, 0.7f, 0.25f, 8.0f, 700.0f,
                      3.0f, BM_STONE, 1.0f, 0.4f, 0, TIER_STONE},
    [B_IRON_ORE] = {"iron ore", BF_SOLID | BF_OPAQUE, ALL(T_IRON_ORE), 15, 3400.0f, 0.7f, 0.25f, 9.0f, 650.0f, 3.5f,
                    BM_STONE, 1.0f, 0.4f, 0, TIER_STONE},
    [B_FURNACE] = {"furnace",
                   BF_SOLID | BF_OPAQUE,
                   {T_FURNACE_FRONT, T_FURNACE_FRONT, T_FURNACE_TOP, T_FURNACE_TOP, T_FURNACE_FRONT, T_FURNACE_FRONT},
                   5,
                   2300.0f,
                   0.7f,
                   0.2f,
                   7.0f,
                   840.0f,
                   1.0f,
                   BM_STONE,
                   1.0f,
                   0.4f,
                   0,
                   0},
    /* Bedrock-like rock mass spans caverns tens of metres wide; a stone
     * block set in a wall by hand (B_STONE, what breaking rock gives) is
     * only as good as its mortar. 15 is the most the support check counts. */
    [B_ROCK] = {"stone", BF_SOLID | BF_OPAQUE, ALL(T_STONE), 15, 2700.0f, 0.7f, 0.25f, 6.0f, 790.0f, 2.5f, BM_STONE,
                1.0f, 0.4f, B_STONE, 0},
    /* Each rock twice: laid by hand (as good as its mortar, and a little
     * lighter for the joints) and in place, one mass like B_ROCK that breaks
     * into the laid kind. Shale splits along its beds and sandstone's grains
     * rub loose, so both span less when laid; obsidian is a glass and
     * shatters like one. Marble and obsidian are smooth underfoot. */
    [B_GRANITE] = {"granite", BF_SOLID | BF_OPAQUE, ALL(T_GRANITE), 7, 2600.0f, 0.7f, 0.25f, 7.0f, 790.0f, 2.9f,
                   BM_STONE, 1.0f, 0.4f, 0, 0},
    [B_GRANITE_ROCK] = {"granite", BF_SOLID | BF_OPAQUE, ALL(T_GRANITE), 15, 2700.0f, 0.7f, 0.25f, 7.0f, 790.0f, 2.9f,
                        BM_STONE, 1.0f, 0.4f, B_GRANITE, 0},
    [B_BASALT] = {"basalt", BF_SOLID | BF_OPAQUE, ALL(T_BASALT), 7, 2900.0f, 0.7f, 0.25f, 7.5f, 840.0f, 1.7f, BM_STONE,
                  1.0f, 0.4f, 0, 0},
    [B_BASALT_ROCK] = {"basalt", BF_SOLID | BF_OPAQUE, ALL(T_BASALT), 15, 3000.0f, 0.7f, 0.25f, 7.5f, 840.0f, 1.7f,
                       BM_STONE, 1.0f, 0.4f, B_BASALT, 0},
    [B_LIMESTONE] = {"limestone", BF_SOLID | BF_OPAQUE, ALL(T_LIMESTONE), 6, 2400.0f, 0.65f, 0.25f, 5.0f, 910.0f, 2.3f,
                     BM_STONE, 1.0f, 0.4f, 0, 0},
    [B_LIMESTONE_ROCK] = {"limestone", BF_SOLID | BF_OPAQUE, ALL(T_LIMESTONE), 15, 2500.0f, 0.65f, 0.25f, 5.0f, 910.0f,
                          2.3f, BM_STONE, 1.0f, 0.4f, B_LIMESTONE, 0},
    [B_SANDSTONE] = {"sandstone", BF_SOLID | BF_OPAQUE, ALL(T_SANDSTONE), 4, 2200.0f, 0.75f, 0.25f, 4.0f, 920.0f, 2.5f,
                     BM_STONE, 1.0f, 0.6f, 0, 0},
    [B_SANDSTONE_ROCK] = {"sandstone", BF_SOLID | BF_OPAQUE, ALL(T_SANDSTONE), 15, 2300.0f, 0.75f, 0.25f, 4.0f, 920.0f,
                          2.5f, BM_STONE, 1.0f, 0.6f, B_SANDSTONE, 0},
    [B_SHALE] = {"shale", BF_SOLID | BF_OPAQUE, ALL(T_SHALE), 3, 2400.0f, 0.6f, 0.25f, 3.5f, 800.0f, 1.5f, BM_STONE,
                 1.0f, 0.35f, 0, 0},
    [B_SHALE_ROCK] = {"shale", BF_SOLID | BF_OPAQUE, ALL(T_SHALE), 15, 2500.0f, 0.6f, 0.25f, 3.5f, 800.0f, 1.5f,
                      BM_STONE, 1.0f, 0.35f, B_SHALE, 0},
    [B_MARBLE] = {"marble", BF_SOLID | BF_OPAQUE, ALL(T_MARBLE), 6, 2600.0f, 0.5f, 0.25f, 6.0f, 880.0f, 2.8f, BM_STONE,
                  1.0f, 0.3f, 0, 0},
    [B_MARBLE_ROCK] = {"marble", BF_SOLID | BF_OPAQUE, ALL(T_MARBLE), 15, 2700.0f, 0.5f, 0.25f, 6.0f, 880.0f, 2.8f,
                       BM_STONE, 1.0f, 0.3f, B_MARBLE, 0},
    [B_OBSIDIAN] = {"obsidian", BF_SOLID | BF_OPAQUE | BF_BRITTLE, ALL(T_OBSIDIAN), 2, 2350.0f, 0.4f, 0.25f, 5.0f,
                    840.0f, 1.3f, BM_STONE, 1.0f, 0.2f, 0, 0},
    [B_OBSIDIAN_ROCK] = {"obsidian", BF_SOLID | BF_OPAQUE, ALL(T_OBSIDIAN), 15, 2450.0f, 0.4f, 0.25f, 5.0f, 840.0f,
                         1.3f, BM_STONE, 1.0f, 0.2f, B_OBSIDIAN, 0},
    /* Soils: clay is sticky and holds a short overhang; mud and peat are
     * mostly water, soft to land on and slow to warm; podzol is the ashen,
     * needle-strewn soil under conifers. */
    [B_CLAY] = {"clay", BF_SOLID | BF_OPAQUE, ALL(T_CLAY), 2, 1800.0f, 0.45f, 0.03f, 1.0f, 920.0f, 1.3f, BM_DIRT, 0.9f,
                0.3f, 0, 0},
    [B_MUD] = {"mud", BF_SOLID | BF_OPAQUE | BF_GRANULAR, ALL(T_MUD), 0, 1700.0f, 0.3f, 0.01f, 0.5f, 2500.0f, 1.5f,
               BM_DIRT, 0.7f, 0.5f, 0, 0},
    [B_PEAT] = {"peat", BF_SOLID | BF_OPAQUE, ALL(T_PEAT), 1, 900.0f, 0.5f, 0.05f, 0.6f, 1900.0f, 0.4f, BM_DIRT, 0.75f,
                0.2f, 0, 0},
    [B_PODZOL] = {"podzol", BF_SOLID | BF_OPAQUE, ALL(T_PODZOL), 1, 1300.0f, 0.6f, 0.05f, 0.8f, 1300.0f, 0.8f, BM_DIRT,
                  0.9f, 0.45f, 0, 0},
    /* Ores are rock with a share of mineral, denser than the rock for the
     * metal in them. The tier is the softest pickaxe that will chip the
     * mineral out: soft deposits (bauxite, sulfur, salts, graphite) give to
     * any, the hard ones (ilmenite, quartz) only to iron. Rock salt passes
     * heat about twice as well as granite, graphite ten times, and sulfur
     * hardly at all. */
    [B_TIN_ORE] = {"tin ore", BF_SOLID | BF_OPAQUE, ALL(T_TIN_ORE), 15, 3200.0f, 0.7f, 0.25f, 8.0f, 700.0f, 3.0f,
                   BM_STONE, 1.0f, 0.4f, 0, TIER_STONE},
    [B_ZINC_ORE] = {"zinc ore", BF_SOLID | BF_OPAQUE, ALL(T_ZINC_ORE), 15, 3000.0f, 0.7f, 0.25f, 7.5f, 720.0f, 3.0f,
                    BM_STONE, 1.0f, 0.4f, 0, TIER_STONE},
    [B_LEAD_ORE] = {"lead ore", BF_SOLID | BF_OPAQUE, ALL(T_LEAD_ORE), 15, 3500.0f, 0.7f, 0.25f, 7.0f, 600.0f, 2.5f,
                    BM_STONE, 1.0f, 0.4f, 0, TIER_STONE},
    [B_NICKEL_ORE] = {"nickel ore", BF_SOLID | BF_OPAQUE, ALL(T_NICKEL_ORE), 15, 3300.0f, 0.7f, 0.25f, 9.0f, 700.0f,
                      3.5f, BM_STONE, 1.0f, 0.4f, 0, TIER_COPPER},
    [B_SILVER_ORE] = {"silver ore", BF_SOLID | BF_OPAQUE, ALL(T_SILVER_ORE), 15, 3000.0f, 0.7f, 0.25f, 8.0f, 720.0f,
                      3.5f, BM_STONE, 1.0f, 0.4f, 0, TIER_STONE},
    [B_GOLD_ORE] = {"gold ore", BF_SOLID | BF_OPAQUE, ALL(T_GOLD_ORE), 15, 2900.0f, 0.7f, 0.25f, 9.0f, 750.0f, 5.0f,
                    BM_STONE, 1.0f, 0.4f, 0, TIER_STONE},
    [B_BAUXITE] = {"bauxite", BF_SOLID | BF_OPAQUE, ALL(T_BAUXITE), 15, 2400.0f, 0.6f, 0.25f, 4.0f, 900.0f, 1.5f,
                   BM_STONE, 1.0f, 0.5f, 0, TIER_WOOD},
    [B_TITANIUM_ORE] = {"titanium ore", BF_SOLID | BF_OPAQUE, ALL(T_TITANIUM_ORE), 15, 3400.0f, 0.7f, 0.25f, 10.0f,
                        700.0f, 2.5f, BM_STONE, 1.0f, 0.4f, 0, TIER_IRON},
    [B_URANIUM_ORE] = {"uranium ore", BF_SOLID | BF_OPAQUE, ALL(T_URANIUM_ORE), 15, 3600.0f, 0.7f, 0.25f, 9.0f, 650.0f,
                       2.5f, BM_STONE, 1.0f, 0.4f, 0, TIER_COPPER},
    [B_LITHIUM_ORE] = {"lithium ore", BF_SOLID | BF_OPAQUE, ALL(T_LITHIUM_ORE), 15, 2800.0f, 0.7f, 0.25f, 9.0f, 800.0f,
                       3.0f, BM_STONE, 1.0f, 0.4f, 0, TIER_COPPER},
    [B_SULFUR_ORE] = {"sulfur deposit", BF_SOLID | BF_OPAQUE, ALL(T_SULFUR_ORE), 15, 2100.0f, 0.6f, 0.25f, 3.0f, 710.0f,
                      0.27f, BM_STONE, 1.0f, 0.3f, 0, TIER_WOOD},
    [B_SALTPETER_ORE] = {"saltpeter deposit", BF_SOLID | BF_OPAQUE, ALL(T_SALTPETER_ORE), 15, 2100.0f, 0.6f, 0.25f,
                         3.0f, 950.0f, 1.0f, BM_STONE, 1.0f, 0.3f, 0, TIER_WOOD},
    [B_ROCK_SALT] = {"rock salt", BF_SOLID | BF_OPAQUE, ALL(T_ROCK_SALT), 15, 2170.0f, 0.5f, 0.25f, 4.0f, 880.0f, 6.0f,
                     BM_STONE, 1.0f, 0.3f, 0, TIER_WOOD},
    [B_QUARTZ_ORE] = {"quartz vein", BF_SOLID | BF_OPAQUE, ALL(T_QUARTZ_ORE), 15, 2650.0f, 0.7f, 0.25f, 11.0f, 740.0f,
                      7.0f, BM_STONE, 1.0f, 0.4f, 0, TIER_IRON},
    [B_GRAPHITE_ORE] = {"graphite seam", BF_SOLID | BF_OPAQUE, ALL(T_GRAPHITE_ORE), 15, 2200.0f, 0.4f, 0.25f, 4.0f,
                        710.0f, 25.0f, BM_STONE, 1.0f, 0.3f, 0, TIER_WOOD},
    /* Basaltic melt; crude oil of a medium grade; brine at a quarter salt
     * by weight, near saturation. */
    [B_LAVA] = {"lava", BF_FLUID | BF_TRANSLUCENT, ALL(T_LAVA), 0, 2600.0f, 0.0f, 0.0f, 0.0f, 1200.0f, 1.5f, BM_WATER,
                1.0f, 0.0f, 0, 0},
    [B_CRUDE_OIL] = {"crude oil", BF_FLUID | BF_TRANSLUCENT, ALL(T_CRUDE_OIL), 0, 870.0f, 0.0f, 0.0f, 0.0f, 2000.0f,
                     0.15f, BM_WATER, 1.0f, 0.0f, 0, 0},
    [B_BRINE] = {"brine", BF_FLUID | BF_TRANSLUCENT, ALL(T_BRINE), 0, 1190.0f, 0.0f, 0.0f, 0.0f, 3300.0f, 0.55f,
                 BM_WATER, 1.0f, 0.0f, 0, 0},
    /* Light sources (light.c gives their colours). A torch is pine and a
     * pitch-soaked head: nothing to stand on, gone at a touch. The lamp is
     * a copper frame round a glass chimney; foxfire is fungus grown
     * through rotten matter on a cave floor, soft and wet. */
    [B_TORCH] = {"torch", 0, ALL(T_ITEM_TORCH), 0, 500.0f, 0.5f, 0.2f, 0.05f, 1700.0f, 0.15f, BM_WOOD, 1.0f, 0.0f, 0,
                 0},
    [B_LAMP] = {"oil lamp", BF_SOLID, ALL(T_LAMP), 0, 1500.0f, 0.6f, 0.2f, 0.3f, 600.0f, 1.0f, BM_GLASS, 1.0f, 0.0f, 0,
                0},
    /* Birch is the densest and hardest of these; the conifers are light
     * and soft, and the thorn tree's wood is dense and tough. Leaves as the
     * oak's: needles are a little denser. */
    [B_BIRCH_LOG] = {"birch log", BF_SOLID | BF_OPAQUE, SIDES_TOP_BOTTOM(T_BIRCH_SIDE, T_BIRCH_TOP, T_BIRCH_TOP), 6,
                     650.0f, 0.6f, 0.3f, 3.0f, 1700.0f, 0.15f, BM_WOOD, 1.0f, 0.3f, 0, 0},
    [B_SPRUCE_LOG] = {"spruce log", BF_SOLID | BF_OPAQUE, SIDES_TOP_BOTTOM(T_SPRUCE_SIDE, T_LOG_TOP, T_LOG_TOP), 6,
                      450.0f, 0.6f, 0.3f, 2.5f, 1700.0f, 0.12f, BM_WOOD, 1.0f, 0.3f, 0, 0},
    [B_PINE_LOG] = {"pine log", BF_SOLID | BF_OPAQUE, SIDES_TOP_BOTTOM(T_PINE_SIDE, T_LOG_TOP, T_LOG_TOP), 6, 520.0f,
                    0.6f, 0.3f, 2.5f, 1700.0f, 0.13f, BM_WOOD, 1.0f, 0.3f, 0, 0},
    [B_THORN_LOG] = {"thorn log", BF_SOLID | BF_OPAQUE, SIDES_TOP_BOTTOM(T_THORN_SIDE, T_LOG_TOP, T_LOG_TOP), 6, 800.0f,
                     0.6f, 0.3f, 3.5f, 1700.0f, 0.17f, BM_WOOD, 1.0f, 0.3f, 0, 0},
    [B_BIRCH_LEAVES] = {"birch leaves", BF_SOLID | BF_OPAQUE, ALL(T_BIRCH_LEAVES), 3, 150.0f, 0.4f, 0.1f, 0.25f,
                        2000.0f, 0.1f, BM_LEAVES, 0.35f, 0.0f, 0, 0},
    [B_SPRUCE_LEAVES] = {"spruce needles", BF_SOLID | BF_OPAQUE, ALL(T_SPRUCE_LEAVES), 3, 180.0f, 0.4f, 0.1f, 0.25f,
                         2000.0f, 0.1f, BM_LEAVES, 0.4f, 0.0f, 0, 0},
    [B_PINE_LEAVES] = {"pine needles", BF_SOLID | BF_OPAQUE, ALL(T_PINE_LEAVES), 3, 170.0f, 0.4f, 0.1f, 0.25f, 2000.0f,
                       0.1f, BM_LEAVES, 0.4f, 0.0f, 0, 0},
    [B_THORN_LEAVES] = {"thorn leaves", BF_SOLID | BF_OPAQUE, ALL(T_THORN_LEAVES), 3, 140.0f, 0.4f, 0.1f, 0.25f,
                        2000.0f, 0.1f, BM_LEAVES, 0.4f, 0.0f, 0, 0},
    [B_FOXFIRE] = {"foxfire", BF_SOLID | BF_OPAQUE, ALL(T_FOXFIRE), 2, 1100.0f, 0.6f, 0.1f, 0.5f, 2500.0f, 0.4f,
                   BM_DIRT, 0.7f, 0.1f, 0, 0},
};
