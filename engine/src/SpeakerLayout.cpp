#include "sp/SpeakerLayout.h"

#include <cmath>

namespace sp {

bool SpeakerLayout::isPlanar() const {
    for (const auto& s : speakers)
        if (!s.lfe && std::fabs(s.elevationDeg) > 0.5f) return false;
    return true;
}

int SpeakerLayout::numSpatialChannels() const {
    int n = 0;
    for (const auto& s : speakers) n += s.lfe ? 0 : 1;
    return n;
}

namespace {

Speaker sp(const char* name, float az, float el = 0, bool lfe = false) {
    Speaker s;
    s.name = name;
    s.azimuthDeg = az;
    s.elevationDeg = el;
    s.lfe = lfe;
    return s;
}

// Channel orders follow the common film/SMPTE order used by Logic and most
// DAWs for interleaved files: L R C LFE Ls Rs (Lrs Rrs) then heights.
// Angles follow ITU-R BS.2051 (BS.775 for 5.1). Heights at 45 degrees
// elevation, which is what Logic's 7.1.4 Atmos bed assumes.
const std::vector<std::pair<std::string, std::vector<Speaker>>>& presets() {
    static const std::vector<std::pair<std::string, std::vector<Speaker>>> p = {
        {"stereo", {sp("L", 30), sp("R", -30)}},
        {"quad", {sp("L", 45), sp("R", -45), sp("Ls", 135), sp("Rs", -135)}},
        {"5.1", {sp("L", 30), sp("R", -30), sp("C", 0), sp("LFE", 0, 0, true), sp("Ls", 110), sp("Rs", -110)}},
        {"7.1", {sp("L", 30), sp("R", -30), sp("C", 0), sp("LFE", 0, 0, true), sp("Lss", 90), sp("Rss", -90),
                 sp("Lrs", 135), sp("Rrs", -135)}},
        {"5.1.4", {sp("L", 30), sp("R", -30), sp("C", 0), sp("LFE", 0, 0, true), sp("Ls", 110), sp("Rs", -110),
                   sp("Ltf", 45, 45), sp("Rtf", -45, 45), sp("Ltr", 135, 45), sp("Rtr", -135, 45)}},
        {"7.1.4", {sp("L", 30), sp("R", -30), sp("C", 0), sp("LFE", 0, 0, true), sp("Lss", 90), sp("Rss", -90),
                   sp("Lrs", 135), sp("Rrs", -135), sp("Ltf", 45, 45), sp("Rtf", -45, 45), sp("Ltr", 135, 45),
                   sp("Rtr", -135, 45)}},
        {"9.1.6", {sp("L", 30), sp("R", -30), sp("C", 0), sp("LFE", 0, 0, true), sp("Lss", 90), sp("Rss", -90),
                   sp("Lrs", 135), sp("Rrs", -135), sp("Lw", 60), sp("Rw", -60), sp("Ltf", 45, 45),
                   sp("Rtf", -45, 45), sp("Ltm", 90, 55), sp("Rtm", -90, 55), sp("Ltr", 135, 45),
                   sp("Rtr", -135, 45)}},
    };
    return p;
}

}  // namespace

SpeakerLayout SpeakerLayout::preset(const std::string& name) {
    SpeakerLayout l;
    for (const auto& kv : presets()) {
        if (kv.first == name) {
            l.name = name;
            l.speakers = kv.second;
            return l;
        }
    }
    return l;
}

std::vector<std::string> SpeakerLayout::presetNames() {
    std::vector<std::string> out;
    for (const auto& kv : presets()) out.push_back(kv.first);
    return out;
}

}  // namespace sp
