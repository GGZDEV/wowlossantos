# World projection and authority

## Entity mapping

Registry key is (epoch, entity_id), mapped server-side to a core GUID plus incarnation and host-side to a GTA pool reference/handle plus generation. Never use a ped index alone as a persistent ID: pools reuse slots. Reject an update for an old incarnation even if a slot now holds another actor.

Only dedicated test entities are mapped in M1. Ordinary traffic/peds are not automatically creatures. Core destruction and GTA streaming loss are distinct lifecycle events. HOST_ENTITY_LOST is not a kill and grants no XP.

## Coordinates

For the selected test rectangle use p_core = origin_core + scale * R * (p_gta - origin_gta). R is a calibrated axis/orientation transform; positive scale converts units. Verify with measured displacement and native spell range, not assumptions about coordinate systems. Orientation has its own conversion. Record the calibration and map ID in config only after testing.

Never send raw GTA coordinates straight into a random Azeroth map. Core range and positional requirements operate on projected native positions. Initial movement must remain within the bounded arena and reasonable speeds; teleports/loading invalidate the binding.

## Geometry limitation

Core terrain, LOS and pathfinding refer to its own maps. GTA collisions describe Los Santos. Those are different geometries. M1 runs only in an open region proven compatible on both sides; do not globally disable range, LOS or spell checks to pretend the maps match.

Later investigate a scoped bridge-world collision/LOS interface or real custom map data. Host raycasts may be observations for a local trusted bridge but do not, by themselves, replace every native core geometry check. Crowd navigation, ground height, projectiles and interiors need independent treatment.

## Health/death

Core retains actual HP/max HP. Mirror absolute HP to the ped where the supported binary can represent it reliably; document units/clamping. If a later normalized display is necessary, publish the conversion explicitly and retain native values in UI. Never subtract combat-event damage and then also apply a snapshot.

For M1 prevent native weapon, vehicle, fall/fire and world hazards from independently damaging the player/test target within the selected arena. Prove scoping; the remainder of GTA must not become invulnerable. Do not invoke a GTA damage routine that silently runs unrelated armor/damage formulas. Verify health field updates plus death handling through source-verified adapter paths.

Future GTA damage needs an explicit DAMAGE_INTENT path and a defined mapping into native server mechanics, including attribution. That is outside M1.

## AI

M1 target is stationary/non-attacking; suppress both native GTA attack tasks and autonomous core movement for that fixture. Later choose who owns NPC intent/navigation. One option is core combat/aggro intent with GTA movement execution and position feedback, but it requires a reconciliation contract. Running both engines' AI independently gives contradictory attacks and positions.

## Pause/save/load

For the bounded bridge fixture, SET_PAUSED must suspend its combat timers and effects with a verified implementation, then acknowledge final state. Do not assume pausing GTA pauses worldserver. If scoped pause isn't feasible without extensive core changes, implement M1 pause as reset/despawn of the fixture and fresh recreation on resume, and report that limitation. Never age bleeds invisibly and apply accumulated damage on resume.

GTA saves are not RPG character saves. Core persists the character through its native lifecycle. GTA loading/new game destroys bindings, changes epoch and requires a fresh snapshot. For M1 no active encounter is serialized. Prevent GTA auto-save/load from silently restoring mismatched bridge state.
