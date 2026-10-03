# Tester Azeroth Theft Auto (M1) — guide rapide

Objectif du test : dans GTA SA, appuyer sur **1** → AzerothCore lance une vraie Boule de feu → les PV
du loup baissent côté serveur → le ped GTA affiche exactement ces PV.

Ce qui est déjà prouvé sans GTA : `docs/evidence/m0-m1-report.md`. Ce qui reste à valider, c'est la partie GTA.

## Prérequis

- PC Windows 10/11 avec **GTA San Andreas PC, exécutable 1.0 US** (le plugin utilise les adresses
  1.0 US de Plugin-SDK). Les versions Steam/Rockstar Launcher récentes et la Definitive Edition ne sont
  **pas** supportées : utiliser un downgrader vers 1.0 US sur une copie du jeu.
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

## Étape 4 — installer le plugin dans GTA

1. Sauvegarder le dossier GTA.
2. Si aucun ASI loader n'est installé : Ultimate ASI Loader **v9.7.4 x86**
   (https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/tag/v9.7.4), copier son `dinput8.dll`
   (ou le nom que sa doc recommande pour GTA SA) à côté de `gta_sa.exe`. Ne pas écraser un DLL existant.
3. Copier `dist/gta-sa/AzerothTheftAuto.asi` et `dist/gta-sa/AzerothTheftAuto.ini` dans le dossier
   de GTA (ou `scripts\` si ton loader l'utilise). Récupérer les fichiers depuis Windows via
   `\\wsl$\Ubuntu-24.04\home\<user>\wowlossantos\dist\gta-sa\` (SHA-256 dans `AzerothTheftAuto.asi.sha256`).
4. Créer `AzerothTheftAuto.token` au même endroit, contenant uniquement le token
   (`cat ~/wowlossantos/local/bridge.token`).

## Étape 5 — le test

1. Serveur démarré (étape 2). Lancer GTA, charger une sauvegarde.
2. Aller à **Grove Street devant la maison de CJ** (≈ 2495, −1670) — zone de l'arène.
3. **F9** → un ped apparaît ~14 m à l'ouest de CJ, overlay « TEST ACTIVE » en haut à gauche.
4. **1** → après 1,5 s l'overlay affiche `target hp (core): 3x` et le ped a exactement ces PV.
5. Rappuyer **1** jusqu'à la mort du ped (animation de mort une seule fois). **F8** = nouveau loup.
6. **F9** = sortir du mode test.

Ce qu'il faut me renvoyer : `AzerothTheftAuto.log` (à côté du .asi, sinon `%TEMP%`),
`~/wowlossantos/local/logs/core/Server.log`, et ce que tu as vu à l'écran. Checklist complète :
`docs/evidence/m1-gta-checklist.md`.

## Si ça coince

| Symptôme | Cause probable |
|---|---|
| Pas de `AzerothTheftAuto.log` | ASI loader absent/mauvais nom, ou mauvaise version de `gta_sa.exe` |
| Log « no bridge token » | fichier `.token` absent / vide |
| Overlay « offline » / log « connecting » en boucle | serveur arrêté ou réseau WSL (étape 3) |
| ERROR `outside_arena` | CJ trop loin de Grove Street (rayon ~36 m) |
| ERROR `unauthorized` | token différent entre GTA et le serveur |
| `SPELL_FAILED_UNIT_NOT_INFRONT` | CJ ne regarde pas le ped (tourne-toi vers lui) |
| `SPELL_FAILED_OUT_OF_RANGE` | trop loin du ped (calibrage d'échelle non encore fait) |
| Crash au lancement | noter la version exacte du jeu ; retirer le .asi et me l'envoyer |

Note : l'orientation/échelle GTA↔WoW n'est pas encore calibrée ; c'est justement un des buts de ce test.
