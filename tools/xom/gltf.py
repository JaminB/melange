"""
gltf.py - static mesh XOM <-> glTF 2.0, mirroring src/xom/{mesh,gltf}.{h,cpp}.

Geometry only (positions/normals/UV0/indices and node transforms); no materials, images, skins
or animation in the glTF JSON itself - XShape.Shader material handling is --material-from, a
deep copy of an existing shader subgraph (see copy_subgraph()), not glTF material JSON.

One primitive per XShape; u16 indices only when writing an XOM mesh back out is not enforced
here (this module always emits u32 index accessors on the glTF side and u16 on the XOM side, as
src/xom/mesh.cpp does).
"""
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xom  # noqa: E402

# XGraphSet/XMeshDescriptor are hand-written (non-CTNR) classes with no FieldDef list, so they
# are absent from schema.json. GUIDs read from real content (Data/Bundles/Bundl416.xom's TYPE
# table, 2026-09-29), the same values src/xom/mesh.cpp hardcodes.
_GRAPHSET_GUID = '0b3dbf644139bb40b1798f882d14449b'
_MESHDESC_GUID = 'dbb2e8a8c30af04ba47696f47cf924d2'


def identity():
    return [1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0]


def mat_mul(a, b):
    r = [0.0] * 16
    for col in range(4):
        for row in range(4):
            s = 0.0
            for k in range(4):
                s += a[k * 4 + row] * b[col * 4 + k]
            r[col * 4 + row] = s
    return r


# ---------------------------------------------------------------- XOM -> primitives

def _transform_matrix(doc, xf_ref):
    if not xf_ref:
        return identity()
    xf = xom.obj(doc, xf_ref)
    if xf is None or xf['type'] != 'XTransform':
        return identity()
    m = xf['fields']['Matrix']  # 12 floats: Xaxis, Yaxis, Zaxis, Translation (see mesh.cpp)
    r = identity()
    for col in range(4):
        r[col * 4 + 0] = m[col * 3 + 0]
        r[col * 4 + 1] = m[col * 3 + 1]
        r[col * 4 + 2] = m[col * 3 + 2]
        r[col * 4 + 3] = 1.0 if col == 3 else 0.0
    return r


def _walk(doc, ref, accum, out):
    if not ref:
        return
    o = xom.obj(doc, ref)
    if o is None:
        return
    t = o['type']
    if t == 'XShape':
        geom = xom.obj(doc, o['fields']['Geometry'])
        idx = xom.obj(doc, geom['fields']['IndexSet'])
        co = xom.obj(doc, geom['fields']['CoordSet'])
        no_ref = geom['fields'].get('NormalSet')
        uv_ref = geom['fields'].get('TexCoordSet')
        no = xom.obj(doc, no_ref) if no_ref else None
        uv = xom.obj(doc, uv_ref) if uv_ref else None
        out.append({
            'name': o['fields'].get('Name', ''),
            'matrix': accum,
            'positions': [tuple(c) for c in co['fields']['Coord']],
            'normals': [tuple(c) for c in no['fields']['Normal']] if no else [],
            'uvs': [tuple(c) for c in uv['fields']['TexCoord']] if uv else [],
            'indices': list(idx['fields']['Index']),
        })
        return
    if t == 'XGroup':
        local = _transform_matrix(doc, o['fields']['Core'])
        nxt = mat_mul(accum, local)
        for c in o['fields'].get('Children', []):
            _walk(doc, c, nxt, out)
        return
    if t == 'XInteriorNode':
        for c in o['fields'].get('Children', []):
            _walk(doc, c, accum, out)
        return


def read_mesh(doc, resource_id):
    """Returns a list of primitive dicts (positions/normals/uvs/indices/matrix/name) for the
    mesh named `resource_id`, walking its "world" graph entry (or the first entry, if none is
    named "world")."""
    desc = None
    for o in doc['objects']:
        if o['type'] == 'XMeshDescriptor' and o.get('fields', {}).get('ResourceId') == resource_id:
            desc = o
            break
    if desc is None:
        raise xom.XomError('no XMeshDescriptor named %s' % resource_id)
    gs = xom.obj(doc, desc['fields']['GraphSet'])
    graphs = gs['fields']['Graphs']
    if not graphs:
        raise xom.XomError('%s: XGraphSet has no graph entries' % resource_id)
    entry = next((g for g in graphs if g['Name'] == 'world'), graphs[0])
    out = []
    _walk(doc, entry['Graph'], identity(), out)
    if not out:
        raise xom.XomError('%s: no XShape found in its "world" graph' % resource_id)
    return out


