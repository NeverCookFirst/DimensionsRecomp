#include "../../rexlego/src/gpu_native/blend_state.h"
using namespace legodimensions::gpu_native;
static_assert(!DecodeColorBlend(0x00010001).enabled);
static_assert(DecodeColorBlend(0x00010001).supported);
// Actual TT logo/fade blend: source alpha / inverse source alpha; independent alpha.
constexpr auto fade = DecodeColorBlend(0x07010706);
static_assert(fade.enabled && fade.supported);
static_assert(fade.source == 6 && fade.destination == 7 && fade.operation == 0);
static_assert(fade.alpha_source == 1 && fade.alpha_destination == 7 && fade.alpha_operation == 0);
static_assert(DecodeColorBlend(0x01010101).supported); // additive
static_assert(DecodeColorBlend(0x00010081).operation == 4); // reverse subtract
static_assert(!DecodeColorBlend(0x000100E1).supported); // invalid op
static_assert(!DecodeColorBlend(0x00010002).supported); // invalid factor
static_assert(!DecodeColorBlend(0x0001000E).supported); // constant alpha is not constant RGB
static_assert(DecodeColorBlend(0x00010D0C).supported); // constant RGB
static_assert(!DecodeColorBlend(0xE001E001).enabled); // reserved bits don't affect identity
int main() {}
