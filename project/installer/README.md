# Motorhead Installer

This Windows product builds `Installer.exe`. The Version page offers a Windows
installation or a native Steam Deck portable folder. Windows defaults to
`Program Files\Motorhead`; Steam Deck defaults to `Documents\Motorhead-Steamdeck`.
Steam Deck output contains an x86_64 ELF executable, launcher, instructions,
all game content, movies, ten local music WAVs and a separate User directory.
Fresh Deck settings use OpenGL, 1280x800, 16:10 and borderless display. Existing
profiles are preserved. A destination containing the other platform executable
is rejected before any files are replaced.

It detects an
owned retail MOTORHEAD mixed-mode CD, extracts the base retail data directly
from `MOTOR/DISK1/DATA1.CAB`, copies `movies/movies.pak`, and installs the
game executables into a user-selected production folder. The default location
is `Program Files\Motorhead`. Original CD audio
tracks 2 through 11 can optionally be ripped as `Music/track02.wav`
through `track11.wav`; music installation is enabled by default.

The installer and reconstructed game target 32-bit Windows XP (5.1) and newer.
The game automatically tries D3D12, D3D11, and D3D9 in that order, with a
software fallback when no hardware renderer can initialize.

Audio extraction uses continuous single-pass reads with drive-level retries and
adaptive transfer sizes. Drives that reject larger raw-CD transfers
automatically fall back to smaller blocks without slowing compatible drives.

The installer creates only the production layout shown below. ModelViewer and
development/test content are not installed:

```text
Motorhead\
  Motorhead.exe
  AddressBook\
  Cars\
  Data\
  Demos\
  Game\
  League\
  Movies\
  Music\
  Sounds\
  Tracks\
  User\
    motorhead.cfg
    Input\
      custom.clo
      Joystick.clo
      Keyboard.clo
      mouse.clo
      Wheel.clo
```

The installer reproduces the retail flat-folder layout directly in the selected
installation folder, then adds `User` for mutable reconstruction-owned data. It
extracts the retail CD data and applies the game-data groups from the official
Motorhead 3.0 update. It does not install or modify a retail executable. The
installer itself and the game executable are both 32-bit. The installer build
incrementally updates the 32-bit release executable from the
separate `project/motorhead-windows` project and the native Linux release from
`project/motorhead-linux`, embedding both directly in Installer.exe. When replacing an installation, each managed retail folder is
kept in a hidden
rollback directory until the new content validates successfully. `User` is
preserved unless the reset option is explicitly selected.

`motorhead.cfg` is not present in the retail CD cabinet; the original setup
generated it. The installer writes the captured original setup baseline only
to `User\motorhead.cfg` and keeps that file unchanged except for appending
`AspectRatio 4:3` and `WindowMode Windowed` directly below `Renderer` in the
original Visual section when either reconstruction display setting is absent.
It does not synthesize or replace any other setting, and no writable
configuration is left in the installed `Game` content folder. Older
`User\Game` layouts are flattened during upgrade, and legacy control layouts
from either `User\Game` or the `User` root are moved into `User\Input`.

The base retail cabinet does not contain `League\Default.LGF`, although the
reconstructed frontend requires it. The installer therefore embeds the
canonical installed p3.1 league definition as a required runtime data file and
validates its presence before reporting success. It does not replace or patch a
retail executable.

The installer also requires the user-owned European Motorhead 3.0 update named
`mhp30.exe` (14,025,656 bytes, SHA-256
`3dce2edb9fdbeba76ee66616a65d93b19a7cffe93bf473fb05b12ac6e14fc5f4`).
It validates the complete package before changing the installation, extracts
its inner InstallShield cabinet to a temporary directory, and applies only the
required game-data groups. The official update is not redistributed with this
project. Obsolete retail executables, multiplayer helpers, renderers, and other
root-level utilities are excluded. This supplies cars 11–14, all sixteen
forward/reverse track definitions, reverse motions, AI data, and the associated
updated assets. The reconstructed `Motorhead.exe` remains the installed program.

The embedded header banner is derived from the original retail `MenuBack.TGA`
artwork and is packaged into `Installer.exe` together with the Motorhead icon.

## Build

Run `build-installer.bat` from the repository root. It prepares the pinned tools,
compiles both release game executables, and embeds them in `bin/Installer.exe`.
The resulting installer is standalone; it does not need a companion Install
folder. Select the destination and platform in its wizard and supply your owned
retail disc/image and official 3.0 patch. Existing User data is preserved unless
reset is selected.

Build tools live under `.tools`, and generated CMake output under `.build`.
See [the repository README](../../README.md#build-from-source) for build requirements. Neither folder is included in the clean source mirror.

For unattended imports and regression checks, the wizard's importer is also
available as:

```text
Installer.exe --install-from-cue game.cue --patch mhp30.exe --destination output --target windows|steamdeck [--without-music] [--s40-cue s40.cue] [--reset-profile]
```

Music cannot be omitted for Steam Deck output. Managed content and program files
are restored together if installation fails or is cancelled.
