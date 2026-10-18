#!/usr/bin/env python3
"""Run melonDS-bench test cases and compare them with reference results.

  suite.py suite.json --rom-dir DIR --bench build/melonDS-bench --refs REFS [--out OUT] [--bless]

Paths of "input" and "state" are relative to the suite file; "rom" is relative to --rom-dir.
A suite file lists cases:
  {"rom": "game.nds",
   "cases": [{"name": "title", "frames": 300, "shots": [300], "audio": true, "jit": false,
              "input": "inputs/title.txt", "state": "states/start.mst"}]}

The references (screenshots and hashes) must be validated by a person, then stored
with --bless. Keep them, the ROMs and the savestates OUT of the repository.
Exit status: 0 if all cases match, 1 on any mismatch, 2 on usage or run errors.
Timings are printed for information only (they depend on the machine).
"""
import argparse, json, os, shutil, subprocess, sys


def read_ppm(path):
    data = open(path, 'rb').read()
    parts = data.split(b'\n', 3)
    w, h = map(int, parts[1].split())
    return w, h, parts[3]


def diff_ppm(a, b):
    wa, ha, da = read_ppm(a)
    wb, hb, db = read_ppm(b)
    if (wa, ha) != (wb, hb):
        return None
    pixels = wa * ha
    bad = 0
    worst = 0
    for i in range(pixels):
        pa, pb = da[i*3:i*3+3], db[i*3:i*3+3]
        if pa != pb:
            bad += 1
            worst = max(worst, max(abs(x - y) for x, y in zip(pa, pb)))
    return bad, pixels, worst


def run_case(args, suite, case, outdir):
    cmd = [args.bench, os.path.join(args.rom_dir, suite['rom']),
           '--frames', str(case['frames']), '--json', os.path.join(outdir, case['name'] + '.json')]
    if case.get('warmup'):
        cmd += ['--warmup', str(case['warmup'])]
    if not case.get('jit', False):
        cmd.append('--no-jit')
    if case.get('audio'):
        cmd.append('--audio-hash')
    for key, flag in (('input', '--input'), ('state', '--state')):
        if case.get(key):
            cmd += [flag, os.path.join(os.path.dirname(os.path.abspath(args.suite)), case[key])]
    for frame in case.get('shots', []):
        cmd += ['--shot', '%d:%s' % (frame, os.path.join(outdir, '%s-%d.ppm' % (case['name'], frame)))]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print('  run failed (rc=%d): %s' % (r.returncode, r.stderr.strip()[-300:]))
        return None
    return json.load(open(os.path.join(outdir, case['name'] + '.json')))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('suite')
    ap.add_argument('--rom-dir', required=True)
    ap.add_argument('--bench', required=True)
    ap.add_argument('--refs', required=True)
    ap.add_argument('--out', default='bench-out')
    ap.add_argument('--bless', action='store_true', help='store the results as the new references')
    args = ap.parse_args()

    suite = json.load(open(args.suite))
    os.makedirs(args.out, exist_ok=True)
    os.makedirs(args.refs, exist_ok=True)
    failed = False

    for case in suite['cases']:
        name = case['name']
        print('== %s' % name)
        res = run_case(args, suite, case, args.out)
        if res is None:
            sys.exit(2)

        ref_json = os.path.join(args.refs, name + '.json')
        if args.bless:
            shutil.copy(os.path.join(args.out, name + '.json'), ref_json)
            for frame in case.get('shots', []):
                fn = '%s-%d.ppm' % (name, frame)
                shutil.copy(os.path.join(args.out, fn), os.path.join(args.refs, fn))
            print('  blessed')
            continue

        if not os.path.exists(ref_json):
            print('  no reference yet (use --bless once you validated the output)')
            failed = True
            continue
        ref = json.load(open(ref_json))

        for shot, rshot in zip(res['shots'], ref['shots']):
            if shot['hash'] == rshot['hash']:
                print('  shot %d: ok' % shot['frame'])
                continue
            failed = True
            fn = '%s-%d.ppm' % (name, shot['frame'])
            d = diff_ppm(os.path.join(args.out, fn), os.path.join(args.refs, fn))
            if d is None:
                print('  shot %d: DIFFERENT (size changed)' % shot['frame'])
            else:
                print('  shot %d: DIFFERENT, %d/%d pixels differ, max channel delta %d' % (shot['frame'], d[0], d[1], d[2]))
        if len(res['shots']) != len(ref['shots']):
            print('  number of screenshots differs from the reference')
            failed = True

        if 'audio' in res and 'audio' in ref:
            if res['audio']['hash'] == ref['audio']['hash']:
                print('  audio: ok')
            else:
                failed = True
                print('  audio: DIFFERENT (%d samples vs %d)' % (res['audio']['samples'], ref['audio']['samples']))

        print('  speed: %.1f fps (reference %.1f fps, %+.1f%%)' % (
            res['fps'], ref['fps'], 100.0 * (res['fps'] / ref['fps'] - 1.0) if ref['fps'] else 0))

    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
