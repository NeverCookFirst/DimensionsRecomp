// legodimensions - ReXGlue Recompiled Project
//
// Keeps the N0CUT5 (skippable cutscenes) flag on for as long as the
// skip_cutscenes setting is. See skip_cutscenes.cpp.

#pragma once

namespace legodimensions::skip_cutscenes {

// Starts the background thread; safe to call more than once.
void Start();

}  // namespace legodimensions::skip_cutscenes
