// Pointer interaction in the viewport: selecting and dragging layers and path
// points, and the path drawing tools.
//
// Tools: select (move layers and path points; Alt+click on the path inserts a
// point, Delete removes), freehand, point-to-point (straight lines), curve
// (smooth through clicked points), pen (Bezier: click for a corner, drag for
// a smooth point with handles), and shapes (line, circle, ellipse, figure-8,
// spiral, helix: drag out from the centre).
import * as THREE from 'three';
import type { Store } from '../model/store';
import type { PathDoc, SegmentDoc, V3 } from '../model/scene';
import { stereoEnds, stereoFromEnds, defaultStereo, isStereo, isAmbisonic, defaultAmbisonic, ambisonicSurfacePoint } from '../model/scene';
import {
  appendSegments, circle, deletePoint, ellipse, evaluateSegment, figure8, fitFreehand, helix, insertPoint, movePoint,
  round3, samplePath, snap, spiral, getPoint, dist, type PointRef,
} from '../model/geometry';
import type { Viewport } from './viewport';
import type { PointHandle, SceneView } from './sceneView';

export type ShapeKind = 'line' | 'circle' | 'ellipse' | 'figure8' | 'spiral' | 'helix';
export type ToolName = 'select' | 'freehand' | 'polyline' | 'curve' | 'pen' | 'shape';

export interface ToolOptions {
  tool: ToolName;
  shape: ShapeKind;
  grid: number;          // snap step in metres, 0 = off
  append: boolean;       // add to the active path instead of starting a new one
  spiralTurns: number;
  helixTurns: number;
  helixRise: number;
}

interface PenAnchor { p: V3; hin: V3; hout: V3 }

type Drag =
  // `end`: which part of the layer is held: the centre (a mono layer's ball,
  // or a stereo layer's bar), the left/right end of a stereo pair, or the
  // handle on an Ambisonic sphere's surface (sets its radius). `mirror` (Alt
  // held) keeps the centre fixed and moves the other end the opposite way.
  | { kind: 'layer'; index: number; end: 'centre' | 'L' | 'R' | 'radius'; mirror: boolean; plane: THREE.Plane; offset: THREE.Vector3 }
  | { kind: 'point'; path: number; ref: PointRef; plane: THREE.Plane; offset: THREE.Vector3 }
  | { kind: 'freehand'; points: V3[] }
  | { kind: 'shape'; start: V3; end: V3 }
  | { kind: 'pen'; anchor: PenAnchor };

const vec = (v: THREE.Vector3): V3 => [v.x, v.y, v.z];

export class Interaction {
  opts: ToolOptions = { tool: 'select', shape: 'circle', grid: 0, append: false, spiralTurns: 3, helixTurns: 3, helixRise: 2 };
  private drag: Drag | null = null;
  private clicks: V3[] = [];            // polyline / curve points so far
  private pen: PenAnchor[] = [];
  private hover: V3 | null = null;
  onToolChange: () => void = () => {};
  blockDelete: () => boolean = () => false;

  // A point-to-point, curve or pen line is being drawn (Enter finishes it).
  get drawing(): boolean { return this.clicks.length > 0 || this.pen.length > 0; }

  constructor(private vp: Viewport, private view: SceneView, private store: Store) {
    const el = vp.renderer.domElement;
    // Capture phase on the container so a hit can switch orbit controls off
    // before they see the same pointerdown.
    vp.container.addEventListener('pointerdown', (e) => this.down(e), { capture: true });
    window.addEventListener('pointermove', (e) => this.move(e));
    window.addEventListener('pointerup', (e) => this.up(e));
    el.addEventListener('dblclick', (e) => this.dblclick(e));
    el.addEventListener('contextmenu', (e) => e.preventDefault());
    window.addEventListener('keydown', (e) => this.key(e));
    this.applyControls();
  }

  setTool(tool: ToolName, shape?: ShapeKind): void {
    this.cancelDrawing();
    this.opts.tool = tool;
    if (shape) this.opts.shape = shape;
    this.applyControls();
    this.onToolChange();
    this.store.emit('tool');
  }

