// Builds and updates the 3D objects for the scene: room, grid, layers, paths
// (with their editable points and time markers) and the listener avatar.
// The room lives in its own group so walls and objects for non-box geometry
// can be added next to it later without touching the rest.
import * as THREE from 'three';
import { CSS2DObject } from 'three/examples/jsm/renderers/CSS2DRenderer.js';
import { Line2 } from 'three/examples/jsm/lines/Line2.js';
import { LineGeometry } from 'three/examples/jsm/lines/LineGeometry.js';
import { LineMaterial } from 'three/examples/jsm/lines/LineMaterial.js';
import type { Store } from '../model/store';
import { isStereo, stereoOffset, defaultStereo, isAmbisonic, defaultAmbisonic, hasPath, isSpatialized, isLinked, linkedAt, type LayerDoc, type PathDoc, type V3 } from '../model/scene';
import { layerOfPath, layerPathIndex } from '../model/layerMotion';
import { editablePoints, getPoint, isHandle, samplePath, type PointRef } from '../model/geometry';
import type { Viewport } from './viewport';

export const LAYER_RADIUS = 0.22;
const v3 = (p: V3) => new THREE.Vector3(p[0], p[1], p[2]);

// Head orientation in the engine's convention -> Three.js quaternion.
// Engine: q = yaw about +Y * pitch about +X * roll about -Z (docs/engine.md).
export function headQuaternion(yawDeg: number, pitchDeg: number, rollDeg: number): THREE.Quaternion {
  const d = Math.PI / 180;
  return new THREE.Quaternion().setFromEuler(new THREE.Euler(pitchDeg * d, yawDeg * d, -rollDeg * d, 'YXZ'));
}

// A layer is a ball on a stem; a stereo layer is two balls (left, right) on
// stems joined by a bar, marked L and R, with a handle and the label at the
// centre (drag the handle or the bar to move the pair); an Ambisonic layer is
// the ball inside a translucent sphere, with an arrow for the recording's
// front and a handle on its surface that sets the radius.
interface LayerEnd { ball: THREE.Mesh; stem: THREE.Line; ring: THREE.Mesh; tag: CSS2DObject }
interface Sphere { group: THREE.Group; shell: THREE.Mesh; wire: THREE.LineSegments; equator: THREE.Line; front: THREE.ArrowHelper; handle: THREE.Mesh }
interface LayerObj { group: THREE.Group; ball: THREE.Mesh; stem: THREE.Line; ring: THREE.Mesh; leftTag: CSS2DObject; right: LayerEnd; bar: THREE.Mesh; hub: THREE.Mesh; arrow: THREE.ArrowHelper; sphere: Sphere; label: CSS2DObject; meter: HTMLElement; name: HTMLElement; link: THREE.Line }

export interface PointHandle { mesh: THREE.Mesh; path: number; ref: PointRef }

export class SceneView {
  readonly root = new THREE.Group();
  readonly roomGroup = new THREE.Group();
  readonly layerGroup = new THREE.Group();
  readonly pathGroup = new THREE.Group();
  readonly pointGroup = new THREE.Group();
  readonly markerGroup = new THREE.Group();
  readonly previewGroup = new THREE.Group();
  readonly listener = new THREE.Group();
  private layers: LayerObj[] = [];
  pointHandles: PointHandle[] = [];
  // The listener's starting point: a disc on the floor under it (drag it to
  // move the start, and the path with it).
  startHandles: THREE.Object3D[] = [];
  private lineMaterials: LineMaterial[] = [];
  private head = new THREE.Group();

  constructor(private vp: Viewport, private store: Store) {
    this.root.add(this.roomGroup, this.layerGroup, this.pathGroup, this.markerGroup, this.pointGroup, this.previewGroup, this.listener);
    vp.scene.add(this.root);
    this.buildListener();
    vp.onBeforeRender(() => {
      const w = vp.container.clientWidth, h = vp.container.clientHeight;
      for (const m of this.lineMaterials) m.resolution.set(w, h);
    });
  }

  // ---------------------------------------------------------------- room

