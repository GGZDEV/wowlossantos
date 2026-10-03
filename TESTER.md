# Tester Azeroth Theft Auto (M1) — guide rapide

Objectif du test : dans GTA SA, appuyer sur **1** → AzerothCore lance une vraie Boule de feu → les PV
du loup baissent côté serveur → le ped GTA affiche exactement ces PV.

Ce qui est déjà prouvé sans GTA : `docs/evidence/m0-m1-report.md`. Ce qui reste à valider, c'est la partie GTA.

## Prérequis

- PC Windows 10/11 avec **GTA San Andreas – The Definitive Edition** (PC, Steam/Rockstar/Epic, `SanAndreas.exe`
  64 bits) → plugin `dist/gta-sa/de/`.
  (La version classique 1.0 US reste supportée → `dist/gta-sa/classic/`.)
- Le support DE de Plugin-SDK est **expérimental** : les fonctions du jeu sont retrouvées par signature binaire,
  ce qui survit en général aux mises à jour mais n'a pas encore été vérifié sur ta version du jeu.
- WSL2 avec Ubuntu 24.04 (`wsl --install -d Ubuntu-24.04` dans PowerShell admin, puis redémarrer).
- ~25 Go libres dans WSL, 8 Go de RAM conseillés.

## Étape 1 — serveur AzerothCore dans WSL (une seule fois, 30-60 min)

Dans le terminal Ubuntu (WSL) :

```bash
git clone -b claude/m0-m1-gamebridge https://github.com/ggzdev/wowlossantos.git ~/wowlossantos
cd ~/wowlossantos
scripts/setup-wsl.sh
```

Le script installe les paquets, récupère AzerothCore au commit épinglé, compile le serveur avec le
module, télécharge les données client (v20.0, vérifiées par SHA-256), configure la fixture et génère
un token secret dans `local/bridge.token`. Il affiche le token à la fin.

## Étape 2 — démarrer et vérifier le serveur (sans GTA)

```bash
cd ~/wowlossantos
scripts/worldserver-ctl.sh start && scripts/worldserver-ctl.sh wait-ready
sleep 10; grep "in world" local/logs/core/Server.log   # 1er démarrage : crée le compte + le perso
scripts/check-bridge.sh
```

Attendu : `CAST_STATUS accepted` puis `completed`, `target hp initial=55 now=3x`, `OK: native cast…`.
Si ça passe, le serveur est bon. Arrêt : `scripts/worldserver-ctl.sh stop` (sauvegarde le perso).

## Étape 3 — réseau Windows ↔ WSL

GTA se connecte à `127.0.0.1:17635`. Recommandé (Windows 11 22H2+) : créer
`C:\Users\<toi>\.wslconfig` avec

```ini
[wsl2]
networkingMode=mirrored
```

puis `wsl --shutdown` et relancer Ubuntu + le serveur. Vérifier depuis PowerShell :

```powershell
Test-NetConnection 127.0.0.1 -Port 17635    # TcpTestSucceeded : True
```

(Sur Windows 10, la redirection localhost par défaut de WSL2 suffit en général ; même vérification.)

## Étape 4 — installer le plugin dans GTA SA Definitive Edition

1. Trouver le dossier qui contient **`SanAndreas.exe`** (en général `…\Gameface\Binaries\Win64\`).
   Faire une copie de sauvegarde de ce dossier.
2. ASI loader 64 bits : télécharger
   https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/download/x64-latest/dinput8-x64.zip et mettre
   `dinput8.dll` à côté de `SanAndreas.exe` (ne pas écraser un fichier existant ; si rien ne se charge,
   renommer en `version.dll` ou `winmm.dll`, cf. README du loader).
3. Copier `dist/gta-sa/de/AzerothTheftAuto.asi` et `dist/gta-sa/de/AzerothTheftAuto.ini` dans le même dossier
   (ou un sous-dossier `scripts\` / `plugins\`). Depuis Windows :
   `\\wsl$\Ubuntu-24.04\home\<user>\wowlossantos\dist\gta-sa\de\` (SHA-256 dans `AzerothTheftAuto.asi.sha256`).
4. Créer `AzerothTheftAuto.token` au même endroit que le `.asi`, contenant uniquement le token
   (`cat ~/wowlossantos/local/bridge.token`).
5. Lancer le jeu une fois : un fichier **`AzerothTheftAuto.log`** doit apparaître à côté du `.asi`
   (sinon dans `%TEMP%`) avec « adapter loaded for GTA SA Definitive Edition (x64) ».

(Version classique 1.0 US : même chose avec `dist/gta-sa/classic/`, l'ASI loader **x86** et `gta_sa.exe`.)

## Étape 5 — le test

En DE il n'y a **pas d'overlay à l'écran** (l'outil de texte de Plugin-SDK ne supporte pas DE) : on suit le log.
Dans PowerShell, à côté du jeu :

```powershell
Get-Content .\AzerothTheftAuto.log -Wait -Tail 20
```

1. Serveur démarré (étape 2). Lancer GTA SA DE, charger une sauvegarde.
2. Aller à **Grove Street devant la maison de CJ** (≈ 2495, −1670) — zone de l'arène.
3. **F9** → log `WELCOME …` puis `bound target-1 …` ; un ped apparaît ~14 m à l'ouest de CJ.
4. Se tourner vers le ped, **1** → log `CAST_STATUS cast-n accepted` puis `completed`, puis
   `MIRROR target-1 core hp 3x/55 -> gta ped health 3x` : les deux nombres doivent être identiques.
5. Rappuyer **1** jusqu'à la mort du ped (`death presented once`, animation de mort une seule fois). **F8** = nouveau loup.
6. **F9** = sortir du mode test (le ped disparaît, CJ redevient vulnérable).

Ce qu'il faut me renvoyer : `AzerothTheftAuto.log`, `~/wowlossantos/local/logs/core/Server.log`,
la version du jeu (propriétés de `SanAndreas.exe`) et ce que tu as vu à l'écran.
Checklist complète : `docs/evidence/m1-gta-checklist.md`.

## Si ça coince

| Symptôme | Cause probable |
|---|---|
| Pas de `AzerothTheftAuto.log` | ASI loader absent / mauvais nom (essayer `version.dll`, `winmm.dll`), ou mauvais build (x64 pour DE) |
| Message d'erreur Plugin-SDK au lancement (« pattern » / « address ») | ta version de DE n'est pas reconnue par le SDK : m'envoyer la version exacte |
| Log « no bridge token » | fichier `.token` absent / vide |
| Log « core connection closed » en boucle | serveur arrêté ou réseau WSL (étape 3) |
| ERROR `outside_arena` | CJ trop loin de Grove Street (rayon ~36 m) |
| ERROR `unauthorized` | token différent entre GTA et le serveur |
| `SPELL_FAILED_UNIT_NOT_INFRONT` | CJ ne regarde pas le ped (tourne-toi vers lui) |
| `SPELL_FAILED_OUT_OF_RANGE` | trop loin du ped (calibrage d'échelle non encore fait) |
| Crash au lancement | noter la version exacte du jeu ; retirer le .asi et me l'envoyer |

Note : l'orientation/échelle GTA↔WoW n'est pas encore calibrée ; c'est justement un des buts de ce test.
