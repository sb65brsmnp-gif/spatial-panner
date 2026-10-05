import { describe, expect, it } from 'vitest';
import { FINE_STEP, MIN_WALK, STEP, pathEndTime, pathStartTime, selectRange, shiftSelection, speedKeyAbsolute, speedKeyTime, steppedTime, stretchPathEnd, transportKeyAction } from '../../src/panels/timeline';
import { defaultScene } from '../../src/model/scene';
import type { Analysis } from '../../src/model/store';

const analysis = (arrival_time: number): Analysis => ({ duration: 30, dt: 0.1, active_path: 0, arrival_time, paths: [], poses: [] });

describe('transport keys', () => {
  it('Enter goes to the beginning of the path', () => {
    expect(transportKeyAction({ key: 'Enter', shiftKey: false })).toEqual({ kind: 'start' });
  });

  it(', and . step one second back and forward', () => {
    expect(transportKeyAction({ key: ',', shiftKey: false })).toEqual({ kind: 'step', seconds: -STEP });
    expect(transportKeyAction({ key: '.', shiftKey: false })).toEqual({ kind: 'step', seconds: STEP });
  });

  it('Shift makes a fine step, also when the layout reports < and >', () => {
    expect(transportKeyAction({ key: '<', shiftKey: true })).toEqual({ kind: 'step', seconds: -FINE_STEP });
    expect(transportKeyAction({ key: '>', shiftKey: true })).toEqual({ kind: 'step', seconds: FINE_STEP });
    expect(transportKeyAction({ key: ',', shiftKey: true })).toEqual({ kind: 'step', seconds: -FINE_STEP });
    expect(transportKeyAction({ key: '.', shiftKey: true })).toEqual({ kind: 'step', seconds: FINE_STEP });
  });

  it('leaves other keys alone', () => {
    for (const key of [' ', 'a', 'Escape', 'Delete', 'ArrowLeft', '/']) expect(transportKeyAction({ key, shiftKey: false })).toBeNull();
  });
});

describe('transport targets', () => {
  it('the beginning of the path is where the listener starts moving, 0 when unset', () => {
    const s = defaultScene();
    expect(pathStartTime(s)).toBe(0);
    s.listener.path_start_time = 2.5;
    expect(pathStartTime(s)).toBe(2.5);
    s.listener.path_start_time = -1;
    expect(pathStartTime(s)).toBe(0);
  });

  it('the end of the path is the arrival time, else the scene length', () => {
    expect(pathEndTime(analysis(22), 30)).toBe(22);
    expect(pathEndTime(analysis(0), 30)).toBe(30);
    expect(pathEndTime(null, 30)).toBe(30);
  });

  it('the end of the path never lies past the end of the scene', () => {
    expect(pathEndTime(analysis(22), 10)).toBe(10);
  });
});

describe('speed keys count from the start of the path', () => {
  it('converts between the timeline and key times', () => {
    const s = defaultScene();
    s.listener.path_start_time = 6;
    expect(speedKeyTime(s, 8)).toBe(2);
    expect(speedKeyAbsolute(s, 2)).toBe(8);
    expect(speedKeyTime(s, 3)).toBe(0);     // before the start: the first key
    expect(speedKeyTime(s, 8.02)).toBe(2);  // snapped to 0.05 s
  });

  it('is the identity without a start time', () => {
    const s = defaultScene();
    expect(speedKeyTime(s, 8)).toBe(8);
    expect(speedKeyAbsolute(s, 8)).toBe(8);
  });
});

describe('stepping the playhead', () => {
  it('steps and clamps to the scene', () => {
    expect(steppedTime(10, 1, 30)).toBe(11);
    expect(steppedTime(0.5, -1, 30)).toBe(0);
    expect(steppedTime(29.5, 1, 30)).toBe(30);
  });

  it('ten fine steps land exactly on the next second', () => {
    let t = 3;
    for (let i = 0; i < 10; i++) t = steppedTime(t, 0.1, 30);
    expect(t).toBe(4);
    for (let i = 0; i < 10; i++) t = steppedTime(t, -0.1, 30);
    expect(t).toBe(3);
  });
});