  rebuildRoom(): void {
    clear(this.roomGroup);
    const room = this.store.scene.room;
    if (room.type === 'box') {
      const [w, h, d] = room.size;
      const o = v3(room.origin);
      const box = new THREE.BoxGeometry(w, h, d);
      const edges = new THREE.LineSegments(new THREE.EdgesGeometry(box), new THREE.LineBasicMaterial({ color: 0x8090a8 }));
      edges.position.set(o.x, o.y + h / 2, o.z);
      this.roomGroup.add(edges);
      const floor = new THREE.Mesh(new THREE.PlaneGeometry(w, d),
        new THREE.MeshStandardMaterial({ color: 0x2a2f38, roughness: 1, side: THREE.DoubleSide }));
      floor.rotation.x = -Math.PI / 2;
      floor.position.set(o.x, o.y - 0.002, o.z);
      floor.name = 'floor';
      this.roomGroup.add(floor);
      const grid = new THREE.GridHelper(Math.ceil(Math.max(w, d)), Math.ceil(Math.max(w, d)), 0x3a4250, 0x2c323c);
      grid.position.set(o.x, o.y, o.z);
      (grid.material as THREE.Material).depthWrite = false;
      this.roomGroup.add(grid);
      // "Front" marker on the -Z wall so orientation is obvious.
      const label = makeLabel('front wall', 'wall-label');
      label.position.set(o.x, o.y + h + 0.3, o.z - d / 2);
      this.roomGroup.add(label);
    } else if (room.type === 'mesh') {
      // The mesh comes from the engine's analysis (it reads the OBJ).
      const m = this.store.analysis?.room_mesh;
      if (m) {
        const g = new THREE.BufferGeometry();
        g.setAttribute('position', new THREE.Float32BufferAttribute(m.vertices.flat(), 3));
        g.setIndex(m.triangles.flat());
        const edges = new THREE.LineSegments(new THREE.EdgesGeometry(g, 20), new THREE.LineBasicMaterial({ color: 0x8090a8 }));
        this.roomGroup.add(edges);
        g.computeBoundingBox();
        const b = g.boundingBox!;
        const w = b.max.x - b.min.x, d = b.max.z - b.min.z;
        const floor = new THREE.Mesh(new THREE.PlaneGeometry(w, d),
          new THREE.MeshStandardMaterial({ color: 0x2a2f38, roughness: 1, side: THREE.DoubleSide, transparent: true, opacity: 0.6 }));
        floor.rotation.x = -Math.PI / 2;
        floor.position.set((b.min.x + b.max.x) / 2, b.min.y - 0.002, (b.min.z + b.max.z) / 2);
        floor.name = 'floor';
        this.roomGroup.add(floor);
        g.dispose();
      }
      const grid = new THREE.GridHelper(40, 40, 0x3a4250, 0x262b33);
      (grid.material as THREE.Material).depthWrite = false;
      this.roomGroup.add(grid);
    } else {
      const grid = new THREE.GridHelper(60, 60, 0x3a4250, 0x262b33);
      this.roomGroup.add(grid);
      const ground = new THREE.Mesh(new THREE.PlaneGeometry(60, 60),
        new THREE.MeshStandardMaterial({ color: room.type === 'outdoor' ? 0x22302a : 0x1c1f25, roughness: 1 }));
      ground.rotation.x = -Math.PI / 2;
      ground.position.y = -0.002;
      ground.name = 'floor';
      this.roomGroup.add(ground);
    }
    for (const o of room.objects ?? []) {
      const size = o.max.map((v, i) => Math.max(v - o.min[i], 0.01)) as V3;
      const box = new THREE.BoxGeometry(...size);
      const solid = new THREE.Mesh(box, new THREE.MeshStandardMaterial({ color: 0x6b7280, transparent: true, opacity: 0.35, roughness: 1 }));
      const edges = new THREE.LineSegments(new THREE.EdgesGeometry(box), new THREE.LineBasicMaterial({ color: 0xa0aab8 }));
      const group = new THREE.Group();
      group.add(solid, edges);
      group.position.set((o.min[0] + o.max[0]) / 2, (o.min[1] + o.max[1]) / 2, (o.min[2] + o.max[2]) / 2);
      if (o.name) {
        const label = makeLabel(o.name, 'wall-label');
        label.position.set(0, size[1] / 2 + 0.2, 0);
        group.add(label);
      }
      this.roomGroup.add(group);
    }
    this.vp.invalidate();
  }