  // Left button orbits/pans only with the select tool; drawing tools keep
  // right-drag to pan and the wheel to zoom.
  applyControls(): void {
    const c = this.vp.controls;
    const drawing = this.opts.tool !== 'select';
    c.mouseButtons = {
      LEFT: drawing ? (null as unknown as THREE.MOUSE) : this.vp.isOrtho ? THREE.MOUSE.PAN : THREE.MOUSE.ROTATE,
      MIDDLE: THREE.MOUSE.DOLLY,
      RIGHT: THREE.MOUSE.PAN,
    };
    this.vp.container.classList.toggle('drawing', drawing);
  }

  private get drawHeight(): number { return this.store.scene.editor?.draw_height ?? 1.7; }

  // Where the pointer meets the drawing plane (horizontal at the draw height,
  // or the view plane in front/side views).
  private drawPoint(e: PointerEvent | MouseEvent, doSnap = true): V3 | null {
    const plane = this.vp.editPlane(new THREE.Vector3(0, this.drawHeight, 0));
    const p = this.vp.intersect(e, plane);
    if (!p) return null;
    let v = round3(vec(p));
    if (doSnap && this.opts.grid > 0) v = snap(v, this.opts.grid, !(this.vp.view === 'front' || this.vp.view === 'side'));
    return v;
  }

  // ------------------------------------------------------------ events

  private down(e: PointerEvent): void {
    if (e.button !== 0 || e.target !== this.vp.renderer.domElement) return;
    const tool = this.opts.tool;
    if (tool === 'select') return this.selectDown(e);
    const p = this.drawPoint(e, tool !== 'freehand');
    if (!p) return;
    this.vp.controls.enabled = false;
    if (tool === 'freehand') this.drag = { kind: 'freehand', points: [p] };
    else if (tool === 'shape') this.drag = { kind: 'shape', start: p, end: p };
    else if (tool === 'pen') {
      if (this.pen.length >= 2 && dist(p, this.pen[0].p) < this.closeRadius(p)) {
        this.finishPen(true);
        return;
      }
      this.drag = { kind: 'pen', anchor: { p, hin: p, hout: p } };
    } else if (tool === 'polyline' || tool === 'curve') {
      if (this.clicks.length >= 2 && dist(p, this.clicks[0]) < this.closeRadius(p)) {
        this.finishClicks(true);
        return;
      }
      if (!this.clicks.length || dist(p, this.clicks[this.clicks.length - 1]) > 0.01) this.clicks.push(p);
      this.updatePreview();
    }
  }

  private closeRadius(p: V3): number {
    return 12 / Math.max(1e-3, this.vp.pixelsPerMetre(new THREE.Vector3(...p)));
  }

  private selectDown(e: PointerEvent): void {
    // Path points first (they sit on top), then layers, then the path line.
    let best: { h: PointHandle; d: number } | null = null;
    for (const h of this.view.pointHandles) {
      const d = this.vp.screenDistance(e, h.mesh.position);
      if (d < 10 && (!best || d < best.d)) best = { h, d };
    }
    if (best) {
      const { path, ref } = best.h;
      const pos = best.h.mesh.position.clone();
      const plane = this.vp.editPlane(pos, e.shiftKey);
      const hit = this.vp.intersect(e, plane) ?? pos.clone();
      this.drag = { kind: 'point', path, ref, plane, offset: pos.clone().sub(hit) };
      this.store.select({ kind: 'point', path, ref });
      this.vp.controls.enabled = false;
      return;
    }
    const rc = this.vp.ray(e);
    const hits = rc.intersectObjects(this.view.pickableLayers(), false);
    if (hits.length) {
      const index = hits[0].object.userData.index as number;
      const l = this.store.scene.layers[index];
      let end = (hits[0].object.userData.end as 'centre' | 'L' | 'R' | 'radius' | undefined) ?? 'centre';
      if (end === 'radius' ? !isAmbisonic(l) : !isStereo(l) || l.stereo?.mono) end = 'centre';
      const [left, right] = stereoEnds(l);
      const pos = new THREE.Vector3(...(end === 'L' ? left : end === 'R' ? right : end === 'radius' ? ambisonicSurfacePoint(l, [1, 0, 0]) : l.position));
      const plane = this.vp.editPlane(pos, e.shiftKey);
      const hit = this.vp.intersect(e, plane) ?? pos.clone();
      this.drag = { kind: 'layer', index, end, mirror: e.altKey, plane, offset: pos.clone().sub(hit) };
      this.store.select({ kind: 'layer', index });
      this.vp.controls.enabled = false;
      return;
    }
    if (e.altKey) {
      const L = this.store.scene.listener;
      const path = L.paths[L.active_path];
      const p = this.drawPoint(e, false);
      if (path && p) {
        let ref: PointRef | null = null;
        this.store.update((s) => { ref = insertPoint(s.listener.paths[s.listener.active_path], this.nearestHeight(path, p)); });
        if (ref) this.store.select({ kind: 'point', path: L.active_path, ref });
        return;
      }
    }
    // Clicking a path line makes it the active path.
    const lineHits = rc.intersectObjects(this.view.pathGroup.children, false).filter((h) => h.object.userData.kind === 'path');
    if (lineHits.length) {
      const idx = lineHits[0].object.userData.path as number;
      if (idx !== this.store.scene.listener.active_path) {
        this.store.update((s) => { s.listener.active_path = idx; });
        return;
      }
    }
    this.store.select({ kind: 'none' });
  }

