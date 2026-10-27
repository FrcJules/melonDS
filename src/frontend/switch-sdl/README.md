# melonDS pour Nintendo Switch (frontend SDL2 minimal)

Frontend homebrew pour le cœur melonDS 1.x : rendu logiciel, **JIT ARM64**, **fastmem**, FreeBIOS + démarrage direct (aucun BIOS, firmware ou ROM n'est fourni ni nécessaire).
Testé sur Switch Atmosphère, firmware 20.2.0. **Lancer hbmenu en mode application** (R tenu en lançant un jeu) : le mode applet (Album) n'a pas assez de mémoire.

## Compilation
Toolchain devkitPro (devkitA64, libnx, portlibs `SDL2`, `SDL2_ttf`, `freetype`, `harfbuzz`, `libpng`, `bz2`, `zlib`) :
```
cmake -S . -B build-switch -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake -DBUILD_SWITCH_SDL=ON -DBUILD_QT_SDL=OFF -DENABLE_OGLRENDERER=OFF
cmake --build build-switch --target melonDS-switch
```
Résultat : `src/frontend/switch-sdl/melonDS-switch.nro` à copier dans `sdmc:/switch/`.

## Utilisation
- Sans `rom=` dans la configuration, un **explorateur** s'ouvre (haut/bas, A choisir, B dossier parent, +/- quitter) ; le dernier choix est retenu (`lastrom.txt`).
- **ZL+ZR+PLUS** : menu (disposition empilée / côte à côte, filtre pixels/lissé, fps à l'écran, emplacement d'état, sauver/charger, changer de ROM, quitter).
- **ZL+ZR tenus, puis X = sauver l'état, Y = charger, gauche/droite = emplacement 1 à 4**. Sauvegarde automatique à la sortie et rechargement au lancement (emplacement 0).
- Sortie : L+R+PLUS+MINUS. Écran tactile = écran du bas.

## Fichiers (`sdmc:/switch/melonds-fork/`)
| Fichier | Rôle |
|---|---|
| `config.txt` | `rom=` `sav=` (pour cette ROM seulement) `romdir=` `jit=0\|1` `fastmem=0\|1` `threaded3d=0\|1` `overclock=<MHz>` `audiointerp=0..4` (défaut 3 : cubique) `autosave=0\|1` `autoload=0\|1` |
| `<nom de la ROM>.sav` | sauvegarde du jeu (une ROM choisie dans l'explorateur a son propre `.sav` ici) |
| `<nom de la ROM>.ss0..4` | états |
| `ui.txt`, `lastrom.txt` | réglages d'affichage, dernière ROM |
| `log.txt` | une ligne par seconde : image/s, ms/image, file audio, sous-alimentations, fautes mémoire, fréquence CPU/EMC/GPU, température |

## Réserves
Mesures et limites : `docs/STATUS.md` (section Switch) du dépôt de travail. Le son n'a été vérifié qu'objectivement (file audio), pas à l'oreille. Le texte du menu utilise la police système de la console (lue à l'exécution, non distribuée).
Licence : GPL-3.0-or-later (comme melonDS). Bibliothèques liées statiquement : SDL2 / SDL2_ttf (zlib), FreeType (FTL ou GPLv2), HarfBuzz (MIT), libpng, bzip2, zlib : textes de licence **non relus** ici.
