"""Run with qrenderdoc --python after setting LEGO_OZ_REPLAY_DIRECTORY.

The directory must contain road_frame2473.rdc (or set LEGO_OZ_REPLAY_CAPTURE)
and production-prelinked 1E13090CD0FA6780-linked-0.dxil and
FFA4DD4B364A999D-linked-0.dxil and E669A744921D81E3-linked-0.dxil.
The latter must be extracted from the candidate bank, even when unchanged.
This never launches the game. Event/pixel
coordinates intentionally identify this specific private regression capture.
"""
import json
import os
from pathlib import Path
import traceback
import renderdoc as rd

out = Path(os.environ['LEGO_OZ_REPLAY_DIRECTORY'])
capture = Path(os.environ.get('LEGO_OZ_REPLAY_CAPTURE', str(out / 'road_frame2473.rdc')))
controller = cap = None
exit_code = 1
(out / 'position-regression-error.txt').unlink(missing_ok=True)
try:
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(str(capture), '', None) != rd.ResultCode.Succeeded:
        raise RuntimeError('Capture open failed')
    status, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if status != rd.ResultCode.Succeeded:
        raise RuntimeError('Replay device creation failed')
    textures = {str(t.resourceId): t.resourceId for t in controller.GetTextures()}
    target = textures['ResourceId::5264']
    originals = {}
    for event in (820, 31775, 1007):
        controller.SetFrameEvent(event, True)
        originals[event] = controller.GetPipelineState().GetShader(rd.ShaderStage.Vertex)
    results = {}
    points = [(40, 875), (42, 875), (50, 870), (25, 890)]
    # The shared static prepass also draws rocks and non-wind foliage. Changing
    # it alone caused a shipped regression that a wind-pair-only test missed.
    controller.SetFrameEvent(1007, True)
    shared_original = bytes(controller.GetPipelineState().GetShaderReflection(
        rd.ShaderStage.Vertex).rawBytes)
    shared_candidate = (out / 'E669A744921D81E3-linked-0.dxil').read_bytes()
    if shared_candidate != shared_original:
        raise AssertionError('Unvalidated shared prepass change: test all paired color shaders first')
    for variant in ('baseline', 'position_precise', 'restored_baseline'):
        if variant == 'position_precise':
            for event, shader in ((820, 'FFA4DD4B364A999D'),
                                  (31775, '1E13090CD0FA6780'),
                                  (1007, 'E669A744921D81E3')):
                blob = (out / (shader + '-linked-0.dxil')).read_bytes()
                replacement, error = controller.BuildTargetShader(
                    'main', rd.ShaderEncoding.DXIL, blob,
                    rd.ShaderCompileFlags(), rd.ShaderStage.Vertex)
                if replacement == rd.ResourceId.Null():
                    raise RuntimeError(error)
                controller.ReplaceResource(originals[event], replacement)
        elif variant == 'restored_baseline':
            for original in originals.values():
                controller.RemoveReplacement(original)
        controller.SetFrameEvent(33611, True)
        rows = []
        for x, y in points:
            history = controller.PixelHistory(target, x, y, rd.Subresource(), rd.CompType.Typeless)
            matches = [h for h in history if h.eventId == 31775]
            if len(matches) != 1:
                raise AssertionError('Capture no longer matches this regression fixture')
            h = matches[0]
            rows.append(dict(x=x, y=y, depth_failed=h.depthTestFailed,
                             discarded=h.shaderDiscarded, before_depth=h.preMod.depth,
                             shader_depth=h.shaderOut.depth, color=list(h.postMod.col.floatValue)))
        results[variant] = rows
        (out / 'position-regression.json').write_text(json.dumps(results, indent=2))
        for row in rows[:3]:
            assert not row['discarded']
            if variant == 'position_precise':
                assert not row['depth_failed'] and row['before_depth'] == row['shader_depth']
                assert max(row['color'][:3]) > 0.1
            else:
                assert row['depth_failed'] and row['shader_depth'] > row['before_depth']
                assert max(row['color'][:3]) == 0
        assert rows[3]['discarded']  # Transparent leaf cutout must still reject.
        rock = [h for h in controller.PixelHistory(
            target, 600, 440, rd.Subresource(), rd.CompType.Typeless)
                if h.eventId == 32540]
        assert rock and not rock[-1].depthTestFailed
        assert max(rock[-1].postMod.col.floatValue[:3]) > 0.03
        results[variant + '_rock'] = dict(depth_failed=rock[-1].depthTestFailed,
            before_depth=rock[-1].preMod.depth, shader_depth=rock[-1].shaderOut.depth,
            color=list(rock[-1].postMod.col.floatValue))
        (out / 'position-regression.json').write_text(json.dumps(results, indent=2))
        controller.SetFrameEvent(33611, True)
        save = rd.TextureSave()
        save.resourceId = target
        save.destType = rd.FileType.PNG
        controller.SaveTexture(save, str(out / (variant + '-scene.png')))
    exit_code = 0
except BaseException:
    (out / 'position-regression-error.txt').write_text(traceback.format_exc())
finally:
    if controller:
        controller.Shutdown()
    if cap:
        cap.Shutdown()
    (out / 'position-regression-exit.txt').write_text(str(exit_code))
    os._exit(exit_code)
