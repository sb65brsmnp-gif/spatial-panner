// Three.js viewport: renderer, the four editing views plus a listener's-eye
// view, orbit/pan/zoom, and picking helpers.
import * as THREE from 'three';
import { OrbitControls } from 'three/examples/jsm/controls/OrbitControls.js';
import { CSS2DRenderer } from 'three/examples/jsm/renderers/CSS2DRenderer.js';

export type ViewName = 'persp' | 'top' | 'front' | 'side' | 'listener';

export class Viewport {
  readonly renderer: THREE.WebGLRenderer;
  readonly labels: CSS2DRenderer;
  readonly scene = new THREE.Scene();
  readonly persp: THREE.PerspectiveCamera;
  readonly ortho: THREE.OrthographicCamera;
  readonly eye: THREE.PerspectiveCamera;      // listener's-eye view
  readonly controls: OrbitControls;
  view: ViewName = 'persp';
  private orthoHalfHeight = 10;
  private needsRender = true;
  private beforeRender: (() => void)[] = [];

  constructor(readonly container: HTMLElement) {
    this.renderer = new THREE.WebGLRenderer({ antialias: true, preserveDrawingBuffer: true });
    this.renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2));
    this.renderer.setClearColor(0x15171c);
    container.appendChild(this.renderer.domElement);
    this.labels = new CSS2DRenderer();
    this.labels.domElement.className = 'label-layer';
    container.appendChild(this.labels.domElement);

    this.persp = new THREE.PerspectiveCamera(50, 1, 0.05, 2000);
    this.persp.position.set(14, 14, 18);
    this.ortho = new THREE.OrthographicCamera(-10, 10, 10, -10, -1000, 1000);
    this.eye = new THREE.PerspectiveCamera(80, 1, 0.05, 2000);

    this.controls = new OrbitControls(this.persp, this.renderer.domElement);
    this.controls.target.set(0, 1, 0);
    this.controls.enableDamping = false;
    this.controls.screenSpacePanning = true;
    this.controls.addEventListener('change', () => this.invalidate());

    this.scene.add(new THREE.HemisphereLight(0xdfe8ff, 0x30302a, 1.6));
    const sun = new THREE.DirectionalLight(0xffffff, 1.4);
    sun.position.set(5, 12, 8);
    this.scene.add(sun);

    new ResizeObserver(() => this.resize()).observe(container);
    this.resize();
    const loop = () => {
      if (this.needsRender) {
        this.needsRender = false;
        for (const f of this.beforeRender) f();
        const cam = this.camera;
        this.renderer.render(this.scene, cam);
        this.labels.render(this.scene, cam);
      }
      requestAnimationFrame(loop);
    };
    requestAnimationFrame(loop);
  }

  get camera(): THREE.Camera {
    return this.view === 'persp' ? this.persp : this.view === 'listener' ? this.eye : this.ortho;
  }

  get isOrtho(): boolean { return this.view === 'top' || this.view === 'front' || this.view === 'side'; }

  invalidate(): void { this.needsRender = true; }

  onBeforeRender(f: () => void): void { this.beforeRender.push(f); }

  resize(): void {
    const w = Math.max(1, this.container.clientWidth), h = Math.max(1, this.container.clientHeight);
    this.renderer.setSize(w, h);
    this.labels.setSize(w, h);
    this.persp.aspect = w / h;
    this.persp.updateProjectionMatrix();
    this.eye.aspect = w / h;
    this.eye.updateProjectionMatrix();
    this.updateOrthoFrustum();
    this.invalidate();
  }

  private updateOrthoFrustum(): void {
    const w = Math.max(1, this.container.clientWidth), h = Math.max(1, this.container.clientHeight);
    const hh = this.orthoHalfHeight / this.ortho.zoom;
    void hh;
    const a = w / h;
    this.ortho.left = -this.orthoHalfHeight * a;
    this.ortho.right = this.orthoHalfHeight * a;
    this.ortho.top = this.orthoHalfHeight;
    this.ortho.bottom = -this.orthoHalfHeight;
    this.ortho.updateProjectionMatrix();
  }

  // Frames a box of `extent` metres around `centre` in the current view.
  setView(view: ViewName, centre = new THREE.Vector3(0, 1, 0), extent = 20): void {
    this.view = view;
    const c = this.controls;
    if (view === 'persp') {
      c.object = this.persp;
      c.enableRotate = true;
      c.enabled = true;
    } else if (view === 'listener') {
      c.enabled = false;
    } else {
      c.object = this.ortho;
      c.enableRotate = false;
      c.enabled = true;
      this.orthoHalfHeight = extent * 0.6;
      this.ortho.zoom = 1;
      const d = 100;
      if (view === 'top') { this.ortho.position.set(centre.x, centre.y + d, centre.z); this.ortho.up.set(0, 0, -1); }
      if (view === 'front') { this.ortho.position.set(centre.x, centre.y, centre.z + d); this.ortho.up.set(0, 1, 0); }
      if (view === 'side') { this.ortho.position.set(centre.x + d, centre.y, centre.z); this.ortho.up.set(0, 1, 0); }
      c.target.copy(centre);
      this.ortho.lookAt(centre);
      this.updateOrthoFrustum();
    }
    c.update();
    this.invalidate();
  }

  // Places the listener's-eye camera.
  setEye(pos: THREE.Vector3, quat: THREE.Quaternion): void {
    this.eye.position.copy(pos);
    this.eye.quaternion.copy(quat);
    if (this.view === 'listener') this.invalidate();
  }

  ndc(e: PointerEvent | MouseEvent): THREE.Vector2 {
    const r = this.renderer.domElement.getBoundingClientRect();
    return new THREE.Vector2(((e.clientX - r.left) / r.width) * 2 - 1, -((e.clientY - r.top) / r.height) * 2 + 1);
  }

  ray(e: PointerEvent | MouseEvent): THREE.Raycaster {
    const rc = new THREE.Raycaster();
    rc.setFromCamera(this.ndc(e), this.camera);
    rc.params.Line = { threshold: 0.15 };
    return rc;
  }

  // The plane edits happen in, for a point at `through`: horizontal in the
  // top and perspective views (vertical when `vertical`), the view plane in
  // front and side views.
  editPlane(through: THREE.Vector3, vertical = false): THREE.Plane {
    if (this.view === 'front') return new THREE.Plane(new THREE.Vector3(0, 0, 1), -through.z);
    if (this.view === 'side') return new THREE.Plane(new THREE.Vector3(1, 0, 0), -through.x);
    if (vertical) {
      // Vertical plane facing the camera.
      const dir = new THREE.Vector3();
      this.camera.getWorldDirection(dir);
      dir.y = 0;
      if (dir.lengthSq() < 1e-6) dir.set(0, 0, -1);
      dir.normalize();
      return new THREE.Plane().setFromNormalAndCoplanarPoint(dir, through);
    }
    return new THREE.Plane(new THREE.Vector3(0, 1, 0), -through.y);
  }

  intersect(e: PointerEvent | MouseEvent, plane: THREE.Plane): THREE.Vector3 | null {
    const out = new THREE.Vector3();
    return this.ray(e).ray.intersectPlane(plane, out) ? out : null;
  }

  // Pixels per metre at `p` (for screen-space hit tolerances).
  pixelsPerMetre(p: THREE.Vector3): number {
    const cam = this.camera;
    const a = p.clone().project(cam);
    const right = new THREE.Vector3(1, 0, 0).applyQuaternion((cam as THREE.PerspectiveCamera).quaternion);
    const b = p.clone().add(right).project(cam);
    return Math.abs(b.x - a.x) * 0.5 * this.container.clientWidth;
  }

  screenDistance(e: PointerEvent | MouseEvent, p: THREE.Vector3): number {
    const r = this.renderer.domElement.getBoundingClientRect();
    const s = p.clone().project(this.camera);
    if (s.z > 1) return Infinity;
    const x = (s.x * 0.5 + 0.5) * r.width + r.left, y = (-s.y * 0.5 + 0.5) * r.height + r.top;
    return Math.hypot(e.clientX - x, e.clientY - y);
  }
}
