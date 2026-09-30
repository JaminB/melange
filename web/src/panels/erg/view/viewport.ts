// The 3D view (three.js, loaded lazily with this module): terrain meshed from voxels in a worker, markers for every
// detail, water and surround, orbit camera, picking, box select and the transform gizmo. Scene units are .xan units.
import {
  AmbientLight, Box3, BoxGeometry, BufferAttribute, BufferGeometry, CanvasTexture, Color, DirectionalLight, EdgesGeometry,
  Euler, Fog, GridHelper, Group, HemisphereLight, LineBasicMaterial, LineSegments, Matrix4, Mesh, MeshBasicMaterial,
  MeshLambertMaterial, Object3D, PerspectiveCamera, PlaneGeometry, Quaternion, Raycaster, Scene as ThreeScene, Sprite,
  SpriteMaterial, SRGBColorSpace, Vector2, Vector3, WebGLRenderer,
} from "three";
import { OrbitControls } from "three/examples/jsm/controls/OrbitControls.js";
import { TransformControls } from "three/examples/jsm/controls/TransformControls.js";
import { GLTFLoader } from "three/examples/jsm/loaders/GLTFLoader.js";
import { WORLD_PER_XAN, type Detail, type DetailFields, type Mat3x4, type Vec3 } from "../../../sdk/erg";
import { setMany } from "../model/edits";
import { round, roundVec, snapVec } from "../model/geometry";
import { rayTerrain } from "../model/ground";
import { place, type PaletteEntry } from "../model/placing";
import { ROLE_STYLE, glyphOf } from "../model/roles";
import type { EditorStore } from "../model/store";
import type { MeshData } from "../terrain/mesher";
import { skyColors, themePalette } from "../terrain/materials";
import { MesherPool, buckets, meshInput, type Bucket } from "../terrain/pool";
import type { TerrainTool } from "../terrain/tool";

export type Tool = "translate" | "rotate" | "scale";

export interface ViewEvents {
  onMessage(text: string): void;
  onPlaced(): void;
}

export interface ViewStats {
  firstFrameMs: number | null; meshMs: number | null; drawCalls: number; triangles: number; frames: number;
  threaded: boolean; buckets: number;
}

export interface Viewport {
  dispose(): void;
  setTool(t: Tool): void;
  setSnap(step: number | null, angle: boolean): void;
  setPlacing(e: PaletteEntry | null): void;
  setSculpt(t: TerrainTool | null): void;
  setPreviews(byResource: Map<string, string>): void;
  setVisibleRoles(roles: Set<string>): void;
  focus(): void;
  remesh(frameIds?: number[]): Promise<void>;
  stats(): ViewStats;
  screenOf(detailId: number): [number, number] | null;
  screenOfWorld(p: Vec3): [number, number] | null;
  benchmark(ms: number): Promise<number>;
}

const toMatrix4 = (m: Mat3x4) => new Matrix4().set(m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], 0, 0, 0, 1);
const vec = (v: Vector3): Vec3 => [v.x, v.y, v.z];

interface Marker { obj: Object3D; kind: string; }