  // -------------------------------------------------------------- layers

  rebuildLayers(): void {
    for (const l of this.layers) disposeLayer(l);
    clear(this.layerGroup);
    this.layers = this.store.scene.layers.map((l, i) => this.makeLayer(l, i));
    this.updateLayers();
  }

  private makeEnd(group: THREE.Group, index: number, end: 'L' | 'R' | 'centre'): LayerEnd {
    const ball = new THREE.Mesh(new THREE.SphereGeometry(LAYER_RADIUS, 32, 16), new THREE.MeshStandardMaterial({ roughness: 0.4, transparent: true, opacity: 1 }));
    ball.userData = { kind: 'layer', index, end };
    group.add(ball);
    const stem = new THREE.Line(new THREE.BufferGeometry().setFromPoints([new THREE.Vector3(), new THREE.Vector3(0, -1, 0)]),
      new THREE.LineDashedMaterial({ color: 0x8a94a6, dashSize: 0.1, gapSize: 0.08 }));
    group.add(stem);
    const ring = new THREE.Mesh(new THREE.RingGeometry(0.18, 0.24, 32), new THREE.MeshBasicMaterial({ side: THREE.DoubleSide, transparent: true, opacity: 0.6 }));
    ring.rotation.x = -Math.PI / 2;
    group.add(ring);
    // The L / R letter sits on the ball (placeEnd moves it with the ball).
    const tag = makeLabel(end === 'R' ? 'R' : 'L', 'layer-end');
    group.add(tag);
    return { ball, stem, ring, tag };
  }

  private makeSphere(group: THREE.Group, index: number): Sphere {
    const sg = new THREE.Group();
    const shell = new THREE.Mesh(new THREE.SphereGeometry(1, 48, 24),
      new THREE.MeshStandardMaterial({ roughness: 0.9, transparent: true, opacity: 0.12, depthWrite: false, side: THREE.DoubleSide }));
    shell.renderOrder = 1;
    const wire = new THREE.LineSegments(new THREE.WireframeGeometry(new THREE.SphereGeometry(1, 16, 8)),
      new THREE.LineBasicMaterial({ transparent: true, opacity: 0.18, depthWrite: false }));
    const ring = new THREE.EllipseCurve(0, 0, 1, 1, 0, Math.PI * 2, false, 0).getPoints(96).map((p) => new THREE.Vector3(p.x, 0, p.y));
    const equator = new THREE.Line(new THREE.BufferGeometry().setFromPoints(ring), new THREE.LineBasicMaterial({ transparent: true, opacity: 0.6 }));
    // The recording's front (-Z of the recording) and a handle on its right.
    const front = new THREE.ArrowHelper(new THREE.Vector3(0, 0, -1), new THREE.Vector3(), 1, 0xffffff, 0.25, 0.12);
    const handle = new THREE.Mesh(new THREE.BoxGeometry(0.16, 0.16, 0.16), new THREE.MeshBasicMaterial({ color: 0xffffff }));
    handle.userData = { kind: 'layer', index, end: 'radius' };
    sg.add(shell, wire, equator, front, handle);
    group.add(sg);
    return { group: sg, shell, wire, equator, front, handle };
  }

  private makeLayer(_l: LayerDoc, index: number): LayerObj {
    const group = new THREE.Group();
    const sphere = this.makeSphere(group, index);
    const left = this.makeEnd(group, index, 'centre');
    const { ball, stem, ring } = left;
    const right = this.makeEnd(group, index, 'R');
    const bar = new THREE.Mesh(new THREE.CylinderGeometry(0.05, 0.05, 1, 12, 1),
      new THREE.MeshStandardMaterial({ roughness: 0.6, transparent: true, opacity: 0.85 }));
    bar.userData = { kind: 'layer', index, end: 'centre' };
    group.add(bar);
    const hub = new THREE.Mesh(new THREE.OctahedronGeometry(0.13), new THREE.MeshStandardMaterial({ roughness: 0.3, color: 0xffffff }));
    hub.userData = { kind: 'layer', index, end: 'centre' };
    group.add(hub);
    const arrow = new THREE.ArrowHelper(new THREE.Vector3(0, 0, 1), new THREE.Vector3(), 0.7, 0xffffff, 0.18, 0.12);
    group.add(arrow);
    const div = document.createElement('div');
    div.className = 'layer-label';
    const name = document.createElement('span');
    const meterBox = document.createElement('div');
    meterBox.className = 'meter';
    const meter = document.createElement('div');
    meterBox.appendChild(meter);
    div.append(name, meterBox);
    const label = new CSS2DObject(div);
    label.position.set(0, LAYER_RADIUS + 0.25, 0);
    group.add(label);
    this.layerGroup.add(group);
    // A dashed line to what the layer is linked to (world coordinates).
    const link = new THREE.Line(new THREE.BufferGeometry().setFromPoints([new THREE.Vector3(), new THREE.Vector3()]),
      new THREE.LineDashedMaterial({ color: 0xf2994a, dashSize: 0.15, gapSize: 0.1, transparent: true, opacity: 0.8 }));
    link.visible = false;
    this.layerGroup.add(link);
    return { group, ball, stem, ring, leftTag: left.tag, right, bar, hub, arrow, sphere, label, meter, name, link };
  }