def find_mesh_shader(doc, resource_id):
    desc = next((o for o in doc['objects']
                 if o['type'] == 'XMeshDescriptor' and o.get('fields', {}).get('ResourceId') == resource_id), None)
    if desc is None:
        raise xom.XomError('no XMeshDescriptor named %s' % resource_id)
    gs = xom.obj(doc, desc['fields']['GraphSet'])
    graphs = gs['fields']['Graphs']
    entry = next((g for g in graphs if g['Name'] == 'world'), graphs[0])
    stack = [entry['Graph']]
    seen = set()
    while stack:
        ref = stack.pop()
        n = ref['ref'] if isinstance(ref, dict) else ref
        if not n or n in seen:
            continue
        seen.add(n)
        o = doc['objects'][n - 1]
        if o['type'] == 'XShape':
            sh = o['fields'].get('Shader')
            if sh and (sh['ref'] if isinstance(sh, dict) else sh):
                return sh
            continue
        stack.extend(o.get('fields', {}).get('Children', []))
    raise xom.XomError('%s: no XShape with a Shader was found' % resource_id)


def find_shader_texture(doc, shader_ref):
    shader = xom.obj(doc, shader_ref)
    if shader is None or shader['type'] != 'XSimpleShader':
        raise xom.XomError('--material-from\'s Shader is not an XSimpleShader')
    found = []
    for stage_ref in shader['fields'].get('TextureStages', []):
        stage = xom.obj(doc, stage_ref)
        if stage is None or stage['type'] != 'XOglTextureMap':
            continue
        tex_ref = stage['fields'].get('Texture')
        img = xom.obj(doc, tex_ref) if tex_ref else None
        if img is not None and img['type'] == 'XImage':
            found.append(tex_ref)
    if len(found) != 1:
        raise xom.XomError('expected exactly one texture on the template shader, found %d' % len(found))
    return found[0]


# ---------------------------------------------------------------- ref closure (copy)

def _collect_refs(v, out):
    if isinstance(v, dict):
        if set(v.keys()) == {'ref'}:
            if v['ref']:
                out.append(v['ref'])
            return
        for x in v.values():
            _collect_refs(x, out)
    elif isinstance(v, list):
        for x in v:
            _collect_refs(x, out)


def _remap_refs(v, m):
    if isinstance(v, dict):
        if set(v.keys()) == {'ref'}:
            return {'ref': m.get(v['ref'], 0)}
        return {k: _remap_refs(x, m) for k, x in v.items()}
    if isinstance(v, list):
        return [_remap_refs(x, m) for x in v]
    return v


def copy_subgraph(dst, src, src_ref):
    """Deep-copies the object subgraph reachable from `src_ref` in `src` into `dst`, grouped by
    TYPE-table order (adding TYPE entries, and their ancestor classes' entries, as needed).
    Returns the new ref within `dst`."""
    import copy as _copy
    order, seen, todo = [], set(), [src_ref]
    seen.add(src_ref)
    while todo:
        n = todo.pop()
        order.append(n)
        o = src['objects'][n - 1]
        if o.get('in_tail'):
            raise xom.XomError('copy: an object in the undelimited tail cannot be copied')
        refs = []
        _collect_refs(o.get('fields', {}), refs)
        for r in refs:
            if r not in seen:
                seen.add(r)
                todo.append(r)
    type_order = [t.get('class', t['name']) for t in src['types']]
    order.sort(key=lambda n: type_order.index(src['objects'][n - 1]['type']))
    remap = {}
    next_idx = len(dst['objects']) + 1
    for old in order:
        remap[old] = next_idx
        next_idx += 1

    def dst_class_names():
        return {t.get('class', t['name']) for t in dst['types']}

    def ancestor_chain(cls):
        chain = []
        classes = xom.schema()['classes']
        while cls is not None:
            chain.append(cls)
            cls = classes.get(cls, {}).get('parent')
        return chain

    for old in order:
        cls = src['objects'][old - 1]['type']
        for c in ([cls] + ancestor_chain(cls)) if cls in xom.schema()['classes'] else [cls]:
            if c in dst_class_names():
                continue
            srcT = next((t for t in src['types'] if t.get('class', t['name']) == c), None)
            if srcT is None:
                raise xom.XomError('copy: class not in the source TYPE table: %s' % c)
            nt = _copy.deepcopy(srcT)
            nt['count'] = 0
            dst['types'].append(nt)
    for old in order:
        o = _copy.deepcopy(src['objects'][old - 1])
        o['fields'] = _remap_refs(o.get('fields', {}), remap)
        dst['objects'].append(o)
    return remap[src_ref]


