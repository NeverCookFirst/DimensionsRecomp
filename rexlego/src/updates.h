// legodimensions - ReXGlue Recompiled Project
//
// Startup update check. The work is done by tools\rexupdate\rexupdate.exe - the
// installer's own binary with its payload stripped off - so all the game does is
// start it and forget about it, and nothing here can delay or break a launch.
//
// The switch the user is meant to find is the "updates_check" cvar, category
// Updates, which shows up as a checkbox in the F4 settings overlay.

#pragma once

namespace legodimensions::updates {

// Starts the updater in the background when updates_check is on and the updater
// is where the config says. It opens a window only if there is something new.
// Never blocks: a slow or unreachable GitHub is the updater's problem.
void CheckAtStartup();

}  // namespace legodimensions::updates
