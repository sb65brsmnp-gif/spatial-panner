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
    bool lfe = false;        // a subwoofer: receives nothing from the spatialiser
                             // (only the bass-management crossover feeds it)
    float trimDb = 0;        // level trim on this speaker's feed
    float delayMs = 0;       // extra delay on this speaker's feed
    // Which output carries this speaker (0-based: the interface's output or
    // the host bus channel); -1 = its index in the layout.
    int channel = -1;
};

// Bass management: a crossover takes the lows out of every spatial speaker
// and sends their sum to the layout's subwoofer(s) (the `lfe` speakers).
struct BassManagement {
    enum class Filter { Butterworth12, Butterworth24, LinkwitzRiley24, LinkwitzRiley48 };
    bool enabled = false;
    float cutoffHz = 80;
    Filter filter = Filter::LinkwitzRiley24;
    float subGainDb = 0;     // on the summed lows
    // Filter names as the scene JSON spells them ("butterworth12", "lr24"...).
    static const char* filterName(Filter f);
    static bool filterFromName(const std::string& name, Filter& out);
};

struct SpeakerLayout {
    std::string name;
    std::vector<Speaker> speakers;
    BassManagement bass;

    int numChannels() const { return static_cast<int>(speakers.size()); }
    bool isPlanar() const;       // all non-LFE speakers at elevation 0
    int numSpatialChannels() const;
    int numSubwoofers() const;
    // Output channel of speaker i (its own `channel`, else i).
    int channelOf(int i) const;
    // Highest output channel used, plus one.
    int numOutputChannelsNeeded() const;
    // Any trim, delay or distance set: the per-speaker alignment runs.
    bool needsAlignment() const;

    // Presets: "stereo", "quad", "5.1", "7.1", "5.1.4", "7.1.4", "9.1.6".
    // Returns an empty layout for unknown names.
    static SpeakerLayout preset(const std::string& name);
    static std::vector<std::string> presetNames();

    // JSON form, as the editor stores it under editor.output:
    //   {"name": "...", "speakers": [{"name","azimuth","elevation","distance",
    //    "lfe","trim_db","delay_ms","channel" (1-based)}...],
    //    "bass": {"enabled","cutoff_hz","filter","sub_gain_db"}}
    // fromJson throws std::runtime_error on malformed input.
    static SpeakerLayout fromJson(const std::string& json);
    std::string toJson() const;
};

}  // namespace sp