# ---------------------------------------------------------------- primitives -> XOM

def write_mesh(doc, mesh, material_from_shader_ref, resource_id, section_id=0):
    """Appends a new mesh graph (mirrors src/xom/mesh.cpp's WriteMesh) to `doc` and returns the
    new XMeshDescriptor's object index. Refuses if `doc` already defines a class this needs to
    introduce (see mesh.cpp's note: this always builds a fresh, self-contained run of objects)."""
    needed = ['XTransform', 'XCoord3fSet', 'XNormal3fSet', 'XTexCoord2fSet', 'XIndexSet',
              'XIndexedTriangleSet', 'XShape', 'XGroup', 'XInteriorNode', 'XGraphSet', 'XMeshDescriptor']
    existing = {t.get('class', t['name']) for t in doc['types']}
    conflict = [c for c in needed if c in existing]
    if conflict:
        raise xom.XomError('the target file already defines %s; write_mesh only writes into a '
                            'file that does not yet use these classes' % conflict[0])

    def default_version(cls):
        return {'XShape': 4, 'XMaterial': 2, 'XGeometry': 2}.get(cls, 0)

    def require_chain(cls):
        classes = xom.schema()['classes']
        cur = cls
        while cur is not None:
            have = next((t for t in doc['types'] if t.get('class', t['name']) == cur), None)
            need = default_version(cur)
            if have is None:
                c = classes[cur]
                doc['types'].append({'name': cur, 'version': need, 'count': 0, 'guid': c['guid']})
            elif have['version'] < need:
                raise xom.XomError('%s: target file already declares this class at an older version' % cur)
            cur = classes.get(cur, {}).get('parent')

    for cls in needed:
        if cls == 'XGraphSet':
            doc['types'].append({'name': cls, 'version': 0, 'count': 0, 'guid': _GRAPHSET_GUID})
        elif cls == 'XMeshDescriptor':
            doc['types'].append({'name': cls, 'version': 0, 'count': 0, 'guid': _MESHDESC_GUID})
        else:
            require_chain(cls)

    def add(t, fields):
        doc['objects'].append({'type': t, 'iflags': 0, 'uflags': 0, 'dxcount': 0, 'fields': fields})
        return len(doc['objects'])

    def add_custom(t, fields):
        doc['objects'].append({'type': t, 'fields': fields})
        return len(doc['objects'])

    group_refs = []
    for p in mesh:
        if p['matrix'] != identity():
            m = p['matrix']
            col = [(m[c * 4], m[c * 4 + 1], m[c * 4 + 2]) for c in range(4)]
            sx = sum(x * x for x in col[0]) ** 0.5
            sy = sum(x * x for x in col[1]) ** 0.5
            sz = sum(x * x for x in col[2]) ** 0.5
            ref = add('XTransform', {
                'Translate': list(col[3]), 'Rotate': [0.0, 0.0, 0.0], 'Scale': [sx, sy, sz],
                'RotateOrder': 0, 'Matrix': list(col[0]) + list(col[1]) + list(col[2]) + list(col[3]),
                'Flags': 0,
            })
        else:
            ref = 0
        group_refs.append(ref)

    coord_refs, normal_refs, uv_refs, index_refs, tri_refs, shape_refs = [], [], [], [], [], []
    for p in mesh:
        coord_refs.append(add('XCoord3fSet', {'Coord': [list(v) for v in p['positions']]}))
    for p in mesh:
        normal_refs.append(add('XNormal3fSet', {'Normal': [list(v) for v in p['normals']]}))
    for p in mesh:
        uv_refs.append(add('XTexCoord2fSet', {'TexCoord': [list(v) for v in p['uvs']]}))
    for p in mesh:
        if max(p['indices'], default=0) > 65535:
            raise xom.XomError('%s: more than 65535 vertices (u16 indices only)' % p.get('name', ''))
        index_refs.append(add('XIndexSet', {'Index': list(p['indices'])}))
    for i, p in enumerate(mesh):
        tri_refs.append(add('XIndexedTriangleSet', {
            'IndexSet': {'ref': index_refs[i]}, 'Flags': 0, 'PrimitiveCount': len(p['indices']) // 3,
            'CoordSet': {'ref': coord_refs[i]}, 'NormalSet': {'ref': normal_refs[i] if p['normals'] else 0},
            'ColorSet': {'ref': 0}, 'TexCoordSet': {'ref': uv_refs[i] if p['uvs'] else 0}, 'WeightSet': {'ref': 0},
            'BoundBox': [0.0] * 6, 'BoundMode': 0, 'VertexShader': {'ref': 0},
        }))
    for i, p in enumerate(mesh):
        shape_refs.append(add('XShape', {
            'Flags': 0, 'Shader': {'ref': material_from_shader_ref}, 'Geometry': {'ref': tri_refs[i]},
            'SortKey': 0, 'Parameters': [], 'PreRenderFunc': {'ref': 0}, 'PostRenderFunc': {'ref': 0},
            'Bounds': [0.0, 0.0, 0.0, 0.0], 'BoundMode': 0, 'Name': p.get('name') or ('%s_%d' % (resource_id, i)),
        }))
    for i, p in enumerate(mesh):
        gref = add('XGroup', {
            'Core': {'ref': group_refs[i]}, 'Children': [{'ref': shape_refs[i]}],
            'Bounds': [0.0, 0.0, 0.0, 0.0], 'BoundMode': 0, 'Name': '%s_group_%d' % (resource_id, i),
        })
        group_refs[i] = gref
    root_ref = add('XInteriorNode', {
        'Children': [{'ref': r} for r in group_refs], 'Bounds': [0.0, 0.0, 0.0, 0.0], 'BoundMode': 0,
        'Name': resource_id,
    })
    gs_ref = add_custom('XGraphSet', {'Graphs': [{'Guid': '0' * 32, 'Graph': {'ref': root_ref}, 'Name': 'world'}]})
    desc_ref = add_custom('XMeshDescriptor', {
        'ResourceId': resource_id, 'SectionId': section_id, 'GraphSet': {'ref': gs_ref}, 'Flags': 8,
    })
    for t in doc['types']:
        t['count'] = 0
    for o in doc['objects']:
        for t in doc['types']:
            if t.get('class', t['name']) == o['type']:
                t['count'] += 1
    return desc_ref


