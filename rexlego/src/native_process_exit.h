#pragma once

#include <cstdlib>
#if defined(_WIN32)
#include <windows.h>
#endif

namespace legodimensions {
// Called only after the player confirms the native Quit Game dialog. Avoid
// guest joins, GPU drains and DLL-detach waits. Committed settings/saves have
// already been written by their owners; a pending preview is never committed.
[[noreturn]] inline void ExitNativeGameNow() {
#if defined(_WIN32)
  TerminateProcess(GetCurrentProcess(), 0);
#endif
  std::_Exit(0);
}
}  // namespace legodimensions
