#!/usr/bin/env python3
"""
xomtool.py - the Sieve CLI, Python parity build (stdlib only). Mirrors tools/xomtool/main.cpp
(src/xom's C++ port) command for command; see docs/xomtool.md.

Exit codes: 0 ok, 1 usage, 2 input error, 3 write refused.
"""
import copy
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import xom  # noqa: E402
import ximage  # noqa: E402
import gltf  # noqa: E402

USAGE = """usage:
  xomtool.py unpack <in.xom|.xan> [-o <out.json>] [--split <dir>]
  xomtool.py pack <in.json|dir> <out.xom>
  xomtool.py inspect <in.xom> [--object <NAME|#N>] [--type <TypeName>]
  xomtool.py diff <a.xom> <b.xom>
  xomtool.py convert <texture.png> --into <file.xom> --as <Name> [--section N] [--mips=0|1] [-o <out.xom>]
  xomtool.py convert <Name> --from <file.xom> --out <texture.png> [--mip N]
  xomtool.py convert <mesh.gltf|.glb> --into <file.xom> --as <Name> [--section N]
                     [--material-from <Name>] [--material-file <file.xom>] [--texture <png>] [-o <out.xom>]
  xomtool.py convert <Name> --from <file.xom> --out <mesh.gltf>
  xomtool.py bank --from <src.xom> --object <BaseName> --as <NewName> [--set Field=value ...] --out <out.xom>
  xomtool.py report <in.xom> -o <out.md>
"""


class Args:
    def __init__(self, argv):
        self.positional = []
        self.flags = {}
        self.sets = []
        i = 0
        while i < len(argv):
            a = argv[i]
            if a.startswith('--'):
                name = a[2:]
                if '=' in name:
                    k, v = name.split('=', 1)
                    self.flags[k] = v
                elif name == 'set' and i + 1 < len(argv):
                    i += 1
                    self.sets.append(argv[i])
                elif i + 1 < len(argv) and not argv[i + 1].startswith('-'):
                    i += 1
                    self.flags[name] = argv[i]
                else:
                    self.flags[name] = '1'
            elif a == '-o' and i + 1 < len(argv):
                i += 1
                self.flags['o'] = argv[i]
            else:
                self.positional.append(a)
            i += 1

    def get(self, name, default=''):
        return self.flags.get(name, default)

    def has(self, name):
        return name in self.flags


def err(msg):
    sys.stderr.write('xomtool: %s\n' % msg)


def usage():
    sys.stderr.write(USAGE)
    return 1


def load_doc(path):
    return xom.load(path)


def save_doc(path, doc):
    xom.save(doc, path)


# ---------------------------------------------------------------- unpack / pack

def cmd_unpack(a):
    if not a.positional:
        return usage()
    try:
        doc = load_doc(a.positional[0])
    except (xom.XomError, OSError) as e:
        err(str(e))
        return 2
    text = xom.to_json(doc)
    if a.has('split'):
        d = a.get('split')
        os.makedirs(os.path.join(d, 'objects'), exist_ok=True)
        index = {k: v for k, v in doc.items() if k != 'objects'}
        obj_index = []
        for i, o in enumerate(doc['objects']):
            fname = 'objects/%06d.json' % i
            with open(os.path.join(d, fname), 'w', encoding='utf-8', newline='\n') as f:
                f.write(json.dumps(o, indent=1, ensure_ascii=False))
            obj_index.append({'type': o['type'], 'file': fname})
        index['objects'] = obj_index
        with open(os.path.join(d, 'index.json'), 'w', encoding='utf-8', newline='\n') as f:
            f.write(json.dumps(index, indent=1, ensure_ascii=False))
        print('wrote %s/index.json and %d object file(s)' % (d, len(doc['objects'])))
        return 0
    if a.has('o'):
        with open(a.get('o'), 'w', encoding='utf-8', newline='\n') as f:
            f.write(text)
    else:
        sys.stdout.write(text)
    return 0


