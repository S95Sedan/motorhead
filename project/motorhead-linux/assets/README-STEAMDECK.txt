MOTORHEAD - NATIVE STEAM DECK BUILD

This is a 64-bit Linux executable with native OpenGL GPU rendering. It runs
directly on SteamOS and does not use Wine, Proton, Direct3D, or DXVK.
Windows and Linux share game code while keeping platform backends and User data separate.

COPY AND PLAY

1. Copy this whole folder to a writable location on the Deck, for example:
     /home/deck/Games/Motorhead
   Keep the content folders and User beside the Motorhead binary.
2. Launch Launch-Motorhead.sh once in Desktop Mode before using Gaming Mode.
   If prompted, approve/allow it to run. Wait for the main menu, then quit.
   Complete this first launch and approval before playing through Steam;
   skipping the execution approval can stop the game launching properly.
   If the script opens as text, allow execution in its file permissions first.
3. While still in Desktop Mode, open Steam > Games > Add a Non-Steam Game > Browse.
   Select Launch-Motorhead.sh (choose All Files if necessary).
4. In the shortcut's Properties > Compatibility, leave "Force the use of a
   specific Steam Play compatibility tool" UNCHECKED. Apply the standard
   Gamepad controller template with Steam Input enabled. Play in Gaming Mode.

If Steam cannot launch the script directly after a Windows file copy, use:
  Target:         /bin/bash
  Start In:       "/home/deck/Games/Motorhead"
  Launch Options: "/home/deck/Games/Motorhead/Launch-Motorhead.sh"
Use the folder you actually copied to and keep the quotes around paths.
The launcher sets the game's executable permission automatically, without sudo.
SteamOS supplies the graphics, X11/XWayland and audio drivers.

DISPLAY AND GRAPHICS

Defaults: 1280x800, 16:10, borderless fullscreen, native OpenGL, SDR, 32-bit textures,
trilinear filtering, 100 percent view distance, and a 100 percent race HUD.
The renderer uses GPU depth testing, perspective-correct textures, distance
fog, car shadows, and depth-projected headlights. It batches geometry and
caches texture mipmaps on the GPU. OpenGL is the default renderer. Software
remains available as an alternative in Graphic Options.
Road textures use anisotropic filtering when supported, retaining mipmaps
for stable distant detail. This follows the Trilinear filtering setting.

Normal Deck play is locked to fullscreen. The Window mode row is fixed at
Fullscreen, including the in-race menu. Lower render resolutions are scaled
to the fullscreen display; older Windowed/exclusive settings cannot override it.

Resolution choices fit the connected display. On the Deck panel:
  16:10: 960x600 or 1280x800
  16:9:  960x540 or 1280x720
  4:3:   640x480, 800x600 or 1024x768
Larger choices become available on a larger external monitor. Oversized saved
settings are clamped when starting or moving back to the handheld display.
Front-end artwork keeps its original proportions; races use a full 16:10 camera. Startup movies are
skipped by default, and the original files are included.

Use Steam's per-game performance controls to target 60 FPS / 60 Hz on LCD or
OLED. A native build removes the Windows translation layer, but a speed or
battery-life improvement is not guaranteed until measured on the Deck.
Leave HDR off. For more headroom, try 960x600 or reduce view distance/detail.

CONTROLS

Menus, pause, and results:
  D-pad / left stick     Navigate
  A                     Confirm
  B                     Back
  Menu (Start)          Pause / resume; back in menus

Movies:
  A / B / X / Y / Menu   Skip (follows the configured skip-one / skip-all mode)
  Stick movement        Does not skip; holding a button does not skip more clips

Driving:
  Left stick            Analog steering (15 percent dead zone)
  D-pad left/right      Digital steering
  R2 / L2               Analog accelerator / brake and reverse
  A                     Handbrake
  B                     Rear view
  X                     Horn
  Y                     Cycle driving cameras
  R1 / L1               Shift up / down in Manual transmission
  Menu (Start)          Pause / resume

Replay:
  L1 / R1               In-car / external view
  Y                     Track camera
  X                     Switch car
  Menu (Start)          Return to menus

Select Name or Team in Personal Options and press A to open Steam's floating
keyboard. Steam + X also opens the keyboard. Normal keyboard/mouse input works.
Keep MenuJoystick Off: this variant's standard gamepad adapter handles menus.

In Control Options, stick directions bind as PadLeft / PadRight and triggers
as PadLT / PadRT. Capture listens to the gamepad before menu navigation. If an
older build saved LeftArrow / RightArrow for the stick, bind those rows again.
Steam's controller layout must output a gamepad (use the Gamepad template).
Selecting a joystick/gamepad driving profile uses its saved assignments;
the default driving layout above applies to keyboard-only profiles. A remapped
stick/trigger keeps analog precision, and default pad actions do not overlap.

SAVES AND UPDATES

User contains this package's independent settings and player data, initially
copied from the current Windows profile with Deck display/control defaults.
The two copies do not share writable files. Local game assets, movies, and
music are included, and no mounted CD is required.

Back up and preserve the Deck's User folder when replacing the binary or
content with an updated build. Do not overwrite it with an old packaged User.
The native binary is named Motorhead; the optional launcher is
Launch-Motorhead.sh. You can keep the Proton package in a different folder as
a fallback, with its own Steam shortcut and User folder.

TROUBLESHOOTING

Run in Gaming Mode with the Gamepad template. Confirm compatibility forcing
is off and the game folder is writable. A microSD mounted with noexec cannot
run the binary; use the internal drive if necessary.

The game log is User/motorhead.log. Restore these settings with the game shut:
  Renderer                opengl
  ScreenDimension         1280 800
  AspectRatio             16:10
  WindowMode              Borderless
  MenuJoystick            Off

This build uses X11/XWayland, as provided in SteamOS Gaming Mode and the Deck
desktop. It statically includes SDL and the C++ runtime, dynamically loads
the system's graphics/audio/input drivers, and targets glibc 2.31 or newer.

VALIDATION

See native-validation.json for the actual completed checks. Physical Deck
performance, speakers/headphones, sleep/resume, and battery life still need
hardware testing. This is not a Valve Deck Verified release.
