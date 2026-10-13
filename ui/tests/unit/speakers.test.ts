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

import { pairSpeakers, unpairSpeaker, pairedWith, moveSpeakerBy, defaultScene, defaultLayer, type SpeakerDoc } from '../../src/model/scene';
import { Store } from '../../src/model/store';

describe('stereo pairs', () => {
  const two = (): SpeakerDoc[] => [
    { name: 'A', azimuth: 10, elevation: 0, distance: 2, channel: 1 },
    { name: 'B', azimuth: -70, elevation: 15, distance: 3, channel: 2 },
    { name: 'Sub', azimuth: 0, elevation: -20, distance: 2, channel: 3, lfe: true },
  ];

  it('set the pair 30° either side of their centre, the left one on the left, at the first one\'s height and distance', () => {
    const list = two();
    pairSpeakers(list, 0, 1);
    expect(pairedWith(list, 0)).toBe(1);
    expect(pairedWith(list, 1)).toBe(0);
    expect(pairedWith(list, 2)).toBe(-1);
    // Centre -30: A (further left) goes to 0, B to -60.
    expect(list[0].azimuth).toBe(0);
    expect(list[1].azimuth).toBe(-60);
    expect(list[1].elevation).toBe(0);
    expect(list[1].distance).toBe(2);
  });

  it('move, turn and travel together; unpairing frees both', () => {
    const list = two();
    pairSpeakers(list, 0, 1);
    moveSpeakerBy(list, 1, 90, 10, 1);
    expect(list[0].azimuth).toBe(90);
    expect(list[1].azimuth).toBe(30);
    expect(list[0].elevation).toBe(10);
    expect(list[0].distance).toBe(3);
    expect(list[1].distance).toBe(3);
    // Alone, a speaker moves alone; wrapping and clamps apply.
    moveSpeakerBy(list, 2, -200, -100, -5);
    expect(list[2].azimuth).toBe(160);
    expect(list[2].elevation).toBe(-90);
    expect(list[2].distance).toBe(0.2);
    unpairSpeaker(list, 0);
    expect(list[0].pair).toBeUndefined();
    expect(list[1].pair).toBeUndefined();
    moveSpeakerBy(list, 0, 10, 0, 0);
    expect(list[1].azimuth).toBe(30);
  });

  it('a new pair dissolves the old ones', () => {
    const list = [...two(), { name: 'D', azimuth: 120, elevation: 0, distance: 2, channel: 4 }];
    pairSpeakers(list, 0, 1);
    pairSpeakers(list, 1, 3);
    expect(pairedWith(list, 0)).toBe(-1);
    expect(pairedWith(list, 1)).toBe(3);
  });
});

describe('selecting several things', () => {
  it('Cmd+click toggles membership and one thing stays a plain selection', () => {
    const st = new Store();
    const sc = defaultScene();
    sc.layers = [defaultLayer(0), defaultLayer(1), defaultLayer(2)];
    sc.editor = { ...sc.editor, output: { mode: 'speakers', layout: 'stereo', speakers: [
      { name: 'L', azimuth: 30, elevation: 0, distance: 2, channel: 1 }, { name: 'R', azimuth: -30, elevation: 0, distance: 2, channel: 2 }] } };
    st.load(sc, null);
    st.select({ kind: 'layer', index: 0 });
    st.toggleSelected('layer', 2);
    expect(st.selection).toEqual({ kind: 'multi', layers: [0, 2], speakers: [] });
    expect(st.isLayerSelected(2)).toBe(true);
    expect(st.isLayerSelected(1)).toBe(false);
    st.toggleSelected('speaker', 1);
    expect(st.selectedSpeakers()).toEqual([1]);
    st.toggleSelected('layer', 0);
    st.toggleSelected('layer', 2);
    expect(st.selection).toEqual({ kind: 'speaker', index: 1 });
    st.selectMany([2, 2, 0], []);
    expect(st.selection).toEqual({ kind: 'multi', layers: [0, 2], speakers: [] });
    // A selection past the end of the list is dropped.
    st.update((s) => { s.layers.splice(1, 1); });
    expect(st.selectedLayers()).toEqual([0]);
    st.selectMany([], []);
    expect(st.selection).toEqual({ kind: 'none' });
  });
});