def cmd_pack(a):
    if len(a.positional) < 2:
        return usage()
    src, out = a.positional[0], a.positional[1]
    try:
        if os.path.isdir(src):
            with open(os.path.join(src, 'index.json'), encoding='utf-8') as f:
                index = json.load(f)
            objects = []
            for entry in index['objects']:
                with open(os.path.join(src, entry['file']), encoding='utf-8') as f:
                    objects.append(json.load(f))
            index['objects'] = objects
            doc = index
        else:
            with open(src, encoding='utf-8') as f:
                doc = json.load(f)
    except (xom.XomError, OSError, KeyError, ValueError) as e:
        err(str(e))
        return 2
    try:
        save_doc(out, doc)
    except xom.XomError as e:
        err(str(e))
        return 3
    except (OSError, KeyError, ValueError) as e:
        err(str(e))
        return 2
    print('wrote %s' % out)
    return 0


# ---------------------------------------------------------------- inspect

def _field_summary(o):
    f = o.get('fields', {})
    if o['type'] == 'XImage' and 'Width' in f:
        return '%dx%d format=%d mips=%d' % (f['Width'], f['Height'], f.get('Format', 0), f.get('MipLevels', 1))
    if o['type'] == 'XIndexedTriangleSet' and 'PrimitiveCount' in f:
        return '%d triangles' % f['PrimitiveCount']
    if o['type'] in ('XMeshDescriptor', 'XBaseResourceDescriptor', 'XBitmapDescriptor') and 'ResourceId' in f:
        return 'ResourceId=%s' % f['ResourceId']
    return ''


def cmd_inspect(a):
    if not a.positional:
        return usage()
    try:
        doc = load_doc(a.positional[0])
    except (xom.XomError, OSError) as e:
        err(str(e))
        return 2
    want_type = a.get('type')
    want_object = a.get('object')
    if not want_object and not want_type:
        print('types: ' + ', '.join('%s v%d x%d' % (t.get('class', t['name']), t['version'], t['count']) for t in doc['types']))
        print('root: #%d  strings: %d  objects: %d' % (doc['root'], len(doc['strings']), len(doc['objects'])))
        return 0
    for i, o in enumerate(doc['objects']):
        match = True
        if want_type:
            match = o['type'] == want_type
        if match and want_object:
            if want_object.startswith('#'):
                match = str(i + 1) == want_object[1:]
            else:
                f = o.get('fields', {})
                match = f.get('Name') == want_object or f.get('ResourceId') == want_object
        if not match:
            continue
        sum_ = _field_summary(o)
        print('#%d %s%s%s' % (i + 1, o['type'], '  ' if sum_ else '', sum_))
        if want_object and 'fields' in o:
            for k, v in o['fields'].items():
                print('    %s = %s' % (k, json.dumps(v, indent=1, ensure_ascii=False)))
    return 0


# ---------------------------------------------------------------- diff

def _json_diff(path, a, b, out):
    if type(a) is not type(b) and not (isinstance(a, (int, float)) and isinstance(b, (int, float))):
        out.append('%s: %s -> %s' % (path, json.dumps(a), json.dumps(b)))
        return
    if isinstance(a, dict):
        keys = list(a.keys()) + [k for k in b.keys() if k not in a]
        for k in keys:
            p = k if not path else path + '.' + k
            if k not in a:
                out.append('%s: (absent) -> %s' % (p, json.dumps(b[k])))
            elif k not in b:
                out.append('%s: %s -> (absent)' % (p, json.dumps(a[k])))
            else:
                _json_diff(p, a[k], b[k], out)
    elif isinstance(a, list):
        n = max(len(a), len(b))
        for i in range(n):
            p = '%s[%d]' % (path, i)
            if i >= len(a):
                out.append('%s: (absent) -> %s' % (p, json.dumps(b[i])))
            elif i >= len(b):
                out.append('%s: %s -> (absent)' % (p, json.dumps(a[i])))
            else:
                _json_diff(p, a[i], b[i], out)
    else:
        if json.dumps(a) != json.dumps(b):
            out.append('%s: %s -> %s' % (path, json.dumps(a), json.dumps(b)))


