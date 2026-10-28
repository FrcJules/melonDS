# melonDS pour Nintendo Switch (frontend SDL2 + GLES 3)

Frontend homebrew pour le cœur melonDS 1.x : rendu **GPU (GLES 3, résolution ×1 à ×3)** ou logiciel, **JIT ARM64**, **fastmem**, FreeBIOS + démarrage direct (aucun BIOS, firmware ou ROM n'est fourni ni nécessaire).
Testé sur Switch Atmosphère, firmware 20.2.0. **Lancer hbmenu en mode application** (R tenu en lançant un jeu) : le mode applet (Album) n'a pas assez de mémoire.

## Compilation
Toolchain devkitPro (devkitA64, libnx, portlibs `SDL2`, `SDL2_ttf`, `freetype`, `harfbuzz`, `libpng`, `bz2`, `zlib`) :
```
cmake -S . -B build-switch -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake -DBUILD_SWITCH_SDL=ON -DBUILD_QT_SDL=OFF \
    -DENABLE_OGLRENDERER=ON -DENABLE_OGL_COMPUTE=OFF -DMELONDS_KEEP_SYMBOLS=ON
cmake --build build-switch --target melonDS-switch
```
Les bibliothèques `EGL GLESv2 glapi drm_nouveau` (Mesa de la console) sont liées par le CMake du frontend ; le renderer GL du cœur est compilé en mode OpenGL ES (`MELONDS_GLES`).

Variante plus rapide (LTO partiel, ~+3 à +10 % de fps mesurés ; le renderer GL est exclu du LTO car il s'effondrait avec, cause inconnue ; `DMA.cpp` aussi à cause d'un bug interne de GCC 15.2) :
```
-DCMAKE_BUILD_TYPE=Release "-DCMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG -flto=1" "-DCMAKE_C_FLAGS_RELEASE=-O3 -DNDEBUG -flto=1" \
-DCMAKE_EXE_LINKER_FLAGS_RELEASE=-flto=1 -DMELONDS_LTO_EXCLUDE_DMA=ON -DMELONDS_LTO_EXCLUDE_GL=ON
```
`-DMELONDS_KEEP_SYMBOLS=ON` garde la table de symboles dans l'ELF (le `.nro` n'en dépend pas) pour décoder les rapports de crash d'Atmosphère avec `aarch64-none-elf-addr2line`.
Résultat : `src/frontend/switch-sdl/melonDS-switch.nro` à copier dans `sdmc:/switch/`.

## Utilisation
- Sans `rom=` dans la configuration, un **explorateur** s'ouvre (haut/bas, A choisir, B dossier parent, +/- quitter) ; le dernier choix est retenu (`lastrom.txt`).
- **ZL+ZR+PLUS** : menu (disposition empilée / côte à côte, filtre pixels/lissé, fps à l'écran, **fréquence CPU auto/1020/1224/1581/1785 MHz**, **rendu 3D logiciel ou GPU**, **résolution GPU 1×-3×**, 3D en thread séparé (logiciel), emplacement d'état, sauver/charger, changer de ROM, quitter). Les réglages sont mémorisés.
- **Rendu GPU par défaut (2×)** : le cœur retombe seul sur le rendu logiciel si le renderer GL ne démarre pas. Mode CPU **auto** : démarre à 1224 MHz et passe à 1785 MHz si le jeu reste sous 57 fps pendant 3 s.
- **ZL+ZR tenus, puis X = sauver l'état, Y = charger, gauche/droite = emplacement 1 à 4**. Sauvegarde automatique à la sortie et rechargement au lancement (emplacement 0).
- Sortie : L+R+PLUS+MINUS. Écran tactile = écran du bas.

## Fichiers (`sdmc:/switch/melonds-fork/`)
| Fichier | Rôle |
|---|---|
| `config.txt` | `rom=` `sav=` (pour cette ROM seulement) `romdir=` `jit=0\|1` `fastmem=0\|1` `gpu=0\|1` `gpuscale=1..3` `threaded3d=0\|1` `overclock=<MHz>` (fixe ; absent = auto) `audiointerp=0..4` (défaut 3 : cubique) `autosave=0\|1` `autoload=0\|1` `loadslot=0..4` `jitblock=` `jitliteral=` `jitbranch=` `helpercore=0..2` `gltest=1` (sonde GLES 3) |
| `<nom de la ROM>.sav` | sauvegarde du jeu (une ROM choisie dans l'explorateur a son propre `.sav` ici) |
| `<nom de la ROM>.ss0..4` | états |
| `ui.txt`, `lastrom.txt` | réglages d'affichage, dernière ROM |
| `log.txt` | une ligne par seconde : image/s, ms/image, file audio, sous-alimentations, étirement audio (`skew`), profil du renderer GL (`glprof`) et de la boucle d'émulation (`prof`), fautes mémoire, fréquence CPU/EMC/GPU, température |

## Réserves
Mesures et limites : `docs/STATUS.md` (section Switch) du dépôt de travail. Le son : le crépitement en dessous de 60 fps est supprimé (la file audio n'est plus vidée : le son est étiré, donc un peu plus grave, si le jeu tourne sous 60 fps). Le texte du menu utilise la police système de la console (lue à l'exécution, non distribuée).
Licence : GPL-3.0-or-later (comme melonDS). Bibliothèques liées statiquement : SDL2 / SDL2_ttf (zlib), FreeType (FTL ou GPLv2), HarfBuzz (MIT), libpng, bzip2, zlib : textes de licence **non relus** ici.
