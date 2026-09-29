#!/usr/bin/env python3
"""
parity_test.py - runs the C++ xomtool and the Python xomtool.py over every shipped XOM/XAN and
the conversion fixtures, and compares their outputs.

    python parity_test.py --game <WormsXHD dir> --xomtool <path to xomtool.exe> [--bundles] [--maps]

`unpack` and `bank` are compared byte for byte (both are pure melange-xom/1 logic, under this
tool's full control in both languages). `convert`'s image output is compared pixel for pixel,
not byte for byte: stb_image_write (C++) and png_codec.py's zlib-based encoder produce different
(but both valid, losslessly decodable) PNG bytes for the same pixels - matching compressed bytes
across two different encoders is not a meaningful goal. The mesh (glTF) and XOM-injection paths
are additionally checked byte for byte, since both languages build them with the same
deterministic layout logic (no third-party codec involved).
"""
import argparse
import glob
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import xom  # noqa: E402
import png_codec  # noqa: E402


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


def run_cpp(xomtool_exe, args):
    r = subprocess.run([xomtool_exe] + args, capture_output=True)
    return r.returncode, r.stdout, r.stderr


def run_py(args):
    r = subprocess.run([sys.executable, os.path.join(HERE, 'xomtool.py')] + args, capture_output=True)
    return r.returncode, r.stdout, r.stderr


def check_unpack_parity(xomtool_exe, files):
    ok = fail = 0
    with tempfile.TemporaryDirectory() as td:
        cpp_json = os.path.join(td, 'cpp.json')
        py_json = os.path.join(td, 'py.json')
        for f in files:
            rc, _, e = run_cpp(xomtool_exe, ['unpack', f, '-o', cpp_json])
            if rc != 0:
                print('CPP unpack FAIL %s: %s' % (f, e.decode(errors='replace')))
                fail += 1
                continue
            rc, _, e = run_py(['unpack', f, '-o', py_json])
            if rc != 0:
                print('PY unpack FAIL %s: %s' % (f, e.decode(errors='replace')))
                fail += 1
                continue
            with open(cpp_json, 'rb') as fh:
                a = fh.read()
            with open(py_json, 'rb') as fh:
                b = fh.read()
            if a == b:
                ok += 1
            else:
                print('UNPACK JSON DIFFERS: %s' % f)
                fail += 1
    print('unpack parity: %d/%d byte-identical' % (ok, ok + fail))
    return fail == 0


def check_bank_parity(xomtool_exe, weaptwk, outdir):
    cpp_out = os.path.join(outdir, 'bank_cpp.xom')
    py_out = os.path.join(outdir, 'bank_py.xom')
    args = ['bank', '--from', weaptwk, '--object', 'kWeaponBazooka', '--as', 'kWeaponMegaBazooka',
            '--set', 'WormDamageMagnitude=120']
    rc, _, e = run_cpp(xomtool_exe, args + ['--out', cpp_out])
    if rc != 0:
        print('CPP bank FAIL: %s' % e.decode(errors='replace'))
        return False
    rc, _, e = run_py(args + ['--out', py_out])
    if rc != 0:
        print('PY bank FAIL: %s' % e.decode(errors='replace'))
        return False
    with open(cpp_out, 'rb') as f:
        a = f.read()
    with open(py_out, 'rb') as f:
        b = f.read()
    same = a == b
    print('bank parity: %s (%d bytes)' % ('byte-identical' if same else 'DIFFERS', len(a)))
    return same


