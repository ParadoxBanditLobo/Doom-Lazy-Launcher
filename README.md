# Doom Lazy Launcher v0.3

A tiny terminal launcher for Doom source ports on Linux.

It is meant to be a lazy command builder, not a full mod manager. Point it at:

- an engine folder, such as UZDoom, VKDoom, GZDoom, or AppImages
- an IWAD/base WAD folder, such as DOOM.WAD, DOOM2.WAD, TNT.WAD, PLUTONIA.WAD
- a mods folder, such as .wad, .pk3, .pk7, .deh, .bex, .zip, or folders

Then create profiles that remember the exact launch command.

## Run

```sh
chmod +x DoomLazyLauncher-v0.3-Linux-x86_64
./DoomLazyLauncher-v0.3-Linux-x86_64
```

## Main menu

```text
1. Launch saved profile
2. New launch
3. Set folders
4. Delete profile
5. Show config/help
6. Quit
```

## New in v0.3

- Interactive mod load-order builder
- Add multiple mods by number
- Move selected mods up or down
- Remove selected mods
- Clear selected mods
- Save the exact selected order in profiles

This is useful for setups like:

```text
Engine: UZDoom.AppImage
IWAD: DOOM2.WAD
Mods:
  1. Project_Brutality.pk3
  2. SomeMapPack.wad
  3. CompatibilityPatch.pk3
Extra args:
  -skill 3
```

The launcher does not download mods, detect dependencies, or guess load order. You control the order manually.

## Config location

```text
~/.config/doom-lazy-launcher/config.txt
```

## Build from source

```sh
cc -std=c99 -Wall -Wextra -O2 -o DoomLazyLauncher doom_lazy_launcher_v0_3.c
```

No third-party libraries are required.

## Full-Disclosure
Ai was used to create this