  // Places a ball and its stem and floor ring at `p` (group-local).
  private placeEnd(e: { ball: THREE.Mesh; stem: THREE.Line; ring: THREE.Mesh }, p: THREE.Vector3, floorY: number, color: THREE.Color, tag?: CSS2DObject): void {
    e.ball.position.copy(p);
    tag?.position.copy(p);
    const h = p.y - floorY;
    (e.stem.geometry as THREE.BufferGeometry).setFromPoints([p, new THREE.Vector3(p.x, p.y - h, p.z)]);
    e.stem.computeLineDistances();
    e.ring.position.set(p.x, p.y - h + 0.01, p.z);
    (e.ring.material as THREE.MeshBasicMaterial).color.copy(color);
  }

  updateLayers(): void {
    const s = this.store.scene;
    if (s.layers.length !== this.layers.length) return this.rebuildLayers();
    const sel = this.store.selection;
    const anySolo = s.layers.some((l) => l.solo);
    const floorY = s.room.type === 'box' ? s.room.origin[1] : 0;
    s.layers.forEach((l, i) => {
      const o = this.layers[i];
      // A layer with a path is drawn where it is at the playhead, turned with
      // its direction of travel when it turns along the path.
      const [px, py, pz, yaw] = this.store.layerPlace(i, this.store.time);
      o.group.position.set(px, py, pz);
      o.group.rotation.y = yaw * Math.PI / 180;
      const color = new THREE.Color(l.color ?? '#4f9cf9');
      const silent = l.mute || (anySolo && !l.solo);
      const mat = o.ball.material as THREE.MeshStandardMaterial;
      mat.color.copy(silent ? color.clone().multiplyScalar(0.35) : color);
      // Straight through: a see-through ball, since its place is not heard.
      const spatial = isSpatialized(l);
      mat.opacity = spatial ? 1 : 0.4;
      (o.right.ball.material as THREE.MeshStandardMaterial).opacity = mat.opacity;
      // Linked and following at the playhead: a dashed line to its leader.
      const t = this.store.time;
      const following = isLinked(l) && linkedAt(l.link!, t);
      o.link.visible = following;
      if (following) {
        const to = l.link!.to;
        const lp: V3 = to === 'listener' ? [this.listener.position.x, this.listener.position.y, this.listener.position.z]
          : s.layers[to] ? (this.store.layerPlace(to, t).slice(0, 3) as V3) : [px, py, pz];
        (o.link.geometry as THREE.BufferGeometry).setFromPoints([new THREE.Vector3(px, py, pz), new THREE.Vector3(lp[0], lp[1], lp[2])]);
        o.link.computeLineDistances();
        (o.link.material as THREE.LineDashedMaterial).color.copy(color);
      }
      const selected = sel.kind === 'layer' && sel.index === i;
      mat.emissive.copy(selected ? color : new THREE.Color(0));
      mat.emissiveIntensity = selected ? 0.6 : 0;
      o.ball.scale.setScalar(selected ? 1.25 : 1);
      // Stereo: left and right ends either side of the centre, joined by a
      // bar; folded to mono, the pair collapses to one ball at the centre.
      const stereo = isStereo(l);
      const st = l.stereo ?? defaultStereo();
      const spread = stereo && !st.mono;
      const d = spread ? stereoOffset(st) : [0, 0, 0];
      const floor = floorY - py;  // group-local floor height
      this.placeEnd(o, new THREE.Vector3(-d[0], -d[1], -d[2]), floor, color, o.leftTag);
      o.ball.userData.end = stereo ? 'L' : 'centre';
      o.right.ball.visible = o.right.stem.visible = o.right.ring.visible = spread;
      o.right.tag.visible = spread;
      o.leftTag.visible = spread;
      o.hub.visible = spread;
      if (spread) {
        this.placeEnd(o.right, new THREE.Vector3(d[0], d[1], d[2]), floor, color, o.right.tag);
        (o.hub.material as THREE.MeshStandardMaterial).emissive.copy(selected ? color : new THREE.Color(0));
        (o.hub.material as THREE.MeshStandardMaterial).emissiveIntensity = selected ? 0.5 : 0;
        const rm = o.right.ball.material as THREE.MeshStandardMaterial;
        rm.color.copy(mat.color);
        rm.emissive.copy(mat.emissive);
        rm.emissiveIntensity = mat.emissiveIntensity;
        o.right.ball.scale.setScalar(selected ? 1.25 : 1);
        const len = 2 * Math.hypot(d[0], d[1], d[2]);
        o.bar.visible = len > 2 * LAYER_RADIUS;
        o.bar.scale.set(1, Math.max(0.01, len - 2 * LAYER_RADIUS * 0.9), 1);
        o.bar.quaternion.setFromUnitVectors(new THREE.Vector3(0, 1, 0), new THREE.Vector3(d[0], d[1], d[2]).normalize());
        (o.bar.material as THREE.MeshStandardMaterial).color.copy(silent ? color.clone().multiplyScalar(0.35) : color);
      } else {
        o.bar.visible = false;
      }
      // Ambisonic: the sphere around the centre, turned like the recording.
      const ambi = isAmbisonic(l);
      o.sphere.group.visible = ambi;
      if (ambi) {
        const a = l.ambisonic ?? defaultAmbisonic();
        const r = Math.max(0.05, a.radius);
        o.sphere.group.quaternion.copy(headQuaternion(a.yaw, a.pitch, a.roll));
        o.sphere.shell.scale.setScalar(r);
        o.sphere.wire.scale.setScalar(r);
        o.sphere.equator.scale.setScalar(r);
        o.sphere.front.setLength(r, Math.min(0.4, r * 0.25), Math.min(0.2, r * 0.12));
        o.sphere.front.setColor(color);
        o.sphere.handle.position.set(r, 0, 0);
        (o.sphere.handle.material as THREE.MeshBasicMaterial).color.copy(selected ? new THREE.Color(0xffffff) : color);
        const tint = silent ? color.clone().multiplyScalar(0.5) : color;
        (o.sphere.shell.material as THREE.MeshStandardMaterial).color.copy(tint);
        (o.sphere.shell.material as THREE.MeshStandardMaterial).opacity = selected ? 0.2 : 0.12;
        (o.sphere.wire.material as THREE.LineBasicMaterial).color.copy(tint);
        (o.sphere.equator.material as THREE.LineBasicMaterial).color.copy(tint);
      }
      o.arrow.visible = !ambi && l.directivity > 0.01;
      const f = v3(l.directivity_forward);
      if (f.lengthSq() > 1e-9) o.arrow.setDirection(f.normalize());
      o.arrow.setColor(color);
      o.name.textContent = (l.name || `Layer ${i + 1}`) + (spatial ? '' : ' · direct') + (following ? ' · linked' : '');
      o.label.element.classList.toggle('selected', selected);
      o.label.element.classList.toggle('silent', silent);
    });
    this.updateMeters();
    this.vp.invalidate();
  }