def cmd_diff(a):
    if len(a.positional) < 2:
        return usage()
    try:
        da, db = load_doc(a.positional[0]), load_doc(a.positional[1])
    except (xom.XomError, OSError) as e:
        err(str(e))
        return 2
    oa, ob = da['objects'], db['objects']
    n = max(len(oa), len(ob))
    diffs = 0
    for i in range(n):
        label = '#%d' % (i + 1)
        if i < len(oa):
            f = oa[i].get('fields', {})
            if f.get('ResourceId'):
                label += ' ' + f['ResourceId']
            elif isinstance(f.get('Name'), str):
                label += ' ' + f['Name']
        if i >= len(oa):
            print('%s: only in b' % label)
            diffs += 1
            continue
        if i >= len(ob):
            print('%s: only in a' % label)
            diffs += 1
            continue
        lines = []
        _json_diff('', oa[i], ob[i], lines)
        for l in lines:
            print('%s %s' % (label, l))
            diffs += 1
    print('%d difference(s)' % diffs)
    return 0


# ---------------------------------------------------------------- convert (texture)

def _find_image_by_name(doc, name):
    for o in doc['objects']:
        if o['type'] == 'XImage' and o.get('fields', {}).get('Name') == name:
            return o
    return None


def cmd_convert_texture_in(a, png_path):
    into, as_name = a.get('into'), a.get('as')
    if not into or not as_name:
        err('convert <png> needs --into and --as')
        return 1
    try:
        with open(png_path, 'rb') as f:
            png_bytes = f.read()
        fields = ximage.png_to_ximage_fields(png_bytes, as_name, generate_mips=a.get('mips', '1') != '0')
        doc = load_doc(into)
    except (xom.XomError, OSError) as e:
        err(str(e))
        return 2
    img = {'type': 'XImage', 'iflags': 0, 'uflags': 0, 'dxcount': 0, 'fields': {'Name': as_name, **fields}}
    existing_pos = next((i for i, t in enumerate(doc['types']) if t.get('class', t['name']) == 'XImage'), None)
    if existing_pos is None:
        cls = xom.schema()['classes']['XImage']
        doc['types'].append({'name': 'XImage', 'version': 0, 'count': 0, 'guid': cls['guid']})
        doc['objects'].append(img)
    else:
        type_order = [t.get('class', t['name']) for t in doc['types']]
        insert_at = len(doc['objects'])
        for i, o in enumerate(doc['objects']):
            if type_order.index(o['type']) > existing_pos:
                insert_at = i
                break
        doc['objects'].insert(insert_at, img)
    out = a.get('o') or into
    try:
        save_doc(out, doc)
    except xom.XomError as e:
        err(str(e))
        return 3
    print('wrote %s (added XImage "%s")' % (out, as_name))
    return 0


def cmd_convert_texture_out(a, name):
    from_, out = a.get('from'), a.get('out')
    if not from_ or not out:
        err('convert <Name> --from needs --out')
        return 1
    try:
        doc = load_doc(from_)
        img = _find_image_by_name(doc, name)
        if img is None:
            err('no XImage named "%s" in %s' % (name, from_))
            return 2
        mip = int(a.get('mip', '0'))
        w, h, bpp, png_ready = ximage.extract_mip(img['fields'], mip)
        png_bytes = ximage.png_codec.encode(w, h, bpp, png_ready)
        with open(out, 'wb') as f:
            f.write(png_bytes)
    except (xom.XomError, OSError, ValueError) as e:
        err(str(e))
        return 2
    print('wrote %s (%dx%d, %d channels)' % (out, w, h, bpp))
    return 0


# ---------------------------------------------------------------- convert (mesh)

def _ext(path):
    return os.path.splitext(path)[1][1:].lower()


