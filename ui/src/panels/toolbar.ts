// Top toolbar: file, undo, drawing tools, views.
import type { Interaction, ShapeKind, ToolName } from '../view/interaction';
import type { ViewName } from '../view/viewport';
import { el } from './dom';

export interface ToolbarActions {
  newScene(): void;
  open(): void;
  recent(): Promise<string[]>;
  openRecent(path: string): void;
  clearRecent(): Promise<void>;
  save(saveAs: boolean): void;
  undo(): void;
  redo(): void;
  setView(v: ViewName): void;
  frame(): void;
  home(): void;
  toggleFollow(): void;
}

const SHAPES: [ShapeKind, string][] = [['line', 'Line'], ['circle', 'Circle'], ['ellipse', 'Ellipse'], ['figure8', 'Figure 8'], ['spiral', 'Spiral'], ['helix', 'Helix']];

export class Toolbar {
  readonly root: HTMLElement;
  private toolBtns = new Map<string, HTMLButtonElement>();
  private viewBtns = new Map<ViewName, HTMLButtonElement>();
  private followBtn: HTMLButtonElement;
  private undoBtn: HTMLButtonElement;
  private redoBtn: HTMLButtonElement;
  private shapeSel: HTMLSelectElement;
  readonly title: HTMLElement;

  // `plugin`: the scene lives in the host's project, so files are only
  // imported and exported.
  constructor(private tools: Interaction, plugin: boolean, actions: ToolbarActions) {
    this.root = el('div', { id: 'toolbar' });
    const b = (label: string, title: string, fn: () => void, cls = 'tbtn') => {
      const x = el('button', { class: cls, title }, label) as HTMLButtonElement;
      x.addEventListener('click', fn);
      return x;
    };
    const file = plugin
      ? el('div', { class: 'group' },
        b('Clear', 'Start over with an empty scene (the tracks stay as layers)', () => actions.newScene()),
        b('Import…', 'Replace this session\'s scene with a scene file', () => actions.open()),
        b('Export…', 'Save this session\'s scene to a file', () => actions.save(true)))
      : el('div', { class: 'group' },
        b('New', 'New scene', () => actions.newScene()),
        b('Open…', 'Open a scene (⌘O)', () => actions.open()),
        this.recentButton(actions),
        b('Save', 'Save (⌘S)', () => actions.save(false)),
        b('Save as…', 'Save as (⇧⌘S)', () => actions.save(true)));
    this.undoBtn = b('↶', 'Undo (⌘Z)', () => actions.undo());
    this.redoBtn = b('↷', 'Redo (⇧⌘Z)', () => actions.redo());
    const hist = el('div', { class: 'group' }, this.undoBtn, this.redoBtn);

    const toolDefs: [ToolName, string, string][] = [
      ['select', '⬚ Select', 'Select and move layers and path points (V)'],
      ['freehand', '✎ Freehand', 'Draw the path freehand (F)'],
      ['polyline', '⟋ Point to point', 'Click points joined by straight lines; double-click or Enter to finish (L)'],
      ['curve', '∿ Curve', 'Click points for a smooth curve through them; double-click or Enter to finish (C)'],
      ['pen', '✒ Pen', 'Bezier pen: click for a corner, drag for a smooth point; Enter to finish (P)'],
    ];
    const toolGroup = el('div', { class: 'group' });
    for (const [t, label, title] of toolDefs) {
      const x = b(label, title, () => tools.setTool(t));
      this.toolBtns.set(t, x);
      toolGroup.append(x);
    }
    const shapeBtn = b('◯ Shape', 'Drag out a shape from its centre (S)', () => tools.setTool('shape', this.shapeSel.value as ShapeKind));
    this.toolBtns.set('shape', shapeBtn);
    this.shapeSel = el('select', { class: 'shape-select', title: 'Shape' });
    for (const [k, label] of SHAPES) this.shapeSel.append(el('option', { value: k }, label));
    this.shapeSel.value = tools.opts.shape;
    this.shapeSel.addEventListener('change', () => tools.setTool('shape', this.shapeSel.value as ShapeKind));
    toolGroup.append(shapeBtn, this.shapeSel);

    const viewGroup = el('div', { class: 'group' });
    const views: [ViewName, string, string][] = [['persp', '3D', 'Orbit view (1)'], ['top', 'Top', 'Top view (2)'], ['front', 'Front', 'Front view (3)'],
      ['side', 'Side', 'Side view (4)'], ['listener', 'Ears', "Listener's view (5)"]];
    for (const [v, label, title] of views) {
      const x = b(label, title, () => actions.setView(v));
      this.viewBtns.set(v, x);
      viewGroup.append(x);
    }
    this.followBtn = b('Follow', 'Keep the listener in view while playing', () => actions.toggleFollow());
    viewGroup.append(b('Home', 'Back to the default 3D view, scene in frame (H)', () => actions.home()),
      b('Frame', 'Fit the scene in view', () => actions.frame()), this.followBtn);
    this.title = el('div', { class: 'doc-title' });
    this.root.append(file, hist, toolGroup, viewGroup, this.title);
  }

  // "Recent ▾": the scenes opened or saved lately, newest first.
  private recentButton(actions: ToolbarActions): HTMLElement {
    const wrap = el('div', { class: 'recent' });
    const btn = el('button', { class: 'tbtn', title: 'Open a recent scene' }, 'Recent ▾') as HTMLButtonElement;
    const menu = el('div', { class: 'recent-menu' });
    menu.style.display = 'none';
    const close = () => { menu.style.display = 'none'; };
    btn.addEventListener('click', async (e) => {
      e.stopPropagation();
      if (menu.style.display !== 'none') { close(); return; }
      const files = await actions.recent();
      menu.replaceChildren();
      if (!files.length) menu.append(el('div', { class: 'recent-empty' }, 'No recent scenes'));
      for (const f of files) {
        const name = f.split(/[\\/]/).pop() ?? f;
        const item = el('button', { class: 'recent-item', title: f }, name);
        item.addEventListener('click', () => { close(); actions.openRecent(f); });
        menu.append(item);
      }
      if (files.length) {
        const clear = el('button', { class: 'recent-item recent-clear' }, 'Clear Menu');
        clear.addEventListener('click', () => { close(); void actions.clearRecent(); });
        menu.append(clear);
      }
      menu.style.display = '';
    });
    window.addEventListener('pointerdown', (e) => { if (!wrap.contains(e.target as Node)) close(); });
    window.addEventListener('keydown', (e) => { if (e.key === 'Escape') close(); });
    wrap.append(btn, menu);
    return wrap;
  }

  refresh(state: { view: ViewName; follow: boolean; canUndo: boolean; canRedo: boolean; title: string }): void {
    for (const [t, x] of this.toolBtns) x.classList.toggle('active', t === this.tools.opts.tool);
    this.shapeSel.value = this.tools.opts.shape;
    for (const [v, x] of this.viewBtns) x.classList.toggle('active', v === state.view);
    this.followBtn.classList.toggle('active', state.follow);
    this.undoBtn.disabled = !state.canUndo;
    this.redoBtn.disabled = !state.canRedo;
    this.title.textContent = state.title;
  }
}