export function createViewport(el: HTMLElement, store: EditorStore, events: ViewEvents): Viewport {
  const t0 = performance.now();
  delete el.dataset.firstFrame;
  const renderer = new WebGLRenderer({ antialias: true, powerPreference: "high-performance" });
  renderer.setPixelRatio(Math.min(window.devicePixelRatio || 1, 2));
  renderer.outputColorSpace = SRGBColorSpace;
  const canvas = renderer.domElement;
  canvas.className = "erg-canvas";
  canvas.tabIndex = 0;
  el.appendChild(canvas);
  const band = document.createElement("div");
  band.className = "erg-band";
  el.appendChild(band);

  const scene = new ThreeScene();
  const camera = new PerspectiveCamera(50, 1, 0.1, 20000);
  camera.position.set(60, 60, 60);
  scene.add(new HemisphereLight(0xffffff, 0x444433, 1.6));
  scene.add(new AmbientLight(0xffffff, 0.25));
  const sun = new DirectionalLight(0xffffff, 1.6);
  sun.position.set(0.4, 1, 0.3);
  scene.add(sun);

  const terrain = new Group();
  const markers = new Group();
  scene.add(terrain, markers);
  const terrainMat = new MeshLambertMaterial({ vertexColors: true });
  const water = new Mesh(new PlaneGeometry(1, 1), new MeshBasicMaterial({ color: 0x2f6fb5, transparent: true, opacity: 0.35, depthWrite: false }));
  water.rotation.x = -Math.PI / 2;
  water.renderOrder = 2;
  scene.add(water);
  let grid: GridHelper | undefined;

  // Pointer listeners go on first so a shift-drag can switch the orbit off before it sees the event.
  let down: { x: number; y: number; box: boolean; button: number } | undefined;
  canvas.addEventListener("pointerdown", onDown);
  canvas.addEventListener("pointermove", onMove);
  canvas.addEventListener("pointerup", onUp);
  canvas.addEventListener("pointercancel", () => endBand());

  const orbit = new OrbitControls(camera, canvas);
  orbit.enableDamping = false;
  orbit.addEventListener("change", () => dirtyRender());

  const gizmo = new TransformControls(camera, canvas);
  const helper = gizmo.getHelper();
  scene.add(helper);
  const proxy = new Object3D();
  scene.add(proxy);
  let tool: Tool = "translate";
  let step: number | null = null;
  let angleSnap = false;
  let placing: PaletteEntry | null = null;
  let sculpt: TerrainTool | null = null;
  let sculpting = false;
  let visible: Set<string> | null = null;
  gizmo.setMode(tool);
  gizmo.setSpace("world");
  gizmo.addEventListener("dragging-changed", (e) => { orbit.enabled = !e.value; });
  gizmo.addEventListener("change", () => dirtyRender());

  let drag: { first: boolean; start: Vector3; starts: Map<number, Vec3> } | undefined;
  gizmo.addEventListener("mouseDown", () => {
    const starts = new Map<number, Vec3>();
    for (const d of store.selected()) starts.set(d.id, store.frames.detailWorld(d));
    drag = { first: true, start: proxy.position.clone(), starts };
  });
  gizmo.addEventListener("mouseUp", () => {
    drag = undefined;
    placeProxy();
  });
  gizmo.addEventListener("objectChange", () => {
    if (!drag) return;
    const sel = store.selected();
    if (!sel.length) return;
    const changes: [number, DetailFields][] = [];
    if (tool === "translate") {
      const delta = proxy.position.clone().sub(drag.start);
      for (const d of sel) {
        const s = drag.starts.get(d.id);
        if (!s) continue;
        const w: Vec3 = [s[0] + delta.x, s[1] + delta.y, s[2] + delta.z];
        changes.push([d.id, { pos: snapVec(store.frames.toLocal(d.frame, w), step) }]);
      }
    } else {
      const d = sel[0];
      const inv = new Matrix4().copy(toMatrix4(store.frames.worldOf(d.frame) ?? [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0])).invert();
      proxy.updateMatrix();
      const local = inv.multiply(proxy.matrix);
      const p = new Vector3(), q = new Quaternion(), s = new Vector3();
      local.decompose(p, q, s);
      if (tool === "rotate") {
        const e = new Euler().setFromQuaternion(q, "ZYX");
        changes.push([d.id, { rot: roundVec([e.x, e.y, e.z], 6) }]);
      } else {
        changes.push([d.id, { scale: roundVec(vec(s), 4).map((v) => (Math.abs(v) < 1e-3 ? 1e-3 : v)) as Vec3 }]);
      }
    }
    if (!changes.length) return;
    store.exec(setMany(changes, tool === "translate" ? "Move" : tool === "rotate" ? "Rotate" : "Scale"), !drag.first);
    drag.first = false;
  });

  // ------------------------------------------------------------------ terrain
  const pool = new MesherPool();
  let bucketList: Bucket[] = [];
  const bucketMesh = new Map<number, Mesh>();
  let firstFrameMs: number | null = null, meshMs: number | null = null;
  let generation = 0, framed = false;

  function toGeometry(m: MeshData): BufferGeometry {
    const g = new BufferGeometry();
    g.setAttribute("position", new BufferAttribute(m.positions, 3));
    g.setAttribute("normal", new BufferAttribute(m.normals, 3));
    g.setAttribute("color", new BufferAttribute(m.colors, 3, true));
    g.setIndex(new BufferAttribute(m.index, 1));
    g.computeBoundingBox();
    g.computeBoundingSphere();
    return g;
  }

  // A full remesh supersedes everything before it; partial ones (a brush stroke) only replace their own buckets.
  async function remesh(frameIds?: number[]) {
    const gen = frameIds ? generation : ++generation;
    const s = store.scene;
    if (!frameIds) bucketList = buckets(s);
    const want = frameIds ? new Set(frameIds) : null;
    const todo = bucketList.filter((b) => !want || b.frames.some((id) => want.has(id)));
    const palette = themePalette(s.databank.theme);
    const tm = performance.now();
    const results = await Promise.all(todo.map((b) =>
      pool.mesh(b.index, meshInput(s, b.frames, (f) => store.voxelsOf(f)), palette).then((m) => [b, m] as const)));
    if (gen !== generation || disposed) return;
    if (!frameIds) {
      for (const m of bucketMesh.values()) { terrain.remove(m); m.geometry.dispose(); }
      bucketMesh.clear();
    }
    for (const [b, m] of results) {
      const old = bucketMesh.get(b.index);
      if (old) { terrain.remove(old); old.geometry.dispose(); }
      const mesh = new Mesh(toGeometry(m), terrainMat);
      mesh.userData.bucket = b.index;
      bucketMesh.set(b.index, mesh);
      terrain.add(mesh);
    }
    meshMs = performance.now() - tm;
    if (!frameIds) {
      sizeSurroundings();
      if (!framed) frameView();
      framed = true;
    }
    dirtyRender();
  }

  // ------------------------------------------------------------------ markers
  const textures = new Map<string, CanvasTexture>();
  function glyphTexture(text: string, color: string, selected: boolean): CanvasTexture {
    const key = `${text}|${color}|${selected}`;
    let t = textures.get(key);
    if (t) return t;
    const c = document.createElement("canvas");
    c.width = c.height = 64;
    const g = c.getContext("2d")!;
    g.beginPath();
    g.arc(32, 32, 27, 0, Math.PI * 2);
    g.fillStyle = color;
    g.fill();
    g.lineWidth = selected ? 7 : 3;
    g.strokeStyle = selected ? "#ffd400" : "#ffffff";
    g.stroke();
    g.fillStyle = "#ffffff";
    g.font = "bold 30px system-ui, sans-serif";
    g.textAlign = "center";
    g.textBaseline = "middle";
    g.fillText(text, 32, 34);
    t = new CanvasTexture(c);
    t.colorSpace = SRGBColorSpace;
    textures.set(key, t);
    return t;
  }
  function labelTexture(text: string): CanvasTexture {
    const key = `box|${text}`;
    let t = textures.get(key);
    if (t) return t;
    const c = document.createElement("canvas");
    c.width = 128;
    c.height = 128;
    const g = c.getContext("2d")!;
    g.fillStyle = ROLE_STYLE.scenery.color;
    g.fillRect(0, 0, 128, 128);
    g.strokeStyle = "#5a4630";
    g.lineWidth = 6;
    g.strokeRect(3, 3, 122, 122);
    g.fillStyle = "#ffffff";
    g.font = "bold 18px system-ui, sans-serif";
    g.textAlign = "center";
    g.textBaseline = "middle";
    const words = text.slice(0, 24).match(/.{1,10}/g) ?? [text];
    words.forEach((w, i) => g.fillText(w, 64, 64 + (i - (words.length - 1) / 2) * 22));
    t = new CanvasTexture(c);
    t.colorSpace = SRGBColorSpace;
    textures.set(key, t);
    return t;
  }
  const spriteMats = new Map<string, SpriteMaterial>();
  function spriteMaterial(d: Detail, selected: boolean): SpriteMaterial {
    const text = glyphOf(d), color = ROLE_STYLE[d.role].color;
    const key = `${text}|${color}|${selected}`;
    let m = spriteMats.get(key);
    if (!m) {
      m = new SpriteMaterial({ map: glyphTexture(text, color, selected), depthTest: false, transparent: true, sizeAttenuation: false });
      spriteMats.set(key, m);
    }
    return m;
  }
  const boxGeo = new BoxGeometry(1, 1, 1).translate(0, 0.5, 0);
  const boxEdges = new EdgesGeometry(boxGeo);
  const selLine = new LineBasicMaterial({ color: 0xffd400, depthTest: false, transparent: true });
  const boxMats = new Map<string, MeshLambertMaterial>();
  const previews = new Map<string, string>();
  const gltfCache = new Map<string, Promise<Object3D | null>>();
  let gltf: GLTFLoader | undefined;

  function loadPreview(key: string): Promise<Object3D | null> {
    let p = gltfCache.get(key);
    if (!p) {
      gltf ??= new GLTFLoader();
      p = gltf.loadAsync(`/erg/assets/${encodeURIComponent(key)}.glb`).then((g) => {
        g.scene.scale.setScalar(1 / WORLD_PER_XAN);
        return g.scene as Object3D;
      }, () => null);
      gltfCache.set(key, p);
    }
    return p;
  }

  const markerOf = new Map<number, Marker>();
  function kindOf(d: Detail) {
    return d.role === "scenery" ? `box:${d.resource}:${previews.get(d.resource.toLowerCase()) ?? ""}` : `sprite:${glyphOf(d)}:${d.role}`;
  }
  function makeMarker(d: Detail): Marker {
    const kind = kindOf(d);
    let obj: Object3D;
    if (d.role === "scenery") {
      let mat = boxMats.get(d.resource);
      if (!mat) boxMats.set(d.resource, (mat = new MeshLambertMaterial({ map: labelTexture(d.resource) })));
      const box = new Mesh(boxGeo, mat);
      box.userData.detailId = d.id;
      obj = box;
      const key = previews.get(d.resource.toLowerCase());
      if (key) {
        loadPreview(key).then((model) => {
          if (!model || disposed || markerOf.get(d.id)?.obj !== box) return;
          const copy = model.clone();
          copy.traverse((o) => { o.userData.detailId = d.id; });
          box.add(copy);
          box.material = new MeshBasicMaterial({ visible: false });
          dirtyRender();
        });
      }
    } else {
      const s = new Sprite(spriteMaterial(d, false));
      s.center.set(0.5, 0);
      const size = d.role === "spawn" || d.role === "object" ? 0.05 : 0.032;
      s.scale.set(size, size, 1);
      s.renderOrder = 3;
      s.userData.detailId = d.id;
      obj = s;
    }
    markers.add(obj);
    return { obj, kind };
  }

  function syncMarkers() {
    const sel = new Set(store.selection);
    const seen = new Set<number>();
    for (const d of store.scene.details) {
      seen.add(d.id);
      let m = markerOf.get(d.id);
      if (m && m.kind !== kindOf(d)) {
        markers.remove(m.obj);
        m = undefined;
      }
      if (!m) {
        m = makeMarker(d);
        markerOf.set(d.id, m);
      }
      const o = m.obj;
      o.visible = !visible || visible.has(d.role) || sel.has(d.id);
      if (o instanceof Sprite) {
        o.material = spriteMaterial(d, sel.has(d.id));
        const w = store.frames.detailWorld(d);
        o.position.set(w[0], w[1], w[2]);
      } else {
        o.matrixAutoUpdate = false;
        o.matrix.copy(toMatrix4(store.frames.detailMatrix(d)));
        o.matrixWorldNeedsUpdate = true;
        const outline = o.children.find((c) => c.userData.outline);
        if (sel.has(d.id) && !outline) {
          const l = new LineSegments(boxEdges, selLine);
          l.userData.outline = true;
          l.userData.detailId = d.id;
          l.renderOrder = 4;
          o.add(l);
        } else if (!sel.has(d.id) && outline) o.remove(outline);
      }
    }
    for (const [id, m] of markerOf) if (!seen.has(id)) { markers.remove(m.obj); markerOf.delete(id); }
  }

  // ------------------------------------------------------------------ gizmo
  function gizmoBlocked(): string | null {
    const sel = store.selected();
    if (!sel.length) return null;
    if (tool !== "translate" && sel.length > 1) return "Rotate and scale work on one detail at a time";
    if (tool !== "translate" && sel.some((d) => store.translationOnly.has(d.frame)))
      return "This detail hangs under an animated frame: it can only be moved";
    return null;
  }
  function placeProxy(announce = false) {
    const sel = store.selected();
    const blocked = gizmoBlocked();
    if (!sel.length || blocked || placing) {
      gizmo.detach();
      if (blocked && announce) events.onMessage(blocked);
      dirtyRender();
      return;
    }
    if (sel.length === 1) {
      const m = toMatrix4(store.frames.detailMatrix(sel[0]));
      m.decompose(proxy.position, proxy.quaternion, proxy.scale);
    } else {
      const c = new Vector3();
      for (const d of sel) c.add(new Vector3(...store.frames.detailWorld(d)));
      proxy.position.copy(c.divideScalar(sel.length));
      proxy.quaternion.identity();
      proxy.scale.set(1, 1, 1);
    }
    proxy.updateMatrixWorld();
    gizmo.attach(proxy);
    gizmo.setSpace(tool === "translate" ? "world" : "local");
    dirtyRender();
  }

  // ------------------------------------------------------------------ level look
  function syncLevel() {
    const s = store.scene;
    const [sky, fog] = skyColors(s.databank.timeOfDay);
    scene.background = new Color(sky);
    scene.fog = new Fog(new Color(fog), 400, 3000);
    const lvl = s.water.level;
    water.position.y = (lvl ?? 0) / WORLD_PER_XAN;
    (water.material as MeshBasicMaterial).opacity = lvl === null ? 0.15 : 0.4;
    if (grid) grid.visible = s.hmp.mode !== "none";
  }

  function bounds(): Box3 {
    const b = new Box3();
    for (const m of bucketMesh.values()) if (m.geometry.boundingBox) b.union(m.geometry.boundingBox);
    for (const d of store.scene.details) b.expandByPoint(new Vector3(...store.frames.detailWorld(d)));
    if (b.isEmpty()) b.set(new Vector3(-50, -5, -50), new Vector3(50, 20, 50));
    return b;
  }

  function sizeSurroundings() {
    const b = bounds();
    const c = b.getCenter(new Vector3()), size = b.getSize(new Vector3());
    const r = Math.max(size.x, size.z, 20);
    water.scale.set(r * 4, r * 4, 1);
    water.position.x = c.x;
    water.position.z = c.z;
    if (grid) { scene.remove(grid); grid.geometry.dispose(); }
    grid = new GridHelper(r * 3, 100, 0x6b8f71, 0x8fae93);
    grid.position.set(c.x, 0.01, c.z);
    scene.add(grid);
    syncLevel();
  }

  function frameView() {
    const b = bounds();
    const c = b.getCenter(new Vector3()), size = b.getSize(new Vector3());
    const r = Math.max(size.x, size.z, 20);
    orbit.target.copy(c);
    camera.position.set(c.x + r * 0.7, c.y + r * 0.8, c.z + r * 0.7);
    camera.far = r * 20;
    camera.updateProjectionMatrix();
    orbit.update();
    dirtyRender();
  }

  function focusSelection() {
    const sel = store.selected();
    if (!sel.length) return frameView();
    const c = new Vector3();
    for (const d of sel) c.add(new Vector3(...store.frames.detailWorld(d)));
    c.divideScalar(sel.length);
    const off = camera.position.clone().sub(orbit.target).setLength(30);
    orbit.target.copy(c);
    camera.position.copy(c.clone().add(off));
    orbit.update();
    dirtyRender();
  }

  // ------------------------------------------------------------------ pointer
  const ray = new Raycaster();
  const ndc = (x: number, y: number) => {
    const r = canvas.getBoundingClientRect();
    return new Vector2(((x - r.left) / r.width) * 2 - 1, -((y - r.top) / r.height) * 2 + 1);
  };
  function sculptRay(e: PointerEvent): [Vec3, Vec3] {
    ray.setFromCamera(ndc(e.clientX, e.clientY), camera);
    return [vec(ray.ray.origin), vec(ray.ray.direction)];
  }
  function onDown(e: PointerEvent) {
    if (e.button !== 0) return;
    if (sculpt && !e.shiftKey && !e.altKey) {
      orbit.enabled = false;
      sculpting = true;
      canvas.setPointerCapture(e.pointerId);
      const r = sculpt.down(...sculptRay(e));
      if (r?.refused) events.onMessage(r.refused);
      return;
    }
    const box = e.shiftKey && !placing;
    orbit.enabled = !box;
    down = { x: e.clientX, y: e.clientY, box, button: e.button };
    if (box) canvas.setPointerCapture(e.pointerId);
  }
  function onMove(e: PointerEvent) {
    if (sculpting && sculpt) {
      const r = sculpt.drag(...sculptRay(e));
      if (r?.refused) events.onMessage(r.refused);
      return;
    }
    if (!down?.box) return;
    const r = el.getBoundingClientRect();
    const x0 = Math.min(down.x, e.clientX) - r.left, y0 = Math.min(down.y, e.clientY) - r.top;
    Object.assign(band.style, { display: "block", left: `${x0}px`, top: `${y0}px`, width: `${Math.abs(e.clientX - down.x)}px`, height: `${Math.abs(e.clientY - down.y)}px` });
  }
  function endBand() {
    if (sculpting) {
      sculpting = false;
      sculpt?.up();
    }
    band.style.display = "none";
    orbit.enabled = true;
    down = undefined;
  }
  function onUp(e: PointerEvent) {
    if (sculpting) {
      sculpting = false;
      orbit.enabled = true;
      sculpt?.up();
      return;
    }
    const d = down;
    endBand();
    if (!d || d.button !== 0) return;
    const moved = Math.hypot(e.clientX - d.x, e.clientY - d.y);
    if (d.box) {
      if (moved < 4) return;
      const [x0, x1] = [Math.min(d.x, e.clientX), Math.max(d.x, e.clientX)];
      const [y0, y1] = [Math.min(d.y, e.clientY), Math.max(d.y, e.clientY)];
      const r = canvas.getBoundingClientRect();
      const hits: number[] = [];
      for (const det of store.scene.details) {
        const m = markerOf.get(det.id);
        if (m && !m.obj.visible) continue;
        const sp = screenOfWorld(store.frames.detailWorld(det));
        if (sp && sp[0] + r.left >= x0 && sp[0] + r.left <= x1 && sp[1] + r.top >= y0 && sp[1] + r.top <= y1) hits.push(det.id);
      }
      store.select(e.ctrlKey || e.metaKey ? [...store.selection, ...hits] : hits);
      return;
    }
    if (moved > 4 || gizmo.dragging || gizmo.axis !== null) return;
    if (placing) return placeAt(e.clientX, e.clientY);
    ray.setFromCamera(ndc(e.clientX, e.clientY), camera);
    const hit = ray.intersectObjects(markers.children.filter((o) => o.visible), true)[0];
    const id = hit ? (hit.object.userData.detailId as number | undefined) : undefined;
    if (id === undefined) {
      if (!e.ctrlKey && !e.metaKey) store.select([]);
      return;
    }
    if (e.ctrlKey || e.metaKey) store.select(store.selection.includes(id) ? store.selection.filter((x) => x !== id) : [...store.selection, id]);
    else store.select([id]);
  }

  function groundAt(x: number, y: number): Vec3 | null {
    ray.setFromCamera(ndc(x, y), camera);
    const o = vec(ray.ray.origin), dir = vec(ray.ray.direction);
    const t = rayTerrain(store.scene, store.frames, (id) => store.voxelsOf({ id }), o, dir);
    if (t !== null) return [o[0] + dir[0] * t, o[1] + dir[1] * t, o[2] + dir[2] * t];
    const planeY = water.position.y;
    if (Math.abs(dir[1]) < 1e-6) return null;
    const tp = (planeY - o[1]) / dir[1];
    return tp > 0 ? [o[0] + dir[0] * tp, planeY, o[2] + dir[2] * tp] : null;
  }

  function placeAt(x: number, y: number) {
    if (!placing) return;
    const p = groundAt(x, y);
    if (!p) return events.onMessage("Nothing under the cursor to place on");
    const cmd = place(store.scene, store.frames, placing, [p[0], round(p[1] + 0.05), p[2]], step);
    if (typeof cmd === "string") return events.onMessage(cmd);
    store.exec(cmd);
    store.select([cmd.detailId]);
    events.onPlaced();
  }

  // ------------------------------------------------------------------ render loop
  let needRender = true, disposed = false, raf = 0, renders = 0, calls = 0, tris = 0;
  function dirtyRender() { needRender = true; }
  function resize() {
    const w = Math.max(1, el.clientWidth), h = Math.max(1, el.clientHeight);
    renderer.setSize(w, h, false);
    camera.aspect = w / h;
    camera.updateProjectionMatrix();
    dirtyRender();
  }
  const ro = new ResizeObserver(resize);
  ro.observe(el);
  resize();
  function loop() {
    if (disposed) return;
    raf = requestAnimationFrame(loop);
    if (!needRender) return;
    needRender = false;
    renderer.render(scene, camera);
    renders++;
    calls = renderer.info.render.calls;
    tris = renderer.info.render.triangles;
    if (firstFrameMs === null && meshMs !== null) {
      firstFrameMs = performance.now() - t0;
      el.dataset.firstFrame = String(Math.round(firstFrameMs));
    }
  }
  raf = requestAnimationFrame(loop);

  let lastTheme = store.scene.databank.theme;
  const off = store.on((why) => {
    if (why === "saved") return;
    syncMarkers();
    if (why === "scene") {
      syncLevel();
      if (!drag) placeProxy();
      const theme = store.scene.databank.theme;
      if (theme !== lastTheme) {
        lastTheme = theme;
        void remesh();
      }
    } else placeProxy(true);
    dirtyRender();
  });
  syncMarkers();
  syncLevel();
  void remesh();

  function screenOfWorld(p: Vec3): [number, number] | null {
    const v = new Vector3(p[0], p[1], p[2]).project(camera);
    if (v.z < -1 || v.z > 1) return null;
    const r = canvas.getBoundingClientRect();
    return [((v.x + 1) / 2) * r.width, ((1 - v.y) / 2) * r.height];
  }

  const api: Viewport = {
    dispose() {
      disposed = true;
      cancelAnimationFrame(raf);
      off();
      ro.disconnect();
      pool.dispose();
      gizmo.detach();
      gizmo.dispose();
      orbit.dispose();
      for (const m of bucketMesh.values()) m.geometry.dispose();
      for (const t of textures.values()) t.dispose();
      for (const m of [...spriteMats.values(), ...boxMats.values(), terrainMat, selLine]) m.dispose();
      boxGeo.dispose();
      boxEdges.dispose();
      renderer.dispose();
      renderer.forceContextLoss();
      canvas.remove();
      delete el.dataset.firstFrame;
      band.remove();
    },
    setTool(t) {
      tool = t;
      gizmo.setMode(t);
      placeProxy(true);
    },
    setSnap(s, angle) {
      step = s;
      angleSnap = angle;
      gizmo.setRotationSnap(angleSnap ? Math.PI / 12 : null);
      gizmo.setScaleSnap(s ? 0.1 : null);
    },
    setPlacing(e) {
      placing = e;
      canvas.style.cursor = e ? "crosshair" : "";
      placeProxy();
    },
    setSculpt(t) {
      if (sculpting) sculpt?.up();
      sculpting = false;
      sculpt = t;
      canvas.style.cursor = t ? "cell" : placing ? "crosshair" : "";
    },
    setPreviews(byResource) {
      previews.clear();
      for (const [k, v] of byResource) previews.set(k.toLowerCase(), v);
      syncMarkers();
      dirtyRender();
    },
    setVisibleRoles(roles) {
      visible = roles;
      syncMarkers();
      dirtyRender();
    },
    focus: focusSelection,
    remesh,
    stats: () => ({ firstFrameMs, meshMs, drawCalls: calls, triangles: tris, frames: renders, threaded: pool.threaded, buckets: bucketList.length }),
    screenOf(id) {
      const d = store.detail(id);
      if (!d) return null;
      const m = markerOf.get(id);
      if (m && !(m.obj instanceof Sprite)) return screenOfWorld(vec(new Vector3(0, 0.5, 0).applyMatrix4(m.obj.matrix)));
      const p = screenOfWorld(store.frames.detailWorld(d));
      if (!p || !m) return p;
      const px = (m.obj.scale.y / Math.tan((camera.fov * Math.PI) / 360)) * (canvas.getBoundingClientRect().height / 2);
      return [p[0], p[1] - px / 2];
    },
    screenOfWorld,
    benchmark(ms) {
      return new Promise((resolve) => {
        const start = performance.now();
        let n = 0;
        const radius = camera.position.distanceTo(orbit.target);
        const tick = () => {
          const t = performance.now() - start;
          if (t >= ms || disposed) return resolve((n * 1000) / Math.max(1, t));
          const a = t / 1000;
          camera.position.set(orbit.target.x + Math.cos(a) * radius * 0.7, orbit.target.y + radius * 0.6, orbit.target.z + Math.sin(a) * radius * 0.7);
          camera.lookAt(orbit.target);
          renderer.render(scene, camera);
          n++;
          requestAnimationFrame(tick);
        };
        requestAnimationFrame(tick);
      });
    },
  };
  return api;
}