def cmd_convert_mesh_in(a, gltf_path):
    into, as_name = a.get('into'), a.get('as')
    if not into or not as_name:
        err('convert <mesh> needs --into and --as')
        return 1
    try:
        with open(gltf_path, 'rb') as f:
            file_bytes = f.read()
        prims = gltf.read_gltf(file_bytes, _ext(gltf_path) == 'glb', os.path.dirname(gltf_path))
        doc = load_doc(into)
        shader_ref = 0
        if a.has('material-from'):
            mat_doc = load_doc(a.get('material-file')) if a.has('material-file') else doc
            src_shader_ref = gltf.find_mesh_shader(mat_doc, a.get('material-from'))
            src_shader_ref = src_shader_ref['ref'] if isinstance(src_shader_ref, dict) else src_shader_ref
            shader_ref = gltf.copy_subgraph(doc, mat_doc, src_shader_ref)
            if a.has('texture'):
                tex_ref = gltf.find_shader_texture(doc, shader_ref)
                tex_ref = tex_ref['ref'] if isinstance(tex_ref, dict) else tex_ref
                with open(a.get('texture'), 'rb') as f:
                    png_bytes = f.read()
                fields = ximage.png_to_ximage_fields(png_bytes, doc['objects'][tex_ref - 1]['fields'].get('Name', as_name))
                keep_name = doc['objects'][tex_ref - 1]['fields'].get('Name')
                fields['Name'] = keep_name
                doc['objects'][tex_ref - 1]['fields'].update(fields)
        section = int(a.get('section', '0'))
        gltf.write_mesh(doc, prims, shader_ref, as_name, section)
        out = a.get('o') or into
        save_doc(out, doc)
    except (xom.XomError, OSError, KeyError, ValueError) as e:
        err(str(e))
        return 2
    print('wrote %s (added XMeshDescriptor "%s", %d primitive(s))' % (out, as_name, len(prims)))
    return 0


def cmd_convert_mesh_out(a, name):
    from_, out = a.get('from'), a.get('out')
    if not from_ or not out:
        err('convert <Name> --from needs --out')
        return 1
    try:
        doc = load_doc(from_)
        prims = gltf.read_mesh(doc, name)
        bin_name = os.path.splitext(os.path.basename(out))[0] + '.bin'
        json_text, bin_bytes = gltf.write_gltf(prims, bin_name)
        with open(out, 'w', encoding='utf-8', newline='\n') as f:
            f.write(json_text)
        bin_path = os.path.join(os.path.dirname(out) or '.', bin_name)
        with open(bin_path, 'wb') as f:
            f.write(bin_bytes)
    except (xom.XomError, OSError) as e:
        err(str(e))
        return 2
    print('wrote %s and %s (%d primitive(s))' % (out, bin_path, len(prims)))
    return 0


def cmd_convert(a):
    if not a.positional:
        return usage()
    first = a.positional[0]
    ext = _ext(first)
    if ext == 'png':
        return cmd_convert_texture_in(a, first) if a.has('into') else cmd_convert_texture_out(a, first)
    if ext in ('gltf', 'glb'):
        if a.has('into'):
            return cmd_convert_mesh_in(a, first)
        err('convert <mesh> --into is the only mesh-import form')
        return 1
    if a.has('out'):
        out_ext = _ext(a.get('out'))
        if out_ext == 'png':
            return cmd_convert_texture_out(a, first)
        if out_ext == 'gltf':
            return cmd_convert_mesh_out(a, first)
    err('convert: cannot tell texture from mesh here; name a .png/.gltf/.glb file')
    return 1


# ---------------------------------------------------------------- bank

