# Simpsons architecture experiment for LEGO Dimensions TU23

Reference: TheSimpsonsGameRecomp `8c2f2c62b1dcf1fc352766e6f41827c58604c6f7`,
[`native_renderer_plan.md`](https://github.com/YesterMester/TheSimpsonsGameRecomp/blob/8c2f2c62b1dcf1fc352766e6f41827c58604c6f7/simpsons/re/native_renderer_plan.md).

That project keeps the original XDK D3D command emission, uses PM4 as the state
source, and progressively replaces parts of the SDK Vulkan renderer. Its shader
translations and native resolves are checked against captured reference frames.
Our current renderer instead replaces XDK functions with direct D3D12 draws.

## Implemented first experiment

`LEGO_NATIVE_PM4_REFERENCE=1` routes **all 55 native GPU replacements** to their
generated original `__imp__sub_*` implementations. The SDK GPU plugin therefore
receives the original command stream. This covers creation, placement resources,
shader binding, state, draws, resolves, queries, kicks and synchronization.
Both typed and raw hook wrappers select the route before any native body runs.
Selection is fixed at first use to prevent mixing resource ownership models.
Normal launches read the restart-required `gpu_native_pm4` setting. An exact
environment value `1` forces PM4 and `0` forces the detached native renderer;
other environment values use the setting. Its compiled default is false, while
the current development configuration now persists true after visual validation.
The application startup uses the same selector: reference mode preserves the
SDK GPU plugin and skips creating the detached native device. Shutdown skips
native device teardown in reference mode as well.

The probe runner exposes `-Pm4Reference` to force PM4 and `-ConfiguredRenderer`
to test the persisted selection with no environment override. With neither flag
the runner explicitly selects detached native mode for existing diagnostic probes.
Native-only diagnostic and candidate
flags cannot be combined with this mode. The runner clears and restores the
environment and records the chosen route and executable/config hashes.

This is a reference experiment, **not a port of the Simpsons native Vulkan
resolves, AOT SPIR-V storage, or headless trace replayer**. It uses our installed
SDK GPU plugin and its configured backend (currently D3D12). The original SDK
translation/cache path supplies shaders; our DXIL archive and native blur
override are bypassed along with the native hooks. Existing configuration shader
filters still apply. Performance and blur must therefore be assessed separately
from whether missing geometry, logos and effects appear.

## Verification and next decision

`test_renderer_route.py` compiles the actual routing header against a mock PPC
context/marshaler, generates a wrapper for every real native GPU hook, and checks
both paths across twelve environment/configuration cases. It verifies original-body
exclusivity, persisted selection and overrides, argument/register preservation,
and immutable route selection.
Full application linking also verifies every original target is available.
These are offline dispatch checks, not proof that original GPU initialization
and the full game work in this executable.

The first authorized run (`oct01-pm4-reference1`) exposed a missing startup
switch: the old app setup cleared the SDK plugin even though the hooks selected
PM4. It stopped before the cutscene with no GPU ring consumer. The process was
closed. Startup selection is now corrected and the offline test executes the
actual `OnPreSetup` / `OnPreLaunchModule` bodies against mock host services to
verify plugin preservation and exclusive native initialization.

`oct01-pm4-reference2` successfully booted on the main monitor. Observed in the
cutscene: Vortech's legs and translucent geometry, blue particles/light, and
the Dimensions logo vortex. At the title screen, the central portal, floating
glowing spheres, orbit lines and three complete characters appeared. Evidence
is under `rexlego/out/native-gpu/session-20261001/pm4-reference2/`. This materially
improves the reported defects; it does not prove every game area is correct.
The process was closed after the title-screen capture. The first pipeline warmup
took 78.5 seconds; a subsequent configured run reused the driver cache and took
0.97 seconds for the same 9707 pipeline descriptions. This is startup timing,
not gameplay FPS.

`oct01-pm4-configured1` also used this route with no environment override and
captured Vortech's translucent torso and brick-built hand/arm. The persisted
setting therefore affects an ordinary launch, rather than only a probe command.
`oct01-pm4-tt3` captured the complete TT Games logo and production text in
`session-20261001/pm4-tt3/logo-12.png`. Earlier spot observations missed its short
display interval; they were not evidence that it was still missing.

The latest user instruction authorizes necessary tests on the PRIMARY monitor
without requesting approval for every run. Close each exact process after the
test. Correct rendering takes priority over FPS. The reference path has now
visibly restored the reported cutscene geometry, logo and title effects, but
unvisited levels and performance remain unverified. An improved PM4 reference
identifies a difference somewhere in the native hook/translation pipeline; it
does not isolate that difference by itself.

Menu navigation remains unverified: brief automated Return/Escape/J presses
did not leave the title prompt. This is an input-test limitation, not established
evidence of a rendering regression. The existing project notes already mention
that injected keyboard events may fail to reach SDL. No controller/input code
was changed for this renderer experiment. Do not disable synchronous memexport
readback to improve this result: previous fast/batched/on-demand trials caused
known geometry and UI regressions (see PROJECT-MEMORY.md sections 5.4-5.5).

Before adopting native resolve optimizations from Simpsons, establish an SDK
reference image, map TU23 resolve formats/aliasing/CPU reads, and reproduce the
same captured commands offline. Title-specific addresses and hardcoded reference
resolve regions from Simpsons must not be used for LEGO. No native resolve
optimization is enabled by this experiment.
