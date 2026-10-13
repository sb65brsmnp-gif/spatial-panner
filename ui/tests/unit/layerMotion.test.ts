import { describe, expect, it } from 'vitest';
import { completeScene, defaultLayer, defaultScene, levelAt, pathKeyAt, speedArrival, SILENT_DB, type SceneDoc } from '../../src/model/scene';
import { attachPath, fitTiming, layerArrival, layerOfPath, layerPathIndex, moveLayerTo, pathOf } from '../../src/model/layerMotion';

function sceneWithLayer(): SceneDoc {
  const s = defaultScene();
  s.layers.push(defaultLayer(0, { position: [1, 1.6, -2] }));
  return s;
}

describe('layer paths', () => {
  it('a drawn path starts at the layer, at its height', () => {
    const s = sceneWithLayer();
    attachPath(s.layers[0], [{ type: 'line', points: [[4, 1.7, 0], [4, 1.7, -10]] }], false, false, 2);
    const l = s.layers[0];
    expect(l.position).toEqual([4, 1.6, 0]);
    expect(l.motion!.path.segments[0].points).toEqual([[4, 1.6, 0], [4, 1.6, -10]]);
    expect(l.motion!.start_time).toBe(2);
    expect(l.motion!.timing).toBe('speed');
  });

  it('moving the layer moves its path', () => {
    const s = sceneWithLayer();
    attachPath(s.layers[0], [{ type: 'line', points: [[0, 1.6, 0], [0, 1.6, -10]] }], false, false, 0);
    moveLayerTo(s.layers[0], [2, 1.6, 1]);
    expect(s.layers[0].motion!.path.segments[0].points).toEqual([[2, 1.6, 1], [2, 1.6, -9]]);
  });

  it('layer paths are addressed by negative path indices', () => {
    const s = sceneWithLayer();
    attachPath(s.layers[0], [{ type: 'line', points: [[0, 1.6, 0], [0, 1.6, -10]] }], false, false, 0);
    expect(layerOfPath(layerPathIndex(0))).toBe(0);
    expect(layerOfPath(0)).toBe(-1);
    expect(pathOf(s, layerPathIndex(0))).toBe(s.layers[0].motion!.path);
  });

  it('old files without motion load unchanged; motion fields are completed', () => {
    const s = completeScene({ layers: [{ ...defaultLayer(0), motion: { path: { name: '', closed: false, segments: [{ type: 'line', points: [[0, 0, 0], [1, 0, 0]] }] } } as never }] });
    expect(s.layers[0].motion!.end).toBe('stop');
    expect(s.layers[0].motion!.speed.length).toBe(1);
    expect(completeScene({ layers: [defaultLayer(0)] }).layers[0].motion).toBeUndefined();
  });
});

describe('timing', () => {
  it('speed arrival matches the distance', () => {
    expect(speedArrival([{ time: 0, speed: 2, easing: 'linear' }], 10)).toBeCloseTo(5, 3);
    expect(speedArrival([{ time: 0, speed: 1, easing: 'linear' }, { time: 1, speed: 0, easing: 'linear' }], 10)).toBeNull();
  });

  it('fit makes the listener and layers start and end together', () => {
    const s = sceneWithLayer();
    s.layers.push(defaultLayer(1, { position: [0, 1.6, 0] }));
    attachPath(s.layers[0], [{ type: 'line', points: [[0, 1.6, 0], [0, 1.6, -10]] }], false, false, 0);
    attachPath(s.layers[1], [{ type: 'line', points: [[0, 1.6, 0], [20, 1.6, 0]] }], false, false, 3);
    s.layers[0].motion!.speed = [{ time: 0, speed: 1, easing: 'smooth' }, { time: 4, speed: 3, easing: 'linear' }];
    s.layers[1].motion!.timing = 'keys';
    s.layers[1].motion!.keys = [{ time: 3, fraction: 0, easing: 'smooth' }, { time: 5, fraction: 0.5, easing: 'smooth' }, { time: 9, fraction: 1, easing: 'smooth' }];
    s.listener.paths.push({ name: 'P', closed: false, segments: [{ type: 'line', points: [[0, 1.7, 0], [0, 1.7, 14]] }] });
    fitTiming(s, null, { listener: true, layers: [0, 1] }, 2, 12);
    expect(s.listener.path_start_time).toBe(2);
    expect(2 + speedArrival(s.listener.speed, 14)!).toBeCloseTo(12, 1);
    expect(layerArrival(s.layers[0], 10)).toBeCloseTo(12, 1);
    // The speed curve keeps its shape: the second key is still 3x the first.
    const k = s.layers[0].motion!.speed;
    expect(k[1].speed / k[0].speed).toBeCloseTo(3, 2);
    expect(s.layers[1].motion!.keys.map((x) => x.time)).toEqual([2, 5.333, 12]);
  });

  it('path keys and level keys interpolate like the engine', () => {
    expect(pathKeyAt([{ time: 0, fraction: 0, easing: 'linear' }, { time: 2, fraction: 1, easing: 'linear' }], 1)).toBeCloseTo(0.5);
    const keys = [{ time: 0, level_db: SILENT_DB, easing: 'linear' as const }, { time: 1, level_db: 0, easing: 'linear' as const }];
    // A fade from silence runs in gain: half way is -6 dB.
    expect(levelAt(keys, 0.5)).toBeCloseTo(-6.02, 1);
    expect(levelAt([{ time: 0, level_db: -20, easing: 'linear' }, { time: 1, level_db: 0, easing: 'linear' }], 0.5)).toBeCloseTo(-10);
  });
});
