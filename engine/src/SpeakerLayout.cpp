#include "sp/SpeakerLayout.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace sp {

using nlohmann::json;

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

int SpeakerLayout::numSubwoofers() const {
    int n = 0;
    for (const auto& s : speakers) n += s.lfe ? 1 : 0;
    return n;
}

int SpeakerLayout::channelOf(int i) const {
    if (i < 0 || i >= numChannels()) return -1;
    const int c = speakers[static_cast<size_t>(i)].channel;
    return c >= 0 ? c : i;
}

int SpeakerLayout::numOutputChannelsNeeded() const {
    int n = 0;
    for (int i = 0; i < numChannels(); ++i) n = std::max(n, channelOf(i) + 1);
    return n;
}

bool SpeakerLayout::needsAlignment() const {
    for (const auto& s : speakers)
        if (s.distance > 0 || s.trimDb != 0 || s.delayMs != 0) return true;
    return false;
}

const char* BassManagement::filterName(Filter f) {
    switch (f) {
        case Filter::Butterworth12: return "butterworth12";
        case Filter::Butterworth24: return "butterworth24";
        case Filter::LinkwitzRiley24: return "lr24";
        case Filter::LinkwitzRiley48: return "lr48";
    }
    return "lr24";
}

bool BassManagement::filterFromName(const std::string& name, Filter& out) {
    for (Filter f : {Filter::Butterworth12, Filter::Butterworth24, Filter::LinkwitzRiley24, Filter::LinkwitzRiley48})
        if (name == filterName(f)) { out = f; return true; }
    return false;
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

float num(const json& j, const char* key, float def) {
    if (!j.contains(key) || !j[key].is_number()) return def;
    const float v = j[key].get<float>();
    return std::isfinite(v) ? v : def;
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

SpeakerLayout SpeakerLayout::fromJson(const std::string& text) {
    const json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) throw std::runtime_error("Speaker layout: not a JSON object");
    SpeakerLayout l;
    l.name = j.value("name", std::string("custom"));
    if (!j.contains("speakers") || !j["speakers"].is_array()) throw std::runtime_error("Speaker layout: no speakers");
    for (const auto& s : j["speakers"]) {
        if (!s.is_object()) throw std::runtime_error("Speaker layout: a speaker is not an object");
        Speaker k;
        k.name = s.value("name", std::string());
        k.azimuthDeg = num(s, "azimuth", 0);
        k.elevationDeg = std::clamp(num(s, "elevation", 0), -90.0f, 90.0f);
        k.distance = std::max(0.0f, num(s, "distance", 0));
        k.lfe = s.value("lfe", false);
        k.trimDb = std::clamp(num(s, "trim_db", 0), -60.0f, 20.0f);
        k.delayMs = std::clamp(num(s, "delay_ms", 0), 0.0f, 500.0f);
        const int ch = s.contains("channel") && s["channel"].is_number_integer() ? s["channel"].get<int>() : 0;
        k.channel = ch >= 1 ? ch - 1 : -1;
        l.speakers.push_back(std::move(k));
    }
    if (l.speakers.empty()) throw std::runtime_error("Speaker layout: no speakers");
    if (l.numSpatialChannels() == 0) throw std::runtime_error("Speaker layout: every speaker is a subwoofer");
    if (j.contains("bass") && j["bass"].is_object()) {
        const auto& b = j["bass"];
        l.bass.enabled = b.value("enabled", false);
        l.bass.cutoffHz = std::clamp(num(b, "cutoff_hz", 80), 20.0f, 500.0f);
        l.bass.subGainDb = std::clamp(num(b, "sub_gain_db", 0), -30.0f, 20.0f);
        BassManagement::filterFromName(b.value("filter", std::string()), l.bass.filter);
    }
    return l;
}

std::string SpeakerLayout::toJson() const {
    json j;
    j["name"] = name;
    j["speakers"] = json::array();
    for (int i = 0; i < numChannels(); ++i) {
        const Speaker& s = speakers[static_cast<size_t>(i)];
        j["speakers"].push_back({{"name", s.name}, {"azimuth", s.azimuthDeg}, {"elevation", s.elevationDeg},
                                 {"distance", s.distance}, {"lfe", s.lfe}, {"trim_db", s.trimDb},
                                 {"delay_ms", s.delayMs}, {"channel", channelOf(i) + 1}});
    }
    j["bass"] = {{"enabled", bass.enabled}, {"cutoff_hz", bass.cutoffHz}, {"filter", BassManagement::filterName(bass.filter)},
                 {"sub_gain_db", bass.subGainDb}};
    return j.dump();
}

}  // namespace sp
