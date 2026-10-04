import { describe, expect, it } from 'vitest';
import {
  appendSegments, circle, closedSmooth, deletePoint, evaluateSegment, fitFreehand, insertPoint, jointPoints, movePoint, samplePath, simplify,
} from '../../src/model/geometry';
import type { PathDoc, V3 } from '../../src/model/scene';
import { applyEasing, engineScene, defaultScene, defaultLayer, speedAt, headKeyAt, mergeEditorKeys } from '../../src/model/scene';

const pathLength = (p: PathDoc) => {
  const s = samplePath(p, 400);
  let l = 0;
  for (let i = 1; i < s.length; i++) l += Math.hypot(s[i][0] - s[i - 1][0], s[i][1] - s[i - 1][1], s[i][2] - s[i - 1][2]);
  return l;
};

describe('simplify / freehand', () => {
  it('keeps the corners of a noisy L', () => {
    const pts: V3[] = [];
    for (let i = 0; i <= 50; i++) pts.push([i * 0.1, 1.7, (i % 2) * 0.01]);
    for (let i = 1; i <= 50; i++) pts.push([5, 1.7, i * 0.1]);
    const s = simplify(pts, 0.05);
    expect(s.length).toBe(3);
    expect(s[1][0]).toBeCloseTo(5);
  });
  it('fits a stroke with a Catmull-Rom through the simplified points', () => {
    const stroke: V3[] = [];
    for (let i = 0; i <= 100; i++) stroke.push([Math.cos(i / 100 * Math.PI) * 3, 1.7, Math.sin(i / 100 * Math.PI) * 3]);
    const seg = fitFreehand(stroke)!;
    expect(seg.type).toBe('catmull_rom');
    expect(seg.points.length).toBeLessThan(20);
    // The fitted curve stays within a few cm of the stroke's circle.
    for (let u = 0; u <= 1; u += 0.05) {
      const p = evaluateSegment(seg, u);
      expect(Math.abs(Math.hypot(p[0], p[2]) - 3)).toBeLessThan(0.1);
    }
  });
  it('ignores a click without movement', () => {
    expect(fitFreehand([[0, 1.7, 0], [0.001, 1.7, 0]])).toBeNull();
  });
});

describe('shapes', () => {
  it('circle has the right circumference and closes', () => {
    const p: PathDoc = { name: '', closed: true, segments: circle([0, 1.7, 0], 2) };
    expect(pathLength(p)).toBeCloseTo(2 * Math.PI * 2, 1);
    const a = evaluateSegment(p.segments[0], 0), b = evaluateSegment(p.segments[3], 1);
    expect(Math.hypot(a[0] - b[0], a[2] - b[2])).toBeLessThan(1e-6);
  });
  it('closed smooth loop is continuous in tangent at the join', () => {
    const segs = closedSmooth([[0, 0, 0], [2, 0, 0], [2, 0, 2], [0, 0, 2]]);
    const last = segs[segs.length - 1].points, first = segs[0].points;
    const t1 = [last[3][0] - last[2][0], last[3][2] - last[2][2]], t2 = [first[1][0] - first[0][0], first[1][2] - first[0][2]];
    expect(t1[0] * t2[1] - t1[1] * t2[0]).toBeCloseTo(0);
  });
});

