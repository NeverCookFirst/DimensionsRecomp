// legodimensions - ReXGlue Recompiled Project
//
// Starts the LEGO Toypad companion app alongside the game. Almost every player
// runs the two together, so the game can do it for them.

#pragma once

namespace legodimensions::toypad_app {

// Starts the app now, whatever the autostart setting says.
void Launch();

// Closes the app again, but only if this game started it - one a player had
// open before us is left alone. Safe when nothing was started.
void StopIfStarted();

// Launches the app if toypad_app_autostart is on and it is not already running.
// Safe to call when the feature is off - it does nothing and costs nothing.
void StartIfEnabled();

}  // namespace legodimensions::toypad_app
