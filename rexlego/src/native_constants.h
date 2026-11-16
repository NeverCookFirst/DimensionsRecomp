#pragma once
#include <optional>

namespace legodimensions {
bool NativeConstantPublisherEnabled();
// Returns an acknowledgement only for the first valid control value or a change.
std::optional<bool> PollNativeConstantPublisher();
}
