// legodimensions - ReXGlue Recompiled Project
//
// Discord Rich Presence. Speaks Discord's local IPC protocol directly over the
// named pipe the desktop client listens on, so there is no third-party
// dependency: the official discord-rpc C library was archived in 2019 and does
// not support presence buttons, which we want.
//
// Everything is driven from cvars (category "Discord"), so the text, the art
// asset and the button can be retuned from legodimensions.toml without a
// rebuild. See discord_presence.cpp for the list.

#pragma once

namespace legodimensions::discord {

// Starts the background connection thread. Cheap and non-blocking: if Discord
// is not running the thread just keeps retrying, and if the cvar discord_rpc
// is false this does nothing at all. Safe to call when Discord is absent.
void Start();

// Stops the thread and drops the connection. Discord clears the presence by
// itself once the pipe closes. Safe to call even if Start() was never called.
void Stop();

}  // namespace legodimensions::discord