  updateMeters(): void {
    const m = this.store.meters;
    this.layers.forEach((o, i) => {
      const db = m[i] ?? -120;
      const frac = Math.max(0, Math.min(1, (db + 60) / 60));
      o.meter.style.width = `${(frac * 100).toFixed(1)}%`;
      o.meter.style.background = db > -3 ? '#eb5757' : db > -12 ? '#f2c94c' : '#5fd38d';
    });
  }

  // --------------------------------------------------------------- paths

  rebuildPaths(): void {
    clear(this.pathGroup);
    clear(this.pointGroup);
    clear(this.markerGroup);
    this.lineMaterials = [];
    this.pointHandles = [];
    this.startHandles = [];
    const L = this.store.scene.listener;
    const a = this.store.analysis;
    const fresh = a && a.revision === this.store.revision;
    L.paths.forEach((p, i) => {
      const active = i === L.active_path;
      // The engine's samples when they match the current revision; the local
      // evaluator (same maths) while an edit is in flight.
      const pts = fresh && a!.paths[i]?.points.length ? a!.paths[i].points : samplePath(p);
      if (pts.length < 2) return;
      const line = this.fatLine(pts as V3[], active ? 0xffc857 : 0x7b8494, active ? 3 : 1.5, active ? 1 : 0.7);
      line.userData = { kind: 'path', path: i };
      this.pathGroup.add(line);
      // Start arrow and end flag.
      if (active) {
        this.pathGroup.add(endMarker(pts[0] as V3, 0x5fd38d));
        if (!p.closed) this.pathGroup.add(endMarker(pts[pts.length - 1] as V3, 0xeb5757));
        this.buildStart(pts[0] as V3);
        this.buildPointHandles(p, i);
      }
    });
    if (!L.paths.length) this.buildStart(L.static_position);
    this.buildLayerPaths();
    this.buildTimeMarkers();
    this.vp.invalidate();
  }

