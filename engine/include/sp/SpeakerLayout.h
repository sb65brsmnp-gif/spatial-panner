// Loudspeaker layouts for the multichannel output mode.
#pragma once

#include <string>
#include <vector>

namespace sp {

struct Speaker {
    std::string name;
    float azimuthDeg = 0;    // counter-clockwise from front, positive = left
    float elevationDeg = 0;
    float distance = 0;      // metres; 0 = equidistant (no compensation)
    bool lfe = false;        // receives nothing from the spatialiser
};

struct SpeakerLayout {
    std::string name;
    std::vector<Speaker> speakers;

    int numChannels() const { return static_cast<int>(speakers.size()); }
    bool isPlanar() const;       // all non-LFE speakers at elevation 0
    int numSpatialChannels() const;

    // Presets: "stereo", "quad", "5.1", "7.1", "5.1.4", "7.1.4", "9.1.6".
    // Returns an empty layout for unknown names.
    static SpeakerLayout preset(const std::string& name);
    static std::vector<std::string> presetNames();
};

}  // namespace sp
