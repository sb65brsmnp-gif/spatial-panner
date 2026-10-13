import { describe, expect, it } from 'vitest';
import { completeScene, defaultLayer, defaultLink, defaultScene, isLinked, isSpatialized, leadsTo, linkedAt, removeLayerLinks } from '../../src/model/scene';

describe('links and the Spatialize switch', () => {
  it('a layer is spatialised unless the file says otherwise', () => {
    const l = defaultLayer(0);
    expect(isSpatialized(l)).toBe(true);
    l.spatialize = false;
    expect(isSpatialized(l)).toBe(false);
    expect(isLinked(l)).toBe(false);
    l.link = defaultLink('listener');
    expect(isLinked(l)).toBe(true);
  });

  it('link keys set the state from their time on', () => {
    const link = defaultLink(0);
    link.keys = [{ time: 2, linked: false }, { time: 5, linked: true }];
    expect(linkedAt(link, 1)).toBe(true);
    expect(linkedAt(link, 2)).toBe(false);
    expect(linkedAt(link, 4.9)).toBe(false);
    expect(linkedAt(link, 5)).toBe(true);
    link.linked_at_start = false;
    expect(linkedAt(link, 1)).toBe(false);
  });

  it('a layer cannot follow one of its own followers', () => {
    const s = defaultScene();
    s.layers = [defaultLayer(0), defaultLayer(1), defaultLayer(2)];
    s.layers[1].link = defaultLink(0);
    s.layers[2].link = defaultLink(1);
    expect(leadsTo(s.layers, 2, 0)).toBe(true);
    expect(leadsTo(s.layers, 1, 0)).toBe(true);
    expect(leadsTo(s.layers, 0, 2)).toBe(false);
    expect(leadsTo(s.layers, 2, 2)).toBe(false);
  });

  it('removing a layer drops links to it and renumbers the rest', () => {
    const s = defaultScene();
    s.layers = [defaultLayer(0), defaultLayer(1), defaultLayer(2), defaultLayer(3)];
    s.layers[0].link = defaultLink(1);
    s.layers[2].link = defaultLink(3);
    s.layers[3].link = defaultLink('listener');
    s.layers.splice(1, 1);
    removeLayerLinks(s.layers, 1);
    expect(s.layers[0].link).toBeUndefined();
    expect(s.layers[1].link!.to).toBe(2);
    expect(s.layers[2].link!.to).toBe('listener');
  });

  it('older files load with the fields absent', () => {
    const s = completeScene({ layers: [defaultLayer(0)] });
    expect(s.layers[0].spatialize).toBeUndefined();
    expect(s.layers[0].link).toBeUndefined();
  });
});