# ---------------------------------------------------------------- glTF JSON/binary framing

def write_gltf(primitives, bin_file_name):
    bin_bytes = bytearray()

    def align4():
        while len(bin_bytes) % 4:
            bin_bytes.append(0)

    def push_floats(vals):
        start = len(bin_bytes)
        bin_bytes.extend(struct.pack('<%df' % len(vals), *vals))
        return start

    buffer_views, accessors, meshes, nodes, scene_nodes = [], [], [], [], []

    def add_bv(offset, length):
        buffer_views.append({'buffer': 0, 'byteOffset': offset, 'byteLength': length})
        return len(buffer_views) - 1

    for p in primitives:
        pos_flat = [c for v in p['positions'] for c in v]
        align4()
        off = push_floats(pos_flat)
        bv = add_bv(off, len(pos_flat) * 4)
        xs, ys, zs = [v[0] for v in p['positions']], [v[1] for v in p['positions']], [v[2] for v in p['positions']]
        acc = {'bufferView': bv, 'componentType': 5126, 'count': len(p['positions']), 'type': 'VEC3',
               'min': [min(xs), min(ys), min(zs)], 'max': [max(xs), max(ys), max(zs)]}
        pos_idx = len(accessors)
        accessors.append(acc)
        attrs = {'POSITION': pos_idx}

        if p['normals']:
            flat = [c for v in p['normals'] for c in v]
            align4()
            off = push_floats(flat)
            bv = add_bv(off, len(flat) * 4)
            attrs['NORMAL'] = len(accessors)
            accessors.append({'bufferView': bv, 'componentType': 5126, 'count': len(p['normals']), 'type': 'VEC3'})
        if p['uvs']:
            flat = [c for v in p['uvs'] for c in v]
            align4()
            off = push_floats(flat)
            bv = add_bv(off, len(flat) * 4)
            attrs['TEXCOORD_0'] = len(accessors)
            accessors.append({'bufferView': bv, 'componentType': 5126, 'count': len(p['uvs']), 'type': 'VEC2'})

        align4()
        off = len(bin_bytes)
        bin_bytes.extend(struct.pack('<%dI' % len(p['indices']), *p['indices']))
        bv = add_bv(off, len(p['indices']) * 4)
        idx_idx = len(accessors)
        accessors.append({'bufferView': bv, 'componentType': 5125, 'count': len(p['indices']), 'type': 'SCALAR'})

        mesh_idx = len(meshes)
        meshes.append({'name': p.get('name', ''), 'primitives': [{'attributes': attrs, 'indices': idx_idx, 'mode': 4}]})
        node = {'name': p.get('name', ''), 'mesh': mesh_idx}
        if p['matrix'] != identity():
            node['matrix'] = list(p['matrix'])
        nodes.append(node)
        scene_nodes.append(len(nodes) - 1)

    doc = {
        'asset': {'version': '2.0', 'generator': 'melange xomtool'},
        'buffers': [{'uri': bin_file_name, 'byteLength': len(bin_bytes)}],
        'bufferViews': buffer_views, 'accessors': accessors, 'meshes': meshes, 'nodes': nodes,
        'scenes': [{'nodes': scene_nodes}], 'scene': 0,
    }
    # indent=1/ensure_ascii=False matches src/xom/json.cpp's WriteJson exactly (it targets this
    # same Python convention), so the two languages' glTF JSON text stays byte-identical too.
    return json.dumps(doc, indent=1, ensure_ascii=False), bytes(bin_bytes)


