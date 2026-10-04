import { describe, expect, it } from 'vitest';
import {
  ambisonicOrder, ambisonicSurfacePoint, channelsForFile, completeScene, defaultAmbisonic, defaultLayer, isAmbisonic, layerExtras,
} from '../../src/model/scene';

describe('Ambisonic layers', () => {
  it('recognises first to third order by channel count', () => {
    expect(ambisonicOrder(4)).toBe(1);
    expect(ambisonicOrder(9)).toBe(2);
    expect(ambisonicOrder(16)).toBe(3);
    expect(ambisonicOrder(2)).toBe(0);
    expect(isAmbisonic(defaultLayer(0, { channels: 9 }))).toBe(true);
    expect(isAmbisonic(defaultLayer(0, { channels: 2 }))).toBe(false);
  });

  it('plays a 4 / 9 / 16-channel file as a sphere, anything else as mono or a pair', () => {
    expect(channelsForFile(1)).toBe(1);
    expect(channelsForFile(2)).toBe(2);
    expect(channelsForFile(4)).toBe(4);
    expect(channelsForFile(6)).toBe(2);
    expect(channelsForFile(16)).toBe(16);
    expect(layerExtras(4)).toEqual({ ambisonic: defaultAmbisonic() });
    expect(layerExtras(2)).toHaveProperty('stereo');
    expect(layerExtras(1)).toEqual({});
    const kept = { ...defaultAmbisonic(), radius: 7 };
    expect(layerExtras(9, { ambisonic: kept }).ambisonic).toBe(kept);
  });

  it('keeps the ambisonic block and file list through completeScene', () => {
    const s = completeScene({ layers: [{ channels: 4, audio_files: ['a', 'b', 'c', 'd'], ambisonic: { ...defaultAmbisonic(), radius: 5, yaw: 30 } } as any] });
    expect(s.layers[0].channels).toBe(4);
    expect(s.layers[0].audio_files).toEqual(['a', 'b', 'c', 'd']);
    expect(s.layers[0].ambisonic?.radius).toBe(5);
    expect(s.layers[0].ambisonic?.yaw).toBe(30);
  });

  it('puts the surface handle on the recording\'s right, turned by yaw', () => {
    const l = defaultLayer(0, { channels: 4, position: [1, 1.6, -2], ambisonic: { ...defaultAmbisonic(), radius: 3 } });
    expect(ambisonicSurfacePoint(l, [1, 0, 0])).toEqual([4, 1.6, -2]);
    expect(ambisonicSurfacePoint(l, [0, 0, -1])).toEqual([1, 1.6, -5]);
    // Yaw 90° turns the front towards -X (like the head), so the right end points forward (-Z).
    l.ambisonic!.yaw = 90;
    const p = ambisonicSurfacePoint(l, [1, 0, 0]);
    expect(p[0]).toBeCloseTo(1, 5);
    expect(p[2]).toBeCloseTo(-5, 5);
    // Pitch 90° raises the front straight up.
    l.ambisonic!.yaw = 0;
    l.ambisonic!.pitch = 90;
    const f = ambisonicSurfacePoint(l, [0, 0, -1]);
    expect(f[1]).toBeCloseTo(4.6, 5);
  });
});
