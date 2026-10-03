# Avengers reference follow-up, October 3

Read `research/avengers/REPORT.md`, `avgref.py`, `ids.py` and `ids2.py`.
Used the installed PC executable and extracted shader index read-only. No PC
game was launched. Reference bytecode/disassembly remains in ignored local
`session-20261003/avengers-reference`; none is embedded or committed.

The remaining native portrait is empty for both Sonic and Batman, while the
emulated slot2 Vorton reference has Sonic's portrait. Binding all texture slots
did not repair it. These captures establish a shared HUD defect, not its cause.

Concrete engine anchors from `avgref.py xref portrait` and local x86 decoding:

- PC `portrait_mask_texture` string0x0192101C is used in0x00BD36A0 and
  0x00C68A10. `m_portraitMaskMtlOverride` string0x0193D57C is used in0x00C68A10.
  That setup loads a portrait mask property, tests availability, constructs an
  override material and updates its texture/material flags. This is setup code,
  not an identified missing native draw.
- Dimensions' same property is string0x8224CBF8, referenced in0x83293E68
  (property registration) and0x833738B8 (setup). Its override string0x8227F2F0
  is used in0x833738B8. Setup stores the loaded object at owner+84; after the
  property lookup succeeds, it creates override storage at owner+200 and passes
  the mask's texture to material+956. Those exact pointers can connect future
  HUD draw/texture captures to the CPU object rather than guessing from color.
- `83373B00` handles event119 and rounds requested dimensions using scene+320,
  with several character/owner prerequisites. It is not proven to issue the
  portrait draw. Nearby `83373850` is a destructor; address proximity does not
  establish a render entry point.

Queried `levels\icons\hud_icons\hud_icons`: several shaders have no PC family
match. VS20 has a same-asset PC match atVS0; its reflected constants are camera,
instance and material with position/UV inputs. It applies world/view projection
and UV transforms. However2500 PC shaders share that reduced name signature.
This is a material-family reference, not proof of bytecode equivalence or that
this shader is the missing portrait pass. Other HUD pixel-family matches chose
different assets, so they were not treated as exact semantic ground truth.

No portrait fix is claimed. Next required evidence is the actual override
material's bound shader/texture and its sampled alpha/UV at a confirmed HUD
draw, compared with the reference. Generic bank175 coverage no longer reports
unavailable pipelines in the captured candidate hub, so more guessed shaders
or texture-slot overrides are not justified by that evidence.
