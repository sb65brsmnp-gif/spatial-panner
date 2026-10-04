import { describe, expect, it } from 'vitest';
import { Store } from '../../src/model/store';
import { defaultLayer, defaultScene, mergeEditorKeys } from '../../src/model/scene';

describe('plugin document handling', () => {
  it('keeps the track binding through the canonical form', () => {
    const raw = { ...defaultScene(), layers: [defaultLayer(0, { name: 'Vox', host_id: 'abc' })] };
    const canonical = { ...raw, layers: raw.layers.map(({ host_id: _h, color: _c, ...l }) => l) };
    const s = mergeEditorKeys(canonical as never, raw);
    expect(s.layers[0].host_id).toBe('abc');
  });

  it('replaces the document without losing undo, file or playhead', () => {
    const st = new Store();
    st.load(defaultScene(), null);
    st.update((s) => { s.name = 'edited'; });
    st.setTime(12);
    const rev = st.revision;
    st.replace({ ...st.scene, layers: [defaultLayer(0, { name: 'Drums', host_id: 't1' })] });
    expect(st.scene.layers[0].host_id).toBe('t1');
    expect(st.revision).toBe(rev + 1);
    expect(st.time).toBe(12);
    expect(st.canUndo).toBe(true);
    st.undo();
    expect(st.scene.layers.length).toBe(0);
    expect(st.scene.name).toBe('edited');
  });

  it('ignores a replacement that changes nothing', () => {
    const st = new Store();
    st.load(defaultScene(), null);
    const rev = st.revision;
    st.replace(JSON.parse(JSON.stringify(st.scene)));
    expect(st.revision).toBe(rev);
  });

  it('follows the host pose while stopped', () => {
    const st = new Store();
    st.hostDriven = true;
    st.livePose = [1, 2, 3, 40, 0, 0, 0, 0];
    expect(st.poseAt(5)?.[3]).toBe(40);
  });
});
