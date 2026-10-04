import { describe, expect, it } from 'vitest';
import { defaultStereo, stereoEnds, stereoFromEnds, stereoOffset, type LayerDoc, defaultLayer } from '../../src/model/scene';

describe('stereo layer geometry', () => {
  it('puts the ends either side of the centre along +X at rotation 0', () => {
    const l: LayerDoc = defaultLayer(0, { channels: 2, position: [1, 1.6, -2], stereo: { width: 4, rotation: 0, elevation: 0, mono: false } });
    const [left, right] = stereoEnds(l);
    expect(left).toEqual([-1, 1.6, -2]);
    expect(right).toEqual([3, 1.6, -2]);
  });

  it('turns the right end towards -Z with positive rotation', () => {
    const d = stereoOffset({ width: 4, rotation: 90, elevation: 0, mono: false });
    expect(d[0]).toBeCloseTo(0);
    expect(d[2]).toBeCloseTo(-2);
  });

  it('recovers centre, width, rotation and elevation from the ends', () => {
    const st = { width: 3, rotation: 30, elevation: 20, mono: false };
    const l: LayerDoc = defaultLayer(0, { channels: 2, position: [0.5, 1.2, -3], stereo: st });
    const [left, right] = stereoEnds(l);
    const r = stereoFromEnds(left, right, defaultStereo());
    expect(r.position).toEqual([0.5, 1.2, -3]);
    expect(r.stereo.width).toBeCloseTo(3, 3);
    expect(r.stereo.rotation).toBeCloseTo(30, 1);
    expect(r.stereo.elevation).toBeCloseTo(20, 1);
  });

  it('keeps the previous angles when the ends coincide', () => {
    const r = stereoFromEnds([1, 1, 1], [1, 1, 1], { width: 2, rotation: 45, elevation: 10, mono: true });
    expect(r.stereo.width).toBe(0);
    expect(r.stereo.rotation).toBe(45);
    expect(r.stereo.mono).toBe(true);
  });
});
