==============================================================================
  DIMENSIONS RECOMPILED  -  TESTER BUILD  -  READ THIS FIRST
==============================================================================

Installed to: {INSTALL_DIR}

!! THIS IS AN EARLY TEST BUILD. IT IS NOT STABLE. EXPECT BUGS. !!

   - The game can crash, freeze, show broken graphics or wrong colours,
     lose audio, or refuse to load a save. All of that is expected at this
     stage, and reporting it is exactly why you have this build.
   - Back up anything you care about. Saves live in the "content" folder
     next to the game (see SAVES below) - copy that folder to keep them.
   - Not affiliated with, or endorsed by, LEGO, TT Games or Warner Bros.
     This installer contains no game data: everything came from your own
     disc dump and your own Title Update.


------------------------------------------------------------------------------
  1. HOW TO PLAY
------------------------------------------------------------------------------

  1. Start  legodimensions.exe  (or the desktop shortcut).
  2. Start the LEGO Toypad app. It stands in for the USB Toy Pad: this is
     where you place and remove characters, vehicles and gadgets.
{TOYPAD_LINE}
     The app and the game find each other automatically over localhost
     (port 9191). Start order does not matter.
  3. In the Toypad app, press BACK / SELECT on your controller to open the
     figure picker (the shortcut can be changed in the app's Settings).

  Controller: any XInput pad (Xbox, or anything Steam / DS4Windows presents
  as XInput). Keyboard also works - defaults: WASD = left stick, arrows =
  right stick, Shift+arrows = D-pad, Space/; = A, Backspace/' = B, L = X,
  P = Y, Q/I = LT, E/O = RT, 1 = LB, 3 = RB. All of it is rebindable in the
  F4 menu under Input / Keybinds.


------------------------------------------------------------------------------
  2. HOTKEYS
------------------------------------------------------------------------------

  F4          Settings menu (graphics, frame rate, input, everything below)
  F8          Mods menu
  F7          Achievements list
  F3          Debug overlay (FPS, timings). Handy when reporting a bug.
  `  (tilde)  Console / log view
  Alt+Enter   Toggle between borderless fullscreen and a 1280x720 window

  Every hotkey can be rebound in F4 (Input / Binds).


------------------------------------------------------------------------------
  3. THE F4 SETTINGS THAT MATTER
------------------------------------------------------------------------------

  Everything below is saved to  legodimensions.toml  next to the game when
  you quit, so you can also edit that file by hand while the game is closed.
  Settings marked [restart] only take effect after restarting the game.

  frame_rate  (30 / 60)                         default: 60
      Whether the game runs at 30 or 60 frames per second. The original
      Xbox 360 game runs at 30; 60 is an unlock and is where most of the
      remaining bugs hide. If something animates wrongly or physics feels
      off, try 30 before reporting.

  framerate_limit                               default: 60
      Hard cap on rendered frames per second. Keep it equal to frame_rate
      unless you know why you want otherwise. 0 = uncapped.

  vsync  (on / off)                             default: off
      Off avoids input lag and lets framerate_limit do the pacing. Turn it
      on only if you see tearing.

  present_effect  (bilinear / cas / fsr)        default: fsr   [restart]
      How the 1280x720 frame is scaled to your screen. "fsr" is AMD
      FidelityFX Super Resolution 1 (sharpest), "cas" is a lighter
      sharpen, "bilinear" is a plain blur. Works on any GPU brand.
      present_fsr_max_upsampling_passes (default 1) and
      present_cas_additional_sharpness fine-tune it.

  resolution_scale                              default: 1
      Renders the game internally at 1x, 2x, 3x the original 1280x720.
      2x = 2560x1440 internally, much crisper, roughly 3-4x the GPU load.
      Leave at 1 on anything weaker than a mid-range desktop card.

  fullscreen  (on / off)                        default: on
      Borderless fullscreen at your desktop resolution. Off = a window;
      windowed_width / windowed_height set its size (default 1280x720).
      Alt+Enter toggles this live.

  anisotropic_override                          default: 5 (16x)
      Texture filtering quality. Free on any modern GPU.

  pause_when_unfocused  (on / off)              default: off
      Freezes the game (and its audio) while another window has focus.

  Do NOT change anything under "Compatibility" in legodimensions.toml
  (invalid_function_nonfatal, license_mask, readback_*, gpu_*). Those
  values keep the game from crashing; the installer set them for you.


------------------------------------------------------------------------------
  4. SAVES
------------------------------------------------------------------------------

  Saves and achievements are stored in:
     {INSTALL_DIR}\content

  To back up: copy that whole folder. To restore: close the game, copy it
  back. Never delete individual save-slot folders while the game is
  running.

  KNOWN BUG - please read: if you start a NEW game and quit before the
  opening cutscenes have finished and you reach the first playable area,
  that save may be impossible to load afterwards. Play a new game until
  you are actually walking around before quitting the first time.

  {SAVECONV_SECTION}


------------------------------------------------------------------------------
  5. DLC
------------------------------------------------------------------------------

  {DLC_COUNT} DLC package(s) were installed to
     {INSTALL_DIR}\content\0000000000000000\5752084B\00000002

  To add more later, RE-RUN THE INSTALLER and point it at the new packages.
  Do not just copy DLC folders in by hand: each package also needs a small
  record under content\...\Headers\00000002, and without it the game lists
  the DLC but will not load any of it - characters stay locked and it asks
  you to install the content. The installer writes those records for you.


------------------------------------------------------------------------------
  6. MODS
------------------------------------------------------------------------------

{MODS_SECTION}


------------------------------------------------------------------------------
  7. REPORTING A BUG
------------------------------------------------------------------------------

  Please include:
    - what you were doing (level / dimension / what was on the Toypad)
    - whether frame_rate was 30 or 60, and resolution_scale
    - the log:   {INSTALL_DIR}\game.log   (it is overwritten every launch,
      so grab it right after the problem happens)
    - a screenshot or video if it is a graphics glitch
    - your GPU and driver version

  A crash on start with no window at all is almost always the graphics
  driver: update it. The game needs a DirectX 12 GPU (feature level 11_0)
  and a 64-bit CPU with AVX2 (Intel Haswell / AMD Zen 1 or newer).


------------------------------------------------------------------------------
  8. WHAT IS THIS, TECHNICALLY
------------------------------------------------------------------------------

  This is a static recompilation of the Xbox 360 executable: the original
  PowerPC code was translated to C++ once, ahead of time, and compiled into
  a native Windows program. The GPU, sound and system calls are provided
  by the ReXGlue runtime (derived from the xenia emulator). There is no
  emulator running underneath - which is why it can run at 60 fps, and also
  why any code path the translation missed shows up as a bug here rather
  than in xenia.

  Recompilation, runtime work, mods and this installer: NeverCookFirst
  Built on:  ReXGlue SDK (BSD-3)  -  xenia (BSD-3)  -
             LEGO Toypad app by harrysof (MIT)  -  AMD FidelityFX (MIT)

  Folder layout:
     legodimensions.exe, *.dll     the recomp
     legodimensions.toml           all settings (see section 3)
     game\                         your disc data
     update\                       Title Update 23
     update-mods\                  mod-patched copy of the update (if mods installed)
     content\                      saves, achievements, DLC
     mods\, tools\                 optional components
     game.log                      the log for bug reports
