# Azeroth Theft Auto — dossier de lancement

Objectif : jouer à un RPG dont AzerothCore 3.3.5a calcule les règles, avec GTA San Andreas comme monde visible. À terme : création du personnage, races/classes, sorts, ressources, progression, équipement et éventuellement classes CoA.

**Livraison actuelle : M0 prouvé (sort natif AzerothCore sans client WoW) et M1a (IPC) testé ; l'ASI GTA est compilé mais pas encore testé en jeu.** Les choix proposés doivent être validés sur les sources et la machine cible. Aucun test en jeu n'a été effectué.

## Démarrer avec Claude Code

1. Extraire cette archive et ouvrir le dossier `azeroth-theft-auto` dans Claude Code sur un PC Windows où GTA SA est disponible.
2. Coller le contenu de `START-HERE.txt` dans la conversation. Claude lit ensuite `CLAUDE.md` et les documents référencés.
3. Commencer par Milestone 0 : build du core, recherche d'une session sans client WoW, personnage réel, créature réelle et sort réel. Puis réaliser Milestone 1 : reflet des PV dans GTA.
4. Si les installations ou les données de jeu manquent, Claude doit préparer le code et les commandes réalisables, puis donner précisément l'action locale restante.

Le premier succès attendu : **appuyer sur une touche dans GTA → sort validé et exécuté par AzerothCore → PV autoritaires modifiés → ped GTA affichant ces PV.** Un nombre de dégâts inventé par le bridge ne suffit pas.

## Contenu

- `CLAUDE.md` : instructions maîtres pour l'agent de code.
- `START-HERE.txt` : prompt de la première session.
- `docs/architecture.md` : composants, responsabilités, threads et frontières.
- `docs/feasibility.md` : vrais verrous, hypothèses et audit du core.
- `docs/milestones.md` : ordre de travail et critères de passage.
- `docs/protocol.md` : contrat IPC local, identité et resynchronisation.
- `docs/world-and-authority.md` : cartes, coordonnées, PV et IA.
- `docs/setup.md` : prérequis et procédure de construction.
- `docs/acceptance.md` : preuves et essais à produire.
- `docs/coa.md` : audit du fork CoA après le prototype standard.
- `docs/sources.md` : dépôts vérifiés et pistes d'inspection.
- `config/bridge.example.json` : configuration de conception, à implémenter.
- `protocol/envelope.schema.json` : schéma initial de l'enveloppe.
- `protocol/examples.jsonl` : trace illustrative, sans valeur de test en jeu.
- `deps.lock.json` : dépendances connues, SHA à fixer après audit.

## Choix de départ

| Élément | Choix |
|---|---|
| Jeu hôte | GTA SA The Definitive Edition (x64) ; GTA SA classique 1.0 US (x86) aussi supporté |
| Adapter | C++ / Plugin-SDK / plugin ASI (x64 pour DE, x86 pour le classique) |
| Gameplay | AzerothCore officiel 3.3.5a, module compilé dans worldserver |
| Serveur initial | Windows x64 sur le même PC ; autre OS possible plus tard |
| Transport initial | TCP sur 127.0.0.1, JSON encadré ; pas d'accès distant |
| Simulation | Boucle native du core ; aucune réécriture des sorts |
| Zone test | Petite zone extérieure ouverte, map proxy existante côté core |
| Apparence initiale | CJ et un ped de test, modèles fantastiques plus tard |
| CoA | Provider ultérieur, après audit et preuve des classes utilisées |

Le transport ne définit pas le concept de passthrough. La mémoire partagée reste possible après mesure ; elle n'est pas nécessaire pour prouver le gameplay autoritaire.

## État du travail

- [x] Cadrage, protocole de départ et critères d'acceptation.
- [x] Dépendances clonées et révisions fixées (`deps.lock.json`).
- [x] AzerothCore compilé et données locales validées (Linux x86_64 ; build Windows x64 non fait).
- [x] Personnage sans client WoW connecté prouvé (M0b, `docs/evidence/m0-m1-report.md`).
- [x] Module + ASI compilés (ASI x86 via clang/mingw ; build MSVC non exécuté).
- [ ] Sort et PV synchronisés en jeu — côté core prouvé avec `bridge-cli`, test GTA en attente (`docs/evidence/m1-gta-checklist.md`).
- [ ] Création personnage / progression / CoA.
