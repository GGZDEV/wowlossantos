# Sources and verification limits

Public repository pages checked on 2026-10-03, Europe/Paris. No repositories were compiled or run while preparing this starter. Proposed architecture and protocol are our design, not claims of existing upstream functionality.

| Source | What it supports | What it does not prove |
|---|---|---|
| https://github.com/azerothcore/azerothcore-wotlk | Official C++ modular server targeting WoW 3.3.5a | Headless GTA session support or a ready-made combat API |
| https://github.com/azerothcore/azerothcore-wotlk/blob/master/src/server/game/Server/WorldSession.h | Real session/character lifecycle code exists and must be inspected | That null-socket/headless execution is safe end-to-end |
| https://github.com/DK22Pac/plugin-sdk | SDK for GTA ASI/CLEO plugin development | Support for every GTA binary or every proposed hook |
| https://github.com/ThirteenAG/Ultimate-ASI-Loader | Loader for custom .asi libraries | Exact proxy DLL choice for the user's install |
| https://github.com/jealous-sound/azerothcore-wotlk-coa | Public Conquest of AzerothCore fork; README mentions world DB package | Complete/reliable CoA class implementations |

During implementation follow links to the official core installation/module documentation and inspect pinned source. GitHub master links are discovery links, not reproducible pins. Write exact revisions into deps.lock.json and evidence.

The SkyCraft discussion motivated the guest-gameplay/host-world separation. This design does not depend on SkyCraft source and does not assert identical transport or feasibility.