describe('point editing', () => {
  const poly = (): PathDoc => ({ name: '', closed: false, segments: [
    { type: 'line', points: [[0, 0, 0], [1, 0, 0]] }, { type: 'line', points: [[1, 0, 0], [1, 0, 1]] }] });

  it('moving a joint moves both segment ends', () => {
    const p = poly();
    expect(jointPoints(p, { seg: 0, pt: 1 }).length).toBe(2);
    movePoint(p, { seg: 0, pt: 1 }, [2, 0, 0]);
    expect(p.segments[1].points[0]).toEqual([2, 0, 0]);
  });
  it('moving a Bezier anchor carries its handles', () => {
    const p: PathDoc = { name: '', closed: false, segments: [{ type: 'bezier', points: [[0, 0, 0], [1, 0, 0], [2, 0, 1], [3, 0, 1]] }] };
    movePoint(p, { seg: 0, pt: 3 }, [3, 0, 2]);
    expect(p.segments[0].points[2]).toEqual([2, 0, 2]);
  });
  it('a smooth handle mirrors across its anchor', () => {
    const p: PathDoc = { name: '', closed: false, segments: [
      { type: 'bezier', points: [[0, 0, 0], [0, 0, 1], [1, 0, 1], [2, 0, 1]] },
      { type: 'bezier', points: [[2, 0, 1], [3, 0, 1], [4, 0, 1], [4, 0, 0]] }] };
    movePoint(p, { seg: 0, pt: 2 }, [2, 0, 0]);  // handle now points down from the anchor
    expect(p.segments[1].points[1][2]).toBeCloseTo(2);  // the other one points up, same length (1)
    expect(p.segments[1].points[1][0]).toBeCloseTo(2);
  });
  it('insert splits a line and delete merges it back', () => {
    const p = poly();
    const ref = insertPoint(p, [0.5, 0, 0.1])!;
    expect(p.segments.length).toBe(3);
    expect(p.segments[0].points[1][0]).toBeCloseTo(0.5);
    expect(deletePoint(p, ref)).toBe(true);
    expect(p.segments.length).toBe(2);
    expect(p.segments[0].points).toEqual([[0, 0, 0], [1, 0, 0]]);
  });
  it('insert on a Bezier keeps the curve shape', () => {
    const seg = { type: 'bezier' as const, points: [[0, 0, 0], [0, 0, 2], [2, 0, 2], [2, 0, 0]] as V3[] };
    const p: PathDoc = { name: '', closed: false, segments: [JSON.parse(JSON.stringify(seg))] };
    const before = pathLength(p);
    insertPoint(p, [1, 0, 1.5]);
    expect(p.segments.length).toBe(2);
    expect(pathLength(p)).toBeCloseTo(before, 2);
  });
  it('appending joins with a line when the stroke starts elsewhere', () => {
    const p = poly();
    appendSegments(p, [{ type: 'line', points: [[3, 0, 3], [4, 0, 4]] }]);
    expect(p.segments.length).toBe(4);
    expect(p.segments[2].points).toEqual([[1, 0, 1], [3, 0, 3]]);
  });
});

describe('scene helpers', () => {
  it('easing and speed match the engine', () => {
    expect(applyEasing('smooth', 0.5)).toBeCloseTo(0.5);
    expect(applyEasing('ease_in', 0.5)).toBeCloseTo(0.25);
    expect(applyEasing('hold', 0.9)).toBe(0);
    const keys = [{ time: 0, speed: 0, easing: 'linear' as const }, { time: 2, speed: 2, easing: 'linear' as const }];
    expect(speedAt(keys, 1)).toBeCloseTo(1);
    expect(speedAt([], 5)).toBeCloseTo(1.4);
    expect(headKeyAt([{ time: 0, yaw: 0, pitch: 0, roll: 0, easing: 'linear' }, { time: 1, yaw: 90, pitch: 10, roll: 0, easing: 'linear' }], 0.5).yaw).toBeCloseTo(45);
  });
  it('solo mutes everything else for the engine only', () => {
    const s = defaultScene();
    s.layers = [defaultLayer(0, { solo: true }), defaultLayer(1)];
    const e = engineScene(s);
    expect(e.layers[1].mute).toBe(true);
    expect(s.layers[1].mute).toBe(false);
  });
  it('keeps editor-only keys when the canonical scene comes back', () => {
    const s = defaultScene();
    s.layers = [defaultLayer(0)];
    const raw = { layers: [{ color: '#123456', solo: true }], editor: { draw_height: 2.5 } };
    const m = mergeEditorKeys(s, raw);
    expect(m.layers[0].color).toBe('#123456');
    expect(m.editor!.draw_height).toBe(2.5);
  });
});