  // The drawing plane is at the draw height; a point inserted on a path that
  // runs at another height should land on the path, so use the nearest
  // sample's height.
  private nearestHeight(path: PathDoc, p: V3): V3 {
    let best = samplePath(path)[0], bd = Infinity;
    for (const q of samplePath(path)) {
      const d = Math.hypot(q[0] - p[0], q[2] - p[2]);
      if (d < bd) { bd = d; best = q; }
    }
    return [p[0], best ? best[1] : p[1], p[2]];
  }

  private move(e: PointerEvent): void {
    const d = this.drag;
    if (!d) {
      if (this.opts.tool === 'polyline' || this.opts.tool === 'curve' || this.opts.tool === 'pen') {
        if (e.target !== this.vp.renderer.domElement) return;
        this.hover = this.drawPoint(e);
        this.updatePreview();
      }
      return;
    }
    switch (d.kind) {
      case 'layer': {
        const hit = this.vp.intersect(e, d.plane);
        if (!hit) return;
        let p = vec(hit.add(d.offset));
        p = this.snapEdit(p, d.plane);
        this.store.update((s) => {
          const l = s.layers[d.index];
          if (d.end === 'centre') {
            l.position = this.keepAxes(l.position, p, d.plane);
            return;
          }
          if (d.end === 'radius') {
            // The handle's distance from the centre is the sphere's radius.
            const c = l.position;
            const r = Math.hypot(p[0] - c[0], p[1] - c[1], p[2] - c[2]);
            l.ambisonic = { ...(l.ambisonic ?? defaultAmbisonic()), radius: Math.max(0.25, Math.round(r * 20) / 20) };
            return;
          }
          // Moving one end of a stereo pair: the other end stays (or mirrors
          // about the centre with Alt); centre, width, rotation and elevation
          // follow from where the two ends are.
          const [left, right] = stereoEnds(l);
          const moved = this.keepAxes(d.end === 'L' ? left : right, p, d.plane);
          const c = l.position;
          const mirrored: V3 = [2 * c[0] - moved[0], 2 * c[1] - moved[1], 2 * c[2] - moved[2]];
          const other = d.mirror ? mirrored : (d.end === 'L' ? right : left);
          const r = d.end === 'L' ? stereoFromEnds(moved, other, l.stereo ?? defaultStereo()) : stereoFromEnds(other, moved, l.stereo ?? defaultStereo());
          l.position = r.position;
          l.stereo = r.stereo;
        }, `layer-move-${d.index}`);
        break;
      }
      case 'point': {
        const hit = this.vp.intersect(e, d.plane);
        if (!hit) return;
        let p = vec(hit.add(d.offset));
        p = this.snapEdit(p, d.plane);
        this.store.update((s) => {
          const path = s.listener.paths[d.path];
          const cur = getPoint(path, d.ref);
          if (cur) movePoint(path, d.ref, this.keepAxes(cur, p, d.plane));
        }, `point-move-${d.path}-${d.ref.seg}-${d.ref.pt}`);
        break;
      }
      case 'freehand': {
        const p = this.drawPoint(e, false);
        if (p && dist(p, d.points[d.points.length - 1]) > 0.03) d.points.push(p);
        this.view.setPreview(d.points);
        break;
      }
      case 'shape': {
        const p = this.drawPoint(e);
        if (p) d.end = p;
        const segs = this.shapeSegments(d.start, d.end);
        this.view.setPreview(segs ? samplePath({ name: '', closed: false, segments: segs }) : [], [d.start]);
        break;
      }
      case 'pen': {
        const p = this.drawPoint(e);
        if (!p) return;
        const a = d.anchor;
        a.hout = p;
        a.hin = round3([2 * a.p[0] - p[0], 2 * a.p[1] - p[1], 2 * a.p[2] - p[2]]);
        this.updatePreview(a);
        break;
      }
    }
  }