describe('dragging the end of the path', () => {
  const walk = () => {
    const s = defaultScene();
    s.listener.path_start_time = 2;
    s.listener.speed = [{ time: 0, speed: 1.4, easing: 'linear' }, { time: 4, speed: 0.7, easing: 'smooth' }];
    return s;
  };

  it('stretches the speed curve: key times scale, speeds scale the other way', () => {
    const s = walk();
    stretchPathEnd(s, 12, 7);   // a 10 s walk becomes a 5 s walk
    expect(s.listener.speed.map((k) => [k.time, k.speed])).toEqual([[0, 2.8], [2, 1.4]]);
    expect(s.listener.speed[1].easing).toBe('smooth');
    expect(s.listener.path_start_time).toBe(2);
  });

  it('never brings the end closer than MIN_WALK to the start', () => {
    const s = walk();
    stretchPathEnd(s, 12, 1);
    expect(s.listener.speed[1].time).toBeCloseTo(4 * (MIN_WALK / 10), 6);
  });

  it('does nothing for a bad or unchanged end', () => {
    const s = walk();
    stretchPathEnd(s, 12, 12);
    stretchPathEnd(s, 2, 7);    // old end at the start: no walk to stretch
    expect(s.listener.speed.map((k) => k.time)).toEqual([0, 4]);
  });
});

describe('a Cmd-dragged range', () => {
  const scene = () => {
    const s = defaultScene();
    s.listener.paths = [{ name: 'p', closed: false, segments: [{ type: 'line', points: [[0, 1.7, 0], [10, 1.7, 0]] }] }] as never;
    s.listener.path_start_time = 2;
    s.listener.speed = [{ time: 0, speed: 1.4, easing: 'linear' }, { time: 4, speed: 0.7, easing: 'linear' }];
    s.listener.head.keys = [{ time: 3, yaw: 10, pitch: 0, roll: 0, easing: 'smooth' }, { time: 10, yaw: -10, pitch: 0, roll: 0, easing: 'smooth' }];
    return s;
  };

  it('takes the start, end and the keys inside it (speed keys by their timeline time)', () => {
    const r = selectRange(scene(), 12, 1.5, 5);
    expect(r).toEqual({ t0: 1.5, t1: 5, start: true, end: false, speed: [0], head: [0] });
    expect(selectRange(scene(), 12, 5.5, 13)).toEqual({ t0: 5.5, t1: 13, start: false, end: true, speed: [1], head: [1] });
  });

  it('moving it with the start shifts the walk and the head keys inside, not the speed keys twice', () => {
    const s = scene();
    const r = selectRange(s, 12, 1.5, 5);
    expect(shiftSelection(s, r, 3, 12)).toBe(3);
    expect(s.listener.path_start_time).toBe(5);
    expect(s.listener.speed.map((k) => k.time)).toEqual([0, 4]);
    expect(s.listener.head.keys.map((k) => k.time)).toEqual([6, 10]);
    expect(r.t0).toBe(4.5);
  });

  it('stops where something would go before 0', () => {
    const s = scene();
    const r = selectRange(s, 12, 1.5, 5);
    expect(shiftSelection(s, r, -3, 12)).toBe(-2);
    expect(s.listener.path_start_time).toBe(0);
    expect(s.listener.head.keys[0].time).toBe(1);
  });

  it('keeps pointing at the same keys when they pass others', () => {
    const s = scene();
    const r = selectRange(s, 12, 2.5, 3.5);   // the head key at 3 only
    expect(r).toEqual({ t0: 2.5, t1: 3.5, start: false, end: false, speed: [], head: [0] });
    shiftSelection(s, r, 9, 12);
    expect(s.listener.head.keys.map((k) => k.time)).toEqual([10, 12]);
    expect(r.head).toEqual([1]);
  });

  it('speed keys on their own move by themselves; with the end they stretch instead', () => {
    const s = scene();
    const r = selectRange(s, 12, 5.5, 6.5);   // the speed key at 2 + 4 = 6 only
    expect(r.speed).toEqual([1]);
    shiftSelection(s, r, 1, 12);
    expect(s.listener.speed[1].time).toBe(5);
    const s2 = scene();
    const r2 = selectRange(s2, 12, 5.5, 13);  // that key and the end
    shiftSelection(s2, r2, 5, 12);            // the walk ends at 17: 15 s instead of 10
    expect(s2.listener.speed.map((k) => [k.time, k.speed])).toEqual([[0, 1.4 / 1.5], [6, 0.7 / 1.5]].map(([t, v]) => [t, Math.round(v * 10000) / 10000]));
  });
});
