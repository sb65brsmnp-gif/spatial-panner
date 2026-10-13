#pragma once
// What the loudspeaker panner would do at a point: the editor's heatmap and
// hover readout, and "capture" for a pinned mix.
#include <vector>

#include "sp/Scene.h"
#include "sp/SpeakerLayout.h"

namespace sp {

struct PanProbeRequest {
    SpeakerLayout layout;
    // Head-local positions (metres; Y up, -Z ahead, +X right), the points
    // to probe and the pins to blend with (same frame).
    std::vector<Vec3> points;
    std::vector<Pin> pins;
    // The layer's distance model.
    float rolloff = 1.0f;
    float minDistance = 0.25f;
    float referenceDistance = 1.0f;
};

// Per point, per layout channel: the linear gain a layer's direct sound at
// that point would reach the speaker feed with. Panner gains (blended with
// the pins) times the distance law times the speaker's alignment gain
// (d / max distance, trim). Subwoofers get 0: bass management is not
// modelled here.
std::vector<std::vector<float>> panProbe(const PanProbeRequest& req);

}  // namespace sp
