import { describe, expect, it } from 'vitest';
import { FINE_STEP, STEP, pathEndTime, pathStartTime, speedKeyAbsolute, speedKeyTime, steppedTime, transportKeyAction } from '../../src/panels/timeline';
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
