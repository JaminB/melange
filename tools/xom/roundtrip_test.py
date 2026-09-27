#!/usr/bin/env python3
"""
roundtrip_test.py - byte-identical round-trip check for xom.py.

For every file: bytes -> xom.loads -> JSON text -> json.loads -> xom.dumps -> bytes,
then compare with the original.  Going through JSON text on purpose, so the
float / string / ref encodings in the JSON model are exercised too.

    python roundtrip_test.py [--game <WormsXHD dir>] [--bundles] [--maps] [-v] [files...]

Default file set: Data/Tweak/*.XOM (includes DEFSAVE.XOM and LVLSETUP.XOM) and
every level/mission XOM directly under Data/ (Data/*.XOM).  --bundles adds
Data/Bundles/*.xom, --maps adds Data/Maps/*.xan / *.xom (mesh-bearing files;
see xom-format.md for what is and is not decoded there).

Reports: files byte-identical, and how many objects were fully decoded vs
kept opaque (raw bytes) because no schema matched.
"""
import argparse
import collections
import glob
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import xom  # noqa: E402

DEFAULT_GAME = r'C:\Program Files (x86)\Steam\steamapps\common\WormsXHD'


def collect(game, bundles, maps):
    data = os.path.join(game, 'Data')
    pats = [os.path.join(data, 'Tweak', '*.XOM'), os.path.join(data, '*.XOM')]
    if bundles:
        pats.append(os.path.join(data, 'Bundles', '*.xom'))
    if maps:
        pats += [os.path.join(data, 'Maps', '*.xan'), os.path.join(data, 'Maps', '*.xom')]
    seen = {}
    for p in pats:
        for f in glob.glob(p):
            seen.setdefault(os.path.normcase(os.path.abspath(f)), f)
    return sorted(seen.values(), key=lambda s: s.lower())


def check(path):
    with open(path, 'rb') as f:
        orig = f.read()
    doc = xom.loads(orig)
    text = xom.to_json(doc)
    doc2 = json.loads(text)
    out = xom.dumps(doc2)
    raw = collections.Counter(o['type'] for o in doc['objects'] if 'raw' in o or o.get('in_tail'))
    return out == orig, len(doc['objects']), raw, (orig, out)


def edit_test(path):
    """Change kWeaponBazooka's damage in the JSON model, write, re-read, compare."""
    with open(path, 'rb') as f:
        orig = f.read()
    doc = xom.loads(orig)
    bank = xom.obj(doc, doc['root'])
    entry = next(xom.obj(doc, r) for r in bank['fields']['ContainerResources']
                 if xom.obj(doc, r)['fields']['Name'] == 'kWeaponBazooka')
    ref = entry['fields']['Value']['ref']
    before = doc['objects'][ref - 1]['fields']['WormDamageMagnitude']
    doc['objects'][ref - 1]['fields']['WormDamageMagnitude'] = 75.0
    out = xom.dumps(doc)
    after = xom.loads(out)['objects'][ref - 1]['fields']['WormDamageMagnitude']
    diff = sum(a != b for a, b in zip(orig, out))
    print('edit test: kWeaponBazooka WormDamageMagnitude %g -> %g, %d byte(s) changed' % (before, after, diff))
    return len(out) == len(orig) and after == 75.0 and 0 < diff <= 4


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', default=DEFAULT_GAME)
    ap.add_argument('--bundles', action='store_true')
    ap.add_argument('--maps', action='store_true')
    ap.add_argument('-v', '--verbose', action='store_true')
    ap.add_argument('files', nargs='*')
    a = ap.parse_args()
    files = a.files or collect(a.game, a.bundles, a.maps)
    npass = nfail = nerr = 0
    nobj = 0
    rawtypes = collections.Counter()
    fully = 0
    for f in files:
        name = os.path.relpath(f, os.path.join(a.game, 'Data')) if not a.files else f
        try:
            ok, n, raw, (orig, out) = check(f)
        except Exception as e:  # parse or serialise error
            nerr += 1
            print('ERROR  %s: %s' % (name, e))
            continue
        if os.path.basename(f).upper() == 'WEAPTWK.XOM' and not edit_test(f):
            ok = False
        nobj += n
        rawtypes.update(raw)
        if not raw:
            fully += 1
        if ok:
            npass += 1
            if a.verbose:
                print('ok     %s (%d objects%s)' % (name, n, ', %d opaque' % sum(raw.values()) if raw else ''))
        else:
            nfail += 1
            i = next((k for k in range(min(len(orig), len(out))) if orig[k] != out[k]), min(len(orig), len(out)))
            print('FAIL   %s: first difference at %#x (sizes %d vs %d)' % (name, i, len(orig), len(out)))
    total = len(files)
    print()
    print('byte-identical round trip: %d/%d files  (%d mismatched, %d errors)' % (npass, total, nfail, nerr))
    print('fully decoded (no opaque objects): %d/%d files, %d objects total, %d opaque'
          % (fully, total - nerr, nobj, sum(rawtypes.values())))
    for t, c in rawtypes.most_common():
        print('  opaque: %-40s %d' % (t, c))
    return 0 if npass == total else 1


if __name__ == '__main__':
    sys.exit(main())
