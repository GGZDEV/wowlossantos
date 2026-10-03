# CoA — later provider audit

Candidate repository: https://github.com/jealous-sound/azerothcore-wotlk-coa

Its public identity is Conquest of AzerothCore. Its README describes an additional world database package. This establishes that a repository exists; it does not establish completeness, fidelity, stability, the presence of a working Tinker class or compatibility with this bridge.

After M1:
1. Pin a CoA revision and corresponding world database/data versions. Record licenses and required client-derived data separately.
2. Diff session/Player, spell/aura, DB schema, class IDs, resource handling and character advancement code against the official baseline.
3. Build/boot independently and list classes actually exposed by data/code.
4. Demonstrate one selected class and its representative abilities natively before mapping GTA input/UI.
5. Define provider capabilities and adapt the existing bridge seam; keep protocol and GTA entity mirroring unchanged where possible.
6. Use separate test DBs; never import fork data into the existing standard schema without a tested migration/backup plan.

Custom Ascension classes are not guaranteed to fit standard WotLK class enums or character-create packets. Capability catalogs must come from the active provider. Do not invent spell IDs, implement fake talents or call a reskinned rogue a working Tinker.

Choose the core only after evidence, not GitHub name or star count. If CoA lacks the desired mechanics, retain standard 3.3.5a as the working provider and explain the exact gap.
