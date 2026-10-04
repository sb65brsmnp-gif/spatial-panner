#include "dsp/WavReader.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace sp::dsp {

namespace {

constexpr uint16_t kPcm = 1, kFloat = 3, kExtensible = 0xFFFE;

uint32_t le32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
uint16_t le16(const unsigned char* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

struct Parsed {
    WavInfo info;
    uint16_t format = 0;
    std::streamoff dataOffset = 0;
    uint32_t dataBytes = 0;
};

Parsed parseHeader(std::ifstream& in, const std::string& path) {
    unsigned char riff[12];
    if (!in.read(reinterpret_cast<char*>(riff), 12) || std::memcmp(riff, "RIFF", 4) != 0 || std::memcmp(riff + 8, "WAVE", 4) != 0)
        throw std::runtime_error("'" + path + "' is not a WAV file");
    Parsed p;
    bool haveFmt = false;
    while (true) {
        unsigned char ch[8];
        if (!in.read(reinterpret_cast<char*>(ch), 8)) break;
        const uint32_t size = le32(ch + 4);
        const std::streamoff next = in.tellg() + static_cast<std::streamoff>(size + (size & 1));
        if (std::memcmp(ch, "fmt ", 4) == 0) {
            std::vector<unsigned char> f(size);
            if (size < 16 || !in.read(reinterpret_cast<char*>(f.data()), size)) throw std::runtime_error("'" + path + "': bad fmt chunk");
            p.format = le16(f.data());
            p.info.channels = le16(f.data() + 2);
            p.info.sampleRate = static_cast<int>(le32(f.data() + 4));
            p.info.bitsPerSample = le16(f.data() + 14);
            if (p.format == kExtensible && size >= 40) p.format = le16(f.data() + 24);  // sub-format GUID's first word
            p.info.isFloat = p.format == kFloat;
            haveFmt = true;
        } else if (std::memcmp(ch, "data", 4) == 0) {
            p.dataOffset = in.tellg();
            p.dataBytes = size;
            break;
        }
        in.seekg(next);
        if (!in) break;
    }
    if (!haveFmt || p.dataOffset == 0) throw std::runtime_error("'" + path + "': no fmt or data chunk");
    if (p.format != kPcm && p.format != kFloat) throw std::runtime_error("'" + path + "': unsupported WAV format " + std::to_string(p.format) + " (PCM or float only)");
    if (p.info.channels <= 0 || p.info.sampleRate <= 0) throw std::runtime_error("'" + path + "': bad channel count or sample rate");
    const int bytes = p.info.bitsPerSample / 8;
    if ((p.format == kPcm && (bytes < 1 || bytes > 4)) || (p.format == kFloat && bytes != 4 && bytes != 8))
        throw std::runtime_error("'" + path + "': unsupported bit depth " + std::to_string(p.info.bitsPerSample));
    // A streaming writer may leave the size at 0 or 0xFFFFFFFF: use the file length.
    in.seekg(0, std::ios::end);
    const std::streamoff fileEnd = in.tellg();
    const std::streamoff avail = fileEnd - p.dataOffset;
    if (p.dataBytes == 0 || p.dataBytes == 0xFFFFFFFFu || static_cast<std::streamoff>(p.dataBytes) > avail) p.dataBytes = static_cast<uint32_t>(avail);
    p.info.frames = static_cast<long>(p.dataBytes / (static_cast<uint32_t>(bytes) * p.info.channels));
    return p;
}

}  // namespace

WavInfo readWavInfo(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("Cannot open WAV file '" + path + "'");
    return parseHeader(in, path).info;
}

WavData readWav(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("Cannot open WAV file '" + path + "'");
    const Parsed p = parseHeader(in, path);
    const int nch = p.info.channels, bytes = p.info.bitsPerSample / 8;
    const long frames = p.info.frames;
    std::vector<unsigned char> raw(static_cast<size_t>(frames) * nch * bytes);
    in.seekg(p.dataOffset);
    if (!in.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size()))) throw std::runtime_error("'" + path + "': truncated data");
    WavData d;
    d.info = p.info;
    d.channels.assign(nch, std::vector<float>(static_cast<size_t>(frames)));
    const unsigned char* s = raw.data();
    for (long i = 0; i < frames; ++i) {
        for (int c = 0; c < nch; ++c, s += bytes) {
            float v = 0;
            if (p.format == kFloat) {
                if (bytes == 4) { float f; std::memcpy(&f, s, 4); v = f; }
                else { double f; std::memcpy(&f, s, 8); v = static_cast<float>(f); }
            } else {
                switch (bytes) {
                    case 1: v = (static_cast<int>(s[0]) - 128) / 128.0f; break;
                    case 2: v = static_cast<int16_t>(le16(s)) / 32768.0f; break;
                    case 3: v = static_cast<int32_t>((static_cast<uint32_t>(s[0]) << 8) | (static_cast<uint32_t>(s[1]) << 16) | (static_cast<uint32_t>(s[2]) << 24)) / 2147483648.0f; break;
                    default: v = static_cast<int32_t>(le32(s)) / 2147483648.0f; break;
                }
            }
            d.channels[c][static_cast<size_t>(i)] = v;
        }
    }
    return d;
}

}  // namespace sp::dsp