  private up(_e: PointerEvent): void {
    const d = this.drag;
    this.drag = null;
    this.vp.controls.enabled = this.vp.view !== 'listener';
    if (!d) return;
    switch (d.kind) {
      case 'layer':
      case 'point':
        this.store.endGesture();
        break;
      case 'freehand': {
        const seg = fitFreehand(d.points);
        this.view.clearPreview();
        if (seg) this.commit([seg], false);
        break;
      }
      case 'shape': {
        const segs = this.shapeSegments(d.start, d.end);
        this.view.clearPreview();
        const closed = this.opts.shape === 'circle' || this.opts.shape === 'ellipse' || this.opts.shape === 'figure8';
        if (segs) this.commit(segs, closed);
        break;
      }
      case 'pen':
        this.pen.push(d.anchor);
        this.updatePreview();
        break;
    }
  }

  private dblclick(_e: MouseEvent): void {
    if (this.opts.tool === 'polyline' || this.opts.tool === 'curve') this.finishClicks(false);
    else if (this.opts.tool === 'pen') this.finishPen(false);
  }

  private key(e: KeyboardEvent): void {
    const t = e.target as HTMLElement;
    if (t && (t.tagName === 'INPUT' || t.tagName === 'SELECT' || t.tagName === 'TEXTAREA')) return;
    const drawingClicks = this.clicks.length > 0 || this.pen.length > 0;
    if (e.key === 'Enter' && drawingClicks) {
      if (this.opts.tool === 'pen') this.finishPen(false); else this.finishClicks(false);
      e.preventDefault();
    } else if (e.key === 'Escape') {
      if (drawingClicks || this.drag) this.cancelDrawing();
      else if (this.opts.tool !== 'select') this.setTool('select');
      else this.store.select({ kind: 'none' });
    } else if ((e.key === 'Backspace' || e.key === 'Delete') && drawingClicks) {
      if (this.opts.tool === 'pen') this.pen.pop(); else this.clicks.pop();
      this.updatePreview();
      e.preventDefault();
    } else if ((e.key === 'Delete' || e.key === 'Backspace') && !this.blockDelete()) {
      this.deleteSelection();
      e.preventDefault();
    }
  }

  deleteSelection(): void {
    const sel = this.store.selection;
    if (sel.kind === 'point') {
      this.store.update((s) => {
        const path = s.listener.paths[sel.path];
        if (path && !deletePoint(path, sel.ref)) {
          // Last segment of the path: remove the whole path.
          s.listener.paths.splice(sel.path, 1);
          s.listener.active_path = Math.max(0, Math.min(s.listener.active_path, s.listener.paths.length - 1));
        }
      });
      this.store.select({ kind: 'none' });
    } else if (sel.kind === 'layer') {
      this.store.update((s) => {
        s.layers.splice(sel.index, 1);
        if (s.listener.head.look_at_layer === sel.index) s.listener.head.look_at_layer = -1;
        else if (s.listener.head.look_at_layer > sel.index) s.listener.head.look_at_layer--;
      });
      this.store.select({ kind: 'none' });
    }
  }

  // --------------------------------------------------------- drawing

  private cancelDrawing(): void {
    this.clicks = [];
    this.pen = [];
    this.hover = null;
    if (this.drag && (this.drag.kind === 'freehand' || this.drag.kind === 'shape' || this.drag.kind === 'pen')) this.drag = null;
    this.view.clearPreview();
  }

  private updatePreview(dragging?: PenAnchor): void {
    if (this.opts.tool === 'pen') {
      const anchors = dragging ? [...this.pen, dragging] : this.pen;
      const segs = penSegments(anchors);
      const pts = segs.length ? samplePath({ name: '', closed: false, segments: segs }) : anchors.map((a) => a.p);
      if (!dragging && this.hover && anchors.length) pts.push(this.hover);
      const handles: [V3, V3][] = [];
      for (const a of anchors) if (dist(a.hout, a.p) > 1e-3) handles.push([a.hin, a.hout]);
      this.view.setPreview(pts, anchors.map((a) => a.p), handles);
      return;
    }
    const pts = this.hover ? [...this.clicks, this.hover] : this.clicks;
    const seg = this.clickSegments(pts);
    this.view.setPreview(seg.length ? samplePath({ name: '', closed: false, segments: seg }) : pts, this.clicks);
  }