  // Each layer's own path in its colour, from where the layer stands; the
  // selected layer's (or the one whose point is selected) with its points.
  private buildLayerPaths(): void {
    const s = this.store.scene;
    const a = this.store.analysis;
    const sel = this.store.selection;
    const editing = sel.kind === 'layer' ? sel.index : sel.kind === 'point' ? layerOfPath(sel.path) : -1;
    s.layers.forEach((l, i) => {
      if (!hasPath(l)) return;
      const p = l.motion!.path;
      const tr = a && a.revision === this.store.revision ? a.layers?.find((x) => x.layer === i) : undefined;
      const pts = tr?.points.length ? tr.points : samplePath(p);
      if (pts.length < 2) return;
      const color = new THREE.Color(l.color ?? '#4f9cf9').getHex();
      const selected = editing === i;
      const line = this.fatLine(pts as V3[], color, selected ? 3 : 2, selected ? 1 : 0.65);
      line.userData = { kind: 'layerpath', layer: i };
      this.pathGroup.add(line);
      if (!p.closed) this.pathGroup.add(endMarker(pts[pts.length - 1] as V3, color, 0.09));
      if (selected) this.buildPointHandles(p, layerPathIndex(i));
    });
  }

  // The start disc on the floor under `p`, with a stem up to it and a label.
  private buildStart(p: V3): void {
    const s = this.store.scene;
    const floorY = s.room.type === 'box' ? s.room.origin[1] : 0;
    const g = new THREE.Group();
    const disc = new THREE.Mesh(new THREE.CircleGeometry(0.45, 40),
      new THREE.MeshBasicMaterial({ color: 0x5fd38d, transparent: true, opacity: 0.3, side: THREE.DoubleSide, depthWrite: false }));
    disc.rotation.x = -Math.PI / 2;
    disc.position.set(p[0], floorY + 0.012, p[2]);
    disc.userData = { kind: 'start' };
    const rim = new THREE.Mesh(new THREE.RingGeometry(0.42, 0.47, 40), new THREE.MeshBasicMaterial({ color: 0x5fd38d, side: THREE.DoubleSide }));
    rim.rotation.x = -Math.PI / 2;
    rim.position.set(p[0], floorY + 0.014, p[2]);
    rim.userData = { kind: 'start' };
    const stem = new THREE.Line(new THREE.BufferGeometry().setFromPoints([new THREE.Vector3(p[0], floorY, p[2]), v3(p)]),
      new THREE.LineDashedMaterial({ color: 0x5fd38d, dashSize: 0.08, gapSize: 0.06 }));
    stem.computeLineDistances();
    const label = makeLabel('Start', 'start-label');
    label.position.set(p[0], floorY + 0.05, p[2] + 0.62);
    g.add(disc, rim, stem, label);
    this.pathGroup.add(g);
    this.startHandles.push(disc, rim);
  }

