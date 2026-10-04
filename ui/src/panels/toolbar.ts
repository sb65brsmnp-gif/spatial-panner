// Top toolbar: file, undo, drawing tools, views.
import type { Interaction, ShapeKind, ToolName } from '../view/interaction';
import type { ViewName } from '../view/viewport';
import { el } from './dom';

export interface ToolbarActions {
  newScene(): void;
  open(): void;
  save(saveAs: boolean): void;
  undo(): void;
  redo(): void;
  setView(v: ViewName): void;
  frame(): void;
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

  constructor(private tools: Interaction, actions: ToolbarActions) {
    this.root = el('div', { id: 'toolbar' });
    const b = (label: string, title: string, fn: () => void, cls = 'tbtn') => {
      const x = el('button', { class: cls, title }, label) as HTMLButtonElement;
      x.addEventListener('click', fn);
      return x;
    };
    const file = el('div', { class: 'group' },
      b('New', 'New scene', () => actions.newScene()),
      b('Open…', 'Open a scene (⌘O)', () => actions.open()),
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
    viewGroup.append(b('Frame', 'Fit the scene in view', () => actions.frame()), this.followBtn);
    this.title = el('div', { class: 'doc-title' });
    this.root.append(file, hist, toolGroup, viewGroup, this.title);
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