def check_mesh_parity(xomtool_exe, bundle416, weaptwk_bank, outdir):
    # Same base name in each half's own subdirectory: the glTF embeds its buffer's file name, so
    # a differing output name would make the JSON differ for a reason that has nothing to do
    # with parity.
    cpp_dir = os.path.join(outdir, 'cpp')
    py_dir = os.path.join(outdir, 'py')
    os.makedirs(cpp_dir, exist_ok=True)
    os.makedirs(py_dir, exist_ok=True)
    cpp_gltf = os.path.join(cpp_dir, 'shell.gltf')
    py_gltf = os.path.join(py_dir, 'shell.gltf')
    rc, _, e = run_cpp(xomtool_exe, ['convert', 'Factory.Proj.Bazookashell', '--from', bundle416, '--out', cpp_gltf])
    if rc != 0:
        print('CPP convert (mesh out) FAIL: %s' % e.decode(errors='replace'))
        return False
    rc, _, e = run_py(['convert', 'Factory.Proj.Bazookashell', '--from', bundle416, '--out', py_gltf])
    if rc != 0:
        print('PY convert (mesh out) FAIL: %s' % e.decode(errors='replace'))
        return False
    same_json = open(cpp_gltf, 'rb').read() == open(py_gltf, 'rb').read()
    same_bin = (open(os.path.splitext(cpp_gltf)[0] + '.bin', 'rb').read()
                == open(os.path.splitext(py_gltf)[0] + '.bin', 'rb').read())
    print('mesh (XOM -> glTF) parity: json %s, bin %s' %
          ('byte-identical' if same_json else 'DIFFERS', 'byte-identical' if same_bin else 'DIFFERS'))

    cpp_xom = os.path.join(outdir, 'mesh_in_cpp.xom')
    py_xom = os.path.join(outdir, 'mesh_in_py.xom')
    mesh_args = ['convert', cpp_gltf, '--into', weaptwk_bank, '--as', 'mega-bazooka.Payload',
                 '--material-from', 'Factory.Proj.Bazookashell', '--material-file', bundle416]
    rc, _, e = run_cpp(xomtool_exe, mesh_args + ['-o', cpp_xom])
    if rc != 0:
        print('CPP convert (mesh in) FAIL: %s' % e.decode(errors='replace'))
        return False
    rc, _, e = run_py(mesh_args + ['-o', py_xom])
    if rc != 0:
        print('PY convert (mesh in) FAIL: %s' % e.decode(errors='replace'))
        return False
    same_xom = open(cpp_xom, 'rb').read() == open(py_xom, 'rb').read()
    print('mesh (glTF -> XOM) parity: %s' % ('byte-identical' if same_xom else 'DIFFERS'))
    return same_json and same_bin and same_xom


def check_texture_parity(xomtool_exe, bundle09, outdir):
    cpp_png = os.path.join(outdir, 'icon_cpp.png')
    py_png = os.path.join(outdir, 'icon_py.png')
    rc, _, e = run_cpp(xomtool_exe, ['convert', 'Weapon Panel Icons1.tga', '--from', bundle09, '--out', cpp_png])
    if rc != 0:
        print('CPP convert (texture out) FAIL: %s' % e.decode(errors='replace'))
        return False
    rc, _, e = run_py(['convert', 'Weapon Panel Icons1.tga', '--from', bundle09, '--out', py_png])
    if rc != 0:
        print('PY convert (texture out) FAIL: %s' % e.decode(errors='replace'))
        return False
    a = png_codec.decode(open(cpp_png, 'rb').read())
    b = png_codec.decode(open(py_png, 'rb').read())
    same = a == b
    print('texture (XOM -> PNG) parity: pixel-identical %s (PNG bytes intentionally differ - see module docstring)'
          % same)
    return same


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', required=True)
    ap.add_argument('--xomtool', required=True, help='path to the built xomtool.exe')
    ap.add_argument('--bundles', action='store_true')
    ap.add_argument('--maps', action='store_true')
    args = ap.parse_args()

    files = collect(args.game, args.bundles, args.maps)
    print('checking %d file(s)' % len(files))
    ok = check_unpack_parity(args.xomtool, files)

    with tempfile.TemporaryDirectory() as td:
        weaptwk = os.path.join(args.game, 'Data', 'Tweak', 'WEAPTWK.XOM')
        bundle09 = os.path.join(args.game, 'Data', 'Bundles', 'Bundl09.xom')
        bundle416 = os.path.join(args.game, 'Data', 'Bundles', 'Bundl416.xom')
        ok &= check_bank_parity(args.xomtool, weaptwk, td)
        bank_out = os.path.join(td, 'bank_cpp.xom')
        if os.path.exists(bank_out):
            ok &= check_mesh_parity(args.xomtool, bundle416, bank_out, td)
        ok &= check_texture_parity(args.xomtool, bundle09, td)

    print('\nPARITY %s' % ('OK' if ok else 'FAILED'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