_COMPONENT_FMT = {5120: 'b', 5121: 'B', 5122: 'h', 5123: 'H', 5125: 'I', 5126: 'f'}
_TYPE_COUNT = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4}


def _read_accessor(doc, bin_bytes, acc, expect_components):
    bv = doc['bufferViews'][acc['bufferView']]
    byte_offset = bv.get('byteOffset', 0) + acc.get('byteOffset', 0)
    nc = _TYPE_COUNT[acc['type']]
    if nc != expect_components:
        raise xom.XomError('accessor: unexpected type %s' % acc['type'])
    fmt = _COMPONENT_FMT[acc['componentType']]
    comp_size = struct.calcsize(fmt)
    stride = bv.get('byteStride') or comp_size * nc
    out = []
    for i in range(acc['count']):
        row = []
        for c in range(nc):
            off = byte_offset + i * stride + c * comp_size
            row.append(struct.unpack_from('<' + fmt, bin_bytes, off)[0])
        out.append(tuple(row) if nc > 1 else row[0])
    return out


def read_gltf(file_bytes, is_glb, bin_dir):
    if is_glb:
        if file_bytes[:4] != b'glTF':
            raise xom.XomError('not a .glb file')
        o = 12
        json_text, bin_bytes = None, b''
        while o + 8 <= len(file_bytes):
            length, chunk_type = struct.unpack_from('<II', file_bytes, o)
            o += 8
            chunk = file_bytes[o:o + length]
            if chunk_type == 0x4E4F534A:
                json_text = chunk.decode('utf-8')
            elif chunk_type == 0x004E4942:
                bin_bytes = chunk
            o += length
        if json_text is None:
            raise xom.XomError('.glb has no JSON chunk')
    else:
        json_text = file_bytes.decode('utf-8')
    doc = json.loads(json_text)
    if not is_glb:
        uri = doc['buffers'][0]['uri']
        if uri.startswith('data:'):
            raise xom.XomError('only a file-referenced buffer is supported (no data: URIs)')
        with open(os.path.join(bin_dir, uri) if bin_dir else uri, 'rb') as f:
            bin_bytes = f.read()

    roots = []
    if doc.get('scenes'):
        scene = doc['scenes'][doc.get('scene', 0)]
        roots = scene.get('nodes', [])
    else:
        roots = list(range(len(doc.get('nodes', []))))

    out = []

    def walk(idx, accum):
        node = doc['nodes'][idx]
        if 'matrix' in node:
            local = list(node['matrix'])
        else:
            local = identity()  # TRS decomposition not needed for this tool's own output
        world = mat_mul(accum, local)
        if 'mesh' in node:
            m = doc['meshes'][node['mesh']]
            for prim in m.get('primitives', []):
                if prim.get('mode', 4) != 4:
                    continue
                attrs = prim['attributes']
                pos_acc = doc['accessors'][attrs['POSITION']]
                positions = _read_accessor(doc, bin_bytes, pos_acc, 3)
                normals = _read_accessor(doc, bin_bytes, doc['accessors'][attrs['NORMAL']], 3) if 'NORMAL' in attrs else []
                uvs = _read_accessor(doc, bin_bytes, doc['accessors'][attrs['TEXCOORD_0']], 2) if 'TEXCOORD_0' in attrs else []
                if 'indices' in prim:
                    indices = _read_accessor(doc, bin_bytes, doc['accessors'][prim['indices']], 1)
                else:
                    indices = list(range(len(positions)))
                out.append({'name': m.get('name', ''), 'matrix': world, 'positions': positions,
                            'normals': normals, 'uvs': uvs, 'indices': indices})
        for c in node.get('children', []):
            walk(c, world)

    for r in roots:
        walk(r, identity())
    if not out:
        raise xom.XomError('no triangle mesh primitives found')
    return out