  private fatLine(points: V3[], color: number, width: number, opacity: number): Line2 {
    const g = new LineGeometry();
    g.setPositions(points.flat());
    const m = new LineMaterial({ color, linewidth: width, transparent: opacity < 1, opacity, worldUnits: false });
    m.resolution.set(this.vp.container.clientWidth, this.vp.container.clientHeight);
    this.lineMaterials.push(m);
    const line = new Line2(g, m);
    line.computeLineDistances();
    return line;
  }

  private buildPointHandles(p: PathDoc, pathIndex: number): void {
    const sel = this.store.selection;
    const anchorGeo = new THREE.SphereGeometry(0.12, 16, 8);
    const handleGeo = new THREE.BoxGeometry(0.11, 0.11, 0.11);
    for (const ref of editablePoints(p)) {
      const seg = p.segments[ref.seg];
      const pos = getPoint(p, ref)!;
      const handle = isHandle(seg, ref.pt);
      const selected = sel.kind === 'point' && sel.path === pathIndex && sel.ref.seg === ref.seg && sel.ref.pt === ref.pt;
      const mesh = new THREE.Mesh(handle ? handleGeo : anchorGeo,
        new THREE.MeshBasicMaterial({ color: selected ? 0xffffff : handle ? 0x56ccf2 : 0xffc857, depthTest: false, transparent: true }));
      mesh.renderOrder = 10;
      mesh.position.copy(v3(pos));
      mesh.userData = { kind: 'point', path: pathIndex, ref };
      this.pointGroup.add(mesh);
      this.pointHandles.push({ mesh, path: pathIndex, ref });
      if (handle) {
        const anchor = seg.points[ref.pt === 1 ? 0 : 3];
        const l = new THREE.Line(new THREE.BufferGeometry().setFromPoints([v3(anchor), v3(pos)]),
          new THREE.LineBasicMaterial({ color: 0x56ccf2, transparent: true, opacity: 0.7, depthTest: false }));
        l.renderOrder = 9;
        this.pointGroup.add(l);
      }
    }
  }

  // Dots where the listener will be at each second, labelled every 5 s
  // (from the engine's pose evaluation, so speed changes show as spacing).
  private buildTimeMarkers(): void {
    const a = this.store.analysis;
    if (!a || a.active_path < 0 || !a.poses.length) return;
    const step = Math.max(1, Math.round(1 / a.dt));
    const dotGeo = new THREE.SphereGeometry(0.045, 8, 6);
    const dotMat = new THREE.MeshBasicMaterial({ color: 0xffe3a3 });
    let lastPos: number[] | null = null;
    for (let i = 0; i < a.poses.length; i += step) {
      const p = a.poses[i];
      const t = i * a.dt;
      if (lastPos && Math.hypot(p[0] - lastPos[0], p[1] - lastPos[1], p[2] - lastPos[2]) < 0.05) continue;  // standing still
      lastPos = p;
      const dot = new THREE.Mesh(dotGeo, dotMat);
      dot.position.set(p[0], p[1], p[2]);
      this.markerGroup.add(dot);
      if (Math.round(t) % 5 === 0) {
        const lab = makeLabel(`${Math.round(t)}s`, 'time-label');
        lab.position.set(p[0], p[1] + 0.25, p[2]);
        this.markerGroup.add(lab);
      }
    }
  }

  // ------------------------------------------------------------ listener

  private buildListener(): void {
    const skin = new THREE.MeshStandardMaterial({ color: 0xe8e1d6, roughness: 0.6 });
    const headMesh = new THREE.Mesh(new THREE.SphereGeometry(0.11, 24, 16), skin);
    headMesh.scale.set(0.9, 1.05, 1);
    const nose = new THREE.Mesh(new THREE.ConeGeometry(0.03, 0.08, 12), skin);
    nose.rotation.x = -Math.PI / 2;
    nose.position.set(0, -0.01, -0.12);
    const earGeo = new THREE.SphereGeometry(0.03, 10, 8);
    const earL = new THREE.Mesh(earGeo, skin); earL.position.set(-0.1, 0, 0); earL.scale.set(0.5, 1.2, 0.9);
    const earR = earL.clone(); earR.position.x = 0.1;
    // View cone: shows where the head points from any camera angle.
    const cone = new THREE.Mesh(new THREE.ConeGeometry(0.35, 1.2, 24, 1, true),
      new THREE.MeshBasicMaterial({ color: 0xffc857, transparent: true, opacity: 0.18, side: THREE.DoubleSide, depthWrite: false }));
    cone.rotation.x = Math.PI / 2;
    cone.position.z = -0.7;
    this.head.add(headMesh, nose, earL, earR, cone);
    this.head.traverse((o) => { o.userData = { kind: 'listener' }; });
    const body = new THREE.Mesh(new THREE.CylinderGeometry(0.16, 0.12, 0.9, 16),
      new THREE.MeshStandardMaterial({ color: 0x3c4658, roughness: 0.8 }));
    body.name = 'body';
    this.listener.add(this.head, body);
  }