  private clickSegments(pts: V3[]): SegmentDoc[] {
    if (pts.length < 2) return [];
    if (this.opts.tool === 'polyline') {
      const segs: SegmentDoc[] = [];
      for (let i = 0; i + 1 < pts.length; i++) segs.push({ type: 'line', points: [pts[i], pts[i + 1]] });
      return segs;
    }
    return [{ type: pts.length === 2 ? 'line' : 'catmull_rom', points: pts.slice() }];
  }

  private finishClicks(close: boolean): void {
    const pts = this.clicks.slice();
    this.clicks = [];
    this.hover = null;
    this.view.clearPreview();
    if (close && pts.length >= 2) pts.push(pts[0]);
    const segs = this.clickSegments(pts);
    if (segs.length) this.commit(segs, close);
  }

  private finishPen(close: boolean): void {
    const anchors = this.pen.slice();
    this.pen = [];
    this.hover = null;
    this.view.clearPreview();
    if (close && anchors.length >= 2) anchors.push(anchors[0]);
    const segs = penSegments(anchors);
    if (segs.length) this.commit(segs, close);
  }

  private shapeSegments(a: V3, b: V3): SegmentDoc[] | null {
    const dx = b[0] - a[0], dz = b[2] - a[2], dy = b[1] - a[1];
    const r = Math.hypot(dx, dy, dz);
    if (r < 0.1) return null;
    const o = this.opts;
    switch (o.shape) {
      case 'line': return [{ type: 'line', points: [a, b] }];
      case 'circle': return circle(a, r);
      case 'ellipse': return Math.abs(dx) < 0.05 || Math.abs(dz) < 0.05 ? null : ellipse(a, Math.abs(dx), Math.abs(dz));
      case 'figure8': return Math.abs(dx) < 0.05 ? null : figure8(a, Math.abs(dx), Math.max(Math.abs(dz), 0.3 * Math.abs(dx)));
      case 'spiral': return spiral(a, r, o.spiralTurns);
      case 'helix': return helix(a, r, o.helixTurns, o.helixRise);
    }
  }

  // Adds drawn segments as a new path (or onto the active one) and makes it active.
  private commit(segs: SegmentDoc[], closed: boolean): void {
    this.store.update((s) => {
      const L = s.listener;
      const active = L.paths[L.active_path];
      if (this.opts.append && active && !active.closed) {
        appendSegments(active, segs);
        active.closed = closed;
      } else {
        const name = `Path ${L.paths.length + 1}`;
        L.paths.push({ name, closed, segments: segs });
        L.active_path = L.paths.length - 1;
      }
    });
  }

  // --------------------------------------------------------- helpers

  private snapEdit(p: V3, plane: THREE.Plane): V3 {
    if (this.opts.grid <= 0) return round3(p);
    const vertical = Math.abs(plane.normal.y) < 0.5;
    return snap(p, this.opts.grid, !vertical);
  }

  // Dragging in a plane changes only the coordinates in that plane.
  private keepAxes(cur: V3, p: V3, plane: THREE.Plane): V3 {
    const n = plane.normal;
    if (Math.abs(n.y) > 0.99) return round3([p[0], cur[1], p[2]]);
    if (Math.abs(n.z) > 0.99) return round3([p[0], p[1], cur[2]]);
    if (Math.abs(n.x) > 0.99) return round3([cur[0], p[1], p[2]]);
    // Camera-facing vertical plane (Shift+drag): height only.
    return round3([cur[0], p[1], cur[2]]);
  }
}

function penSegments(anchors: PenAnchor[]): SegmentDoc[] {
  const segs: SegmentDoc[] = [];
  for (let i = 0; i + 1 < anchors.length; i++) {
    const a = anchors[i], b = anchors[i + 1];
    const straight = dist(a.hout, a.p) < 1e-3 && dist(b.hin, b.p) < 1e-3;
    segs.push(straight ? { type: 'line', points: [a.p, b.p] } : { type: 'bezier', points: [a.p, a.hout, b.hin, b.p] });
  }
  return segs;
}

export { evaluateSegment };
