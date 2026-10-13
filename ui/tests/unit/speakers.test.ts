import { describe, expect, it } from 'vitest';
import { presetSpeakers, speakerLocal, sphericalOf, outputSpeakers, speakerChannelsNeeded, SPEAKER_PRESETS, LAYOUTS } from '../../src/model/scene';
import { MeterBallistics } from '../../src/model/meters';
import { DEFAULT_SETTINGS, loadSettings } from '../../src/model/settings';

describe('speaker presets', () => {
  it('mirror the engine: every layout, in its channel order, with the LFE fourth', () => {
    for (const name of LAYOUTS) expect(SPEAKER_PRESETS[name]).toBeDefined();
    const s = presetSpeakers('7.1.4');
    expect(s.map((x) => x.name)).toEqual(['L', 'R', 'C', 'LFE', 'Lss', 'Rss', 'Lrs', 'Rrs', 'Ltf', 'Rtf', 'Ltr', 'Rtr']);
    expect(s[3].lfe).toBe(true);
    expect(s.map((x) => x.channel)).toEqual([1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12]);
    expect(s[0].azimuth).toBe(30);   // L is to the left: positive azimuth
    expect(s[8].elevation).toBe(45);
    expect(s.every((x) => x.distance === 2)).toBe(true);
    expect(speakerChannelsNeeded(s)).toBe(12);
  });

  it('are what a speakers output plays through when none are placed; none in binaural', () => {
    expect(outputSpeakers({ mode: 'speakers', layout: '5.1' }).length).toBe(6);
    expect(outputSpeakers({ mode: 'binaural', layout: '5.1' }).length).toBe(0);
    const placed = [{ name: 'A', azimuth: 0, elevation: 0, distance: 1, channel: 3 }];
    expect(outputSpeakers({ mode: 'speakers', layout: '5.1', speakers: placed })).toBe(placed);
  });
});

describe('speaker placement', () => {
  it('puts azimuth 0 ahead (-Z), positive azimuth to the left (-X), elevation up', () => {
    const ahead = speakerLocal({ name: '', azimuth: 0, elevation: 0, distance: 2, channel: 1 });
    expect(ahead[0]).toBeCloseTo(0);
    expect(ahead[2]).toBeCloseTo(-2);
    const left = speakerLocal({ name: '', azimuth: 90, elevation: 0, distance: 2, channel: 1 });
    expect(left[0]).toBeCloseTo(-2);
    const up = speakerLocal({ name: '', azimuth: 0, elevation: 90, distance: 2, channel: 1 });
    expect(up[1]).toBeCloseTo(2);
  });

  it('round-trips through spherical coordinates', () => {
    for (const [az, el, d] of [[30, 0, 2], [-110, 10, 3.5], [135, 45, 1.2], [-180, -20, 2], [0, 55, 4]]) {
      const v = speakerLocal({ name: '', azimuth: az, elevation: el, distance: d, channel: 1 });
      const s = sphericalOf(v);
      expect(s.distance).toBeCloseTo(d, 5);
      expect(s.elevation).toBeCloseTo(el, 4);
      // -180 and 180 are the same direction.
      expect(Math.abs(((s.azimuth - az + 540) % 360) - 180)).toBeCloseTo(0, 4);
    }
  });

  it('draws an equidistant (distance 0) speaker at the standard radius', () => {
    const v = speakerLocal({ name: '', azimuth: 0, elevation: 0, distance: 0, channel: 1 });
    expect(Math.hypot(...v)).toBeCloseTo(2);
  });
});

describe('meter ballistics', () => {
  it('holds a peak through empty reads, then falls at the set rate', () => {
    const m = new MeterBallistics(150, 30);
    expect(m.update([-6], 0)).toEqual([-6]);
    // The host did not run between reads: the raw value is empty.
    expect(m.update([-120], 33)[0]).toBe(-6);
    expect(m.update([-120], 100)[0]).toBe(-6);
    // After the hold, it falls: 30 dB/s.
    const after = m.update([-120], 250)[0];
    expect(after).toBeLessThan(-6);
    expect(after).toBeCloseTo(-6 - 30 * 0.1, 1);
    // A louder peak takes over at once.
    expect(m.update([-3], 300)[0]).toBe(-3);
  });

  it('never shows less than the raw value', () => {
    const m = new MeterBallistics(150, 30);
    m.update([-6], 0);
    expect(m.update([-4], 1000)[0]).toBe(-4);
    expect(m.update([-20], 1300)[0]).toBeGreaterThanOrEqual(-20);
  });
});

describe('settings', () => {
  it('load the defaults when the stored value is missing or bad', () => {
    expect(loadSettings(null)).toEqual(DEFAULT_SETTINGS);
    expect(loadSettings({ getItem: () => 'not json' })).toEqual(DEFAULT_SETTINGS);
    expect(loadSettings({ getItem: () => JSON.stringify({ labelSize: 'big', labelMode: 'sometimes', dotSize: 1.5 }) }))
      .toEqual({ ...DEFAULT_SETTINGS, dotSize: 1.5 });
  });
});