  updateListener(pose: number[] | null): void {
    const L = this.store.scene.listener;
    const p = pose ?? [...L.static_position, 0, 0, 0, 0, 0];
    this.listener.position.set(p[0], p[1], p[2]);
    this.head.quaternion.copy(headQuaternion(p[3], p[4], p[5]));
    const body = this.listener.getObjectByName('body')!;
    const floorY = this.store.scene.room.type === 'box' ? this.store.scene.room.origin[1] : 0;
    const h = Math.max(0.3, p[1] - floorY - 0.2);
    body.scale.y = h / 0.9;
    body.position.y = -0.2 - h / 2;
    body.rotation.y = p[3] * Math.PI / 180;
    this.vp.setEye(new THREE.Vector3(p[0], p[1], p[2]), this.head.quaternion);
    this.listener.visible = this.vp.view !== 'listener';
    this.vp.invalidate();
  }

  // ------------------------------------------------------------- preview

  setPreview(points: V3[], extra: V3[] = [], handles: [V3, V3][] = []): void {
    clear(this.previewGroup);
    if (points.length >= 2) this.previewGroup.add(this.fatLine(points, 0x8fe3ff, 2.5, 1));
    const geo = new THREE.SphereGeometry(0.07, 12, 8);
    const mat = new THREE.MeshBasicMaterial({ color: 0x8fe3ff, depthTest: false });
    for (const p of extra) {
      const m = new THREE.Mesh(geo, mat);
      m.position.copy(v3(p));
      m.renderOrder = 11;
      this.previewGroup.add(m);
    }
    for (const [a, b] of handles) {
      this.previewGroup.add(new THREE.Line(new THREE.BufferGeometry().setFromPoints([v3(a), v3(b)]),
        new THREE.LineBasicMaterial({ color: 0x56ccf2 })));
    }
    this.vp.invalidate();
  }

  clearPreview(): void { this.setPreview([]); }

  // The listener figure (head and body) when it is drawn.
  pickableListener(): THREE.Object3D[] {
    if (!this.listener.visible) return [];
    const out: THREE.Object3D[] = [];
    this.listener.traverse((o) => { if ((o as THREE.Mesh).isMesh) out.push(o); });
    return out;
  }

  pickableLayers(): THREE.Object3D[] {
    const out: THREE.Object3D[] = [];
    for (const l of this.layers) {
      out.push(l.ball);
      if (l.right.ball.visible) out.push(l.right.ball, l.hub, l.bar);
      if (l.sphere.group.visible) out.push(l.sphere.handle);
    }
    return out;
  }
}

function makeLabel(text: string, cls: string): CSS2DObject {
  const div = document.createElement('div');
  div.className = cls;
  div.textContent = text;
  return new CSS2DObject(div);
}

function endMarker(p: V3, color: number, size = 0.12): THREE.Mesh {
  const m = new THREE.Mesh(new THREE.OctahedronGeometry(size), new THREE.MeshBasicMaterial({ color }));
  m.position.copy(v3(p));
  return m;
}

function disposeLayer(l: LayerObj): void {
  l.label.element.remove();
  l.leftTag.element.remove();
  l.right.tag.element.remove();
}

function clear(g: THREE.Object3D): void {
  for (const c of [...g.children]) {
    c.traverse((o) => {
      if (o instanceof CSS2DObject) o.element.remove();
      const m = o as THREE.Mesh;
      if (m.geometry) m.geometry.dispose();
    });
    g.remove(c);
  }
}
