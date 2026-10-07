#!/usr/bin/env python3
"""Compare every supplied cadence outcome; never select the fastest window."""
import sys
sys.dont_write_bytecode = True
import argparse
import json
from pathlib import Path

from prepare_run import require, write_json
from sample_cadence import complete_rows, stats


def comparison_identity(sample, varying='command-slots'):
    scene = sample['scene']
    flags = {key: value for key, value in sample['flags'].items()
             if key.startswith(('LEGO_NATIVE_', 'LEGO_DUMP_', 'LEGO_GPU_', 'REX_TIMER_'))}
    for key in ('LEGO_NATIVE_FRAME_METRICS', 'LEGO_DUMP_MISSING_SHADERS',
                'LEGO_NATIVE_SHADER_PACK', 'LEGO_NATIVE_SHADER_PACK_TRIGGER'):
        flags.pop(key, None)  # Private paths differ; sampler verifies their ownership.
    flags.pop('LEGO_NATIVE_COMMAND_SLOTS' if varying == 'command-slots' else 'REX_TIMER_WAIT_BLOCKING', None)
    return {'binaries': sample['binaries'], 'checkpoint': sample['checkpoint'], 'criteria': sample['criteria'],
            'normalized_config': sample['normalized_config'], 'shader_packs': sample['shader_packs'],
            'supporting_state': sample['supporting_state'],
            'scene_id': scene['scene_id'], 'camera_id': scene['camera_id'],
            'renderer': scene['renderer'], 'figures': scene['figures'], 'flags': flags}


def compare(paths, varying='command-slots'):
    require(varying in ('command-slots', 'timer-wait'), 'Unknown experimental variable')
    samples = [json.loads((path/'sample.json').read_text()) for path in paths]
    for path, sample in zip(paths, samples):
        if 'metrics' in sample:
            _, rows = complete_rows(path/'frames.csv')
            require(stats(rows) == sample['metrics'], 'Frozen CSV disagrees with sample metrics')
    outcomes = [{'source': str(path.resolve()), 'status': sample['status'], 'slots': sample['slots'],
                 'timer_wait': sample['timer_wait'],
                 'metrics': sample.get('metrics'), 'rejection_reasons': sample.get('rejection_reasons', [])}
                for path, sample in zip(paths, samples)]
    accepted = [sample for sample in samples if sample['status'] == 'accepted']
    identity = comparison_identity(accepted[0], varying) if accepted else None
    comparable = (all(comparison_identity(sample, varying) == identity for sample in accepted)
                  if accepted else None)
    means = {}
    if comparable:
        variable = 'slots' if varying == 'command-slots' else 'timer_wait'
        for value in sorted({sample[variable] for sample in accepted}):
            group = [sample for sample in accepted if sample[variable] == value]
            frames = sum(sample['metrics']['frames'] for sample in group)
            seconds = sum(sample['metrics']['recorded_seconds'] for sample in group)
            means[str(value)] = {'windows': len(group), 'frames': frames,
                                 'recorded_seconds': seconds, 'fps': frames/seconds}
    baseline, candidate = ('3', '12') if varying == 'command-slots' else ('spin', 'blocking')
    gain = (100*(means[candidate]['fps']/means[baseline]['fps']-1)
            if candidate in means and baseline in means else None)
    return {'outcomes': outcomes, 'comparable_identity': comparable, 'accepted_aggregate': means,
            'experimental_variable': varying, 'candidate_vs_baseline_fps_percent': gain,
            'source_default_changed': False,
            'decision': 'Retain source defaults; measurements never automatically promote an experimental setting.',
            'limits': ['All supplied windows are retained, including rejected and unfavorable outcomes.',
                       'Operator scene assertions and NPC/particle phases limit causal attribution.',
                       'This compares native present cadence; PM4 XE_SWAP metrics have a different boundary.',
                       'Repeated A/B/A evidence and visual correctness are needed before a policy change.']}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('samples', nargs='+', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--vary', choices=('command-slots', 'timer-wait'), default='command-slots')
    args = parser.parse_args()
    require(not args.output.exists(), 'Refuse overwrite of a previous comparison')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    report = compare(args.samples, args.vary)
    write_json(args.output, report)
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
