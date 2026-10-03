# Milestones and stop conditions

| Stage | Deliverable | Acceptance |
|---|---|---|
| M0a | Source/build audit and exact revisions | Core builds and boots with real local data; session/map plan cites code |
| M0b | Native headless gameplay proof | Real Player casts native learned spell at Creature, health/resource results proven without connected WoW client |
| M1a | Local IPC + diagnostic host | Framing, handshake, rejection, request dedupe and snapshots tested |
| M1b | ASI loader + one dedicated target | Plugin loads on identified GTA binary; target handle/generation survives validation |
| M1c | End-to-end authoritative spell | Key 1 → core cast → absolute target HP mirrored once; rejected casts cause no HP change |
| M2 | One complete saved character flow | Core creates/loads/saves character; supported race/class catalog drives selection |
| M3 | Small playable combat slice | Resources, GCD, cooldown display, aura ticks, death/reset and combat controls |
| M4 | Progression and inventory | XP/loot through native credit flow; reload preserves progression |
| M5 | World authority and appearance | Explicit NPC AI policy, obstacle-aware movement/LOS, alternate models/animations |
| M6 | Audited CoA provider | Demonstrated class/abilities on pinned CoA data; matching character advancement |

Stop after M1c report. Later stages are product direction, not the first session's implementation scope.

## Controls for the first GTA test

Configurable debug toggle: F9 enables/disables bridge test mode after identifying conflicts. F8 spawns/resets one dedicated test target; key 1 requests the selected spell. Ignore key repeats and input while menus/loading are active. Show connection, target HP and last cast result using a minimal text overlay/log. Do not permanently bind the game's weapon controls outside test mode.

## M1 exact scope

One configured character, one stationary dedicated hostile target, one real spell, one calibrated open arena. No traffic/police damage, no native attacking target, no full Los Santos actor mirroring. Keep the normal world running outside the dedicated test entities.

## Product character flow after M1

Catalog → name/race/class/appearance selection → server validates legal combinations → native character create/load → GTA model mapping. Until custom assets exist, use explicit placeholder mappings to stock peds. A stock ped cannot visually represent a tauren faithfully. Quadruped shapeshifts and wearable armor need rigs, skinning, animation and model work; swapping an ID alone does not implement them.

Level-ups and XP must come from real damage attribution/death handling. Native quest, loot, threat and talents are not automatically exposed just because combat works.
