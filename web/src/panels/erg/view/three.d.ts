// Type declarations for the part of three.js the editor uses (the package ships no types, and @types/three would be a
// second dependency).
declare module "three" {
  export const SRGBColorSpace: string;

  export class Vector2 {
    constructor(x?: number, y?: number);
    x: number; y: number;
    set(x: number, y: number): this;
  }
  export class Vector3 {
    constructor(x?: number, y?: number, z?: number);
    x: number; y: number; z: number;
    set(x: number, y: number, z: number): this;
    setScalar(v: number): this;
    copy(v: Vector3): this;
    clone(): Vector3;
    add(v: Vector3): this;
    sub(v: Vector3): this;
    divideScalar(s: number): this;
    setLength(l: number): this;
    distanceTo(v: Vector3): number;
    project(camera: Camera): this;
    applyMatrix4(m: Matrix4): this;
  }
  export class Quaternion {
    identity(): this;
  }
  export class Euler {
    x: number; y: number; z: number;
    setFromQuaternion(q: Quaternion, order?: string): this;
  }
  export class Matrix4 {
    set(...v: number[]): this;
    copy(m: Matrix4): this;
    invert(): this;
    multiply(m: Matrix4): this;
    decompose(p: Vector3, q: Quaternion, s: Vector3): this;
  }
  export class Color {
    constructor(c?: string | number);
  }
  export class Box3 {
    constructor(min?: Vector3, max?: Vector3);
    set(min: Vector3, max: Vector3): this;
    union(b: Box3): this;
    expandByPoint(p: Vector3): this;
    isEmpty(): boolean;
    getCenter(t: Vector3): Vector3;
    getSize(t: Vector3): Vector3;
  }
  export interface EventLike { type: string; value?: unknown; }
  export class EventDispatcher {
    addEventListener(type: string, fn: (e: EventLike) => void): void;
    removeEventListener(type: string, fn: (e: EventLike) => void): void;
  }
  export class Object3D extends EventDispatcher {
    position: Vector3;
    quaternion: Quaternion;
    scale: Vector3;
    rotation: Euler;
    matrix: Matrix4;
    matrixAutoUpdate: boolean;
    matrixWorldNeedsUpdate: boolean;
    visible: boolean;
    renderOrder: number;
    userData: Record<string, unknown>;
    children: Object3D[];
    add(...o: Object3D[]): this;
    remove(...o: Object3D[]): this;
    clone(recursive?: boolean): this;
    traverse(fn: (o: Object3D) => void): void;
    updateMatrix(): void;
    updateMatrixWorld(force?: boolean): void;
    lookAt(v: Vector3): void;
  }
  export class Group extends Object3D {}
  export class Fog {
    constructor(color: Color, near?: number, far?: number);
  }
  export class Scene extends Object3D {
    background: Color | null;
    fog: Fog | null;
  }
  export class Camera extends Object3D {}
  export class PerspectiveCamera extends Camera {
    constructor(fov?: number, aspect?: number, near?: number, far?: number);
    aspect: number;
    fov: number;
    far: number;
    updateProjectionMatrix(): void;
  }
  export class Light extends Object3D {}
  export class AmbientLight extends Light { constructor(color?: number, intensity?: number); }
  export class HemisphereLight extends Light { constructor(sky?: number, ground?: number, intensity?: number); }
  export class DirectionalLight extends Light { constructor(color?: number, intensity?: number); }

  type TypedArray = Float32Array | Uint8Array | Uint16Array | Uint32Array;
  export class BufferAttribute {
    constructor(array: TypedArray, itemSize: number, normalized?: boolean);
  }
  export class BufferGeometry {
    boundingBox: Box3 | null;
    setAttribute(name: string, a: BufferAttribute): this;
    setIndex(a: BufferAttribute): this;
    computeBoundingBox(): void;
    computeBoundingSphere(): void;
    translate(x: number, y: number, z: number): this;
    dispose(): void;
  }
  export class BoxGeometry extends BufferGeometry { constructor(w?: number, h?: number, d?: number); }
  export class PlaneGeometry extends BufferGeometry { constructor(w?: number, h?: number); }
  export class EdgesGeometry extends BufferGeometry { constructor(g: BufferGeometry); }

  export class Texture {
    colorSpace: string;
    dispose(): void;
  }
  export class CanvasTexture extends Texture { constructor(c: HTMLCanvasElement); }

  interface MaterialParams {
    color?: number | string; map?: Texture; transparent?: boolean; opacity?: number; depthTest?: boolean;
    depthWrite?: boolean; vertexColors?: boolean; visible?: boolean; sizeAttenuation?: boolean;
  }
  export class Material {
    opacity: number;
    visible: boolean;
    dispose(): void;
  }
  export class MeshBasicMaterial extends Material { constructor(p?: MaterialParams); }
  export class MeshLambertMaterial extends Material { constructor(p?: MaterialParams); }
  export class LineBasicMaterial extends Material { constructor(p?: MaterialParams); }
  export class SpriteMaterial extends Material { constructor(p?: MaterialParams); }

  export class Mesh extends Object3D {
    constructor(g?: BufferGeometry, m?: Material);
    geometry: BufferGeometry;
    material: Material;
  }
  export class LineSegments extends Object3D { constructor(g?: BufferGeometry, m?: Material); }
  export class Sprite extends Object3D {
    constructor(m?: SpriteMaterial);
    center: Vector2;
    material: SpriteMaterial;
  }
  export class GridHelper extends LineSegments {
    constructor(size?: number, divisions?: number, c1?: number, c2?: number);
    geometry: BufferGeometry;
  }

  export interface Intersection { distance: number; object: Object3D; }
  export class Ray { origin: Vector3; direction: Vector3; }
  export class Raycaster {
    ray: Ray;
    setFromCamera(p: Vector2, c: Camera): void;
    intersectObjects(o: Object3D[], recursive?: boolean): Intersection[];
  }

  export class WebGLRenderer {
    constructor(p?: { antialias?: boolean; powerPreference?: string; canvas?: HTMLCanvasElement });
    domElement: HTMLCanvasElement;
    outputColorSpace: string;
    info: { render: { calls: number; triangles: number } };
    setPixelRatio(r: number): void;
    setSize(w: number, h: number, style?: boolean): void;
    render(s: Scene, c: Camera): void;
    dispose(): void;
    forceContextLoss(): void;
  }
}

declare module "three/examples/jsm/controls/OrbitControls.js" {
  import { Camera, EventDispatcher, Vector3 } from "three";
  export class OrbitControls extends EventDispatcher {
    constructor(camera: Camera, dom?: HTMLElement);
    enabled: boolean;
    enableDamping: boolean;
    target: Vector3;
    update(): boolean;
    dispose(): void;
  }
}

declare module "three/examples/jsm/controls/TransformControls.js" {
  import { Camera, EventDispatcher, Object3D } from "three";
  export class TransformControls extends EventDispatcher {
    constructor(camera: Camera, dom?: HTMLElement);
    dragging: boolean;
    axis: string | null;
    getHelper(): Object3D;
    attach(o: Object3D): this;
    detach(): this;
    setMode(m: "translate" | "rotate" | "scale"): void;
    setSpace(s: "world" | "local"): void;
    setTranslationSnap(v: number | null): void;
    setRotationSnap(v: number | null): void;
    setScaleSnap(v: number | null): void;
    dispose(): void;
  }
}

declare module "three/examples/jsm/loaders/GLTFLoader.js" {
  import { Object3D } from "three";
  export class GLTFLoader {
    loadAsync(url: string): Promise<{ scene: Object3D }>;
  }
}