def cmd_bank(a):
    from_, obj_name, as_name, out = a.get('from'), a.get('object'), a.get('as'), a.get('out')
    if not (from_ and obj_name and as_name and out):
        err('bank needs --from --object --as --out')
        return 1
    try:
        doc = load_doc(from_)
    except (xom.XomError, OSError) as e:
        err(str(e))
        return 2
    bank = next((o for o in doc['objects'] if o['type'] == 'XDataBank'), None)
    if bank is None:
        err('%s has no XDataBank' % from_)
        return 2
    template_detail, base_ref = None, None
    for r in bank['fields'].get('ContainerResources', []):
        det = xom.obj(doc, r)
        if det and det['fields'].get('Name') == obj_name:
            template_detail = det
            base_ref = det['fields']['Value']
            break
    if template_detail is None:
        err('no container resource named "%s" in %s' % (obj_name, from_))
        return 2
    base_ref = base_ref['ref'] if isinstance(base_ref, dict) else base_ref
    cont = copy.deepcopy(xom.obj(doc, base_ref))
    cls = cont['type']
    for kv in a.sets:
        if '=' not in kv:
            err('--set needs Field=value (got "%s")' % kv)
            return 1
        field, val = kv.split('=', 1)
        if field not in cont['fields']:
            err('%s has no field "%s"' % (cls, field))
            return 2
        cur = cont['fields'][field]
        if isinstance(cur, bool):
            cont['fields'][field] = val in ('1', 'true')
        elif isinstance(cur, float):
            cont['fields'][field] = float(val)
        elif isinstance(cur, int):
            cont['fields'][field] = int(val)
        elif isinstance(cur, str):
            cont['fields'][field] = val
        else:
            err('field "%s" has a type --set cannot write' % field)
            return 2

    out_doc = {'format': doc['format'], 'header': doc['header'], 'types': copy.deepcopy(doc['types']),
               'guid_rec': doc['guid_rec'], 'schm_rec': doc['schm_rec'], 'strings': doc['strings'], 'root': 2}
    detail = copy.deepcopy(template_detail)
    detail['fields']['Name'] = as_name
    detail['fields']['Value'] = {'ref': 3}
    bank_copy = copy.deepcopy(bank)
    for k, v in bank_copy['fields'].items():
        bank_copy['fields'][k] = [{'ref': 1}] if k == 'ContainerResources' else ([] if isinstance(v, list) else v)
    out_doc['objects'] = [detail, bank_copy, cont]
    for t in out_doc['types']:
        t['count'] = 0
    for o in out_doc['objects']:
        for t in out_doc['types']:
            if t.get('class', t['name']) == o['type']:
                t['count'] += 1
    try:
        save_doc(out, out_doc)
    except xom.XomError as e:
        err(str(e))
        return 3
    print('wrote %s: %s/%s/%s, root #%d' % (out, detail['type'], bank_copy['type'], cont['type'], out_doc['root']))
    return 0


# ---------------------------------------------------------------- report

def cmd_report(a):
    if not a.positional or not a.has('o'):
        err('report <in.xom> -o <out.md>')
        return 1
    try:
        doc = load_doc(a.positional[0])
    except (xom.XomError, OSError) as e:
        err(str(e))
        return 2
    bank = xom.obj(doc, doc['root'])
    lines = ['# %s' % a.positional[0], '', 'Generated by `xomtool.py report`.', '']
    if bank and bank['type'] == 'XDataBank':
        lines += ['| Name | Class | Fields set |', '|---|---|---|']
        for r in bank['fields'].get('ContainerResources', []):
            det = xom.obj(doc, r)
            if not det:
                continue
            cont = xom.obj(doc, det['fields']['Value'])
            if not cont:
                continue
            lines.append('| `%s` | %s | %d |' % (det['fields']['Name'], cont['type'], len(cont['fields'])))
    else:
        lines.append('Object types: ' + ', '.join('%s x%d' % (t.get('class', t['name']), t['count']) for t in doc['types']))
    with open(a.get('o'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    print('wrote %s' % a.get('o'))
    return 0


def main(argv):
    if len(argv) < 2:
        return usage()
    cmd = argv[1]
    a = Args(argv[2:])
    if cmd == 'unpack':
        return cmd_unpack(a)
    if cmd == 'pack':
        return cmd_pack(a)
    if cmd == 'inspect':
        return cmd_inspect(a)
    if cmd == 'diff':
        return cmd_diff(a)
    if cmd == 'convert':
        return cmd_convert(a)
    if cmd == 'bank':
        return cmd_bank(a)
    if cmd == 'report':
        return cmd_report(a)
    return usage()


if __name__ == '__main__':
    sys.exit(main(sys.argv))
