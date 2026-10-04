#include "SharedSession.h"

#include <juce_core/juce_core.h>

#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include <cstddef>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace spplug {

namespace {
constexpr uint64_t kMagic = 0x53504c4956453031ull;  // "SPLIVE01"
constexpr uint32_t kLayoutVersion = 5;

static_assert(std::atomic<uint64_t>::is_always_lock_free, "needs lock-free 64-bit atomics");
static_assert(std::atomic<uint32_t>::is_always_lock_free, "needs lock-free 32-bit atomics");
static_assert(std::atomic<float>::is_always_lock_free, "needs lock-free float atomics");

uint32_t floatBits(float v) { uint32_t b; std::memcpy(&b, &v, 4); return b; }
float bitsFloat(uint32_t b) { float v; std::memcpy(&v, &b, 4); return v; }

// History bins hold ~bits so that zero (a fresh, sparse file) means "never
// played": no real float encodes to all ones.
uint32_t encodeBin(float v) { return std::isnan(v) ? 0u : ~floatBits(v); }
float decodeBin(uint32_t e) { return e == 0 ? std::numeric_limits<float>::quiet_NaN() : bitsFloat(~e); }

void storeString(std::atomic<uint64_t>* words, int n, const std::string& s) {
    for (int w = 0; w < n; ++w) {
        uint64_t v = 0;
        for (int b = 0; b < 8; ++b) {
            const size_t i = static_cast<size_t>(w * 8 + b);
            const unsigned char c = i < s.size() && i < static_cast<size_t>(n * 8 - 1) ? static_cast<unsigned char>(s[i]) : 0;
            v |= static_cast<uint64_t>(c) << (8 * b);
        }
        words[w].store(v, std::memory_order_relaxed);
    }
}

std::string loadString(const std::atomic<uint64_t>* words, int n) {
    std::string s;
    for (int w = 0; w < n; ++w) {
        const uint64_t v = words[w].load(std::memory_order_relaxed);
        for (int b = 0; b < 8; ++b) {
            const char c = static_cast<char>((v >> (8 * b)) & 0xff);
            if (c == 0) return s;
            s.push_back(c);
        }
    }
    return s;
}
}  // namespace

struct SharedSession::Region {
    std::atomic<uint64_t> magic;
    std::atomic<uint32_t> layoutVersion;
    std::atomic<uint32_t> ownerGeneration;
    std::atomic<uint64_t> owner;
    std::atomic<uint64_t> ownerHeartbeat;
    std::atomic<uint64_t> sceneRevision;
    std::atomic<uint32_t> sceneSeq;
    std::atomic<uint32_t> sceneLength;
    std::atomic<uint32_t> historyEpoch;
    std::atomic<uint32_t> pad0;
    std::atomic<float> live[kNumListenerParams];

    struct Slot {
        std::atomic<uint64_t> owner;      // token of the instance holding it; 0 = free
        std::atomic<uint64_t> heartbeat;
        std::atomic<uint32_t> seq;        // seqlock over the strings
        std::atomic<uint32_t> meterBits;  // peak since last read
        std::atomic<uint64_t> id[kIdWords];
        std::atomic<uint64_t> name[kNameWords];
        std::atomic<uint64_t> cloneOf[kIdWords];
        std::atomic<uint32_t> channels;   // the track's input channels (1, 2, or 4 for a quad track; up to 16)
        std::atomic<uint32_t> pad;
    } slots[kMaxSlots];

    std::atomic<uint32_t> chunkVersion[kNumChunks];
    std::atomic<uint32_t> bins[kNumListenerParams][kMaxBins];
    std::atomic<uint64_t> scene[kMaxSceneBytes / 8];
};

// ------------------------------------------------------------------ attach

namespace {
std::mutex& registryMutex() { static std::mutex m; return m; }
std::map<std::string, std::weak_ptr<SharedSession>>& registry() {
    static std::map<std::string, std::weak_ptr<SharedSession>> r;
    return r;
}

std::string defaultPath() {
    const auto dir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
#if JUCE_MAC
                         .getChildFile("Application Support")
#endif
                         .getChildFile("Spatial Panner");
    dir.createDirectory();
    return dir.getChildFile("live-session.bin").getFullPathName().toStdString();
}

void initialise(SharedSession::Region* r) {
    // The file is zero-filled; set what must not start at zero.
    r->live[kSpeed].store(1.0f);
    r->live[kActivePath].store(-1.0f);
    r->layoutVersion.store(kLayoutVersion);
    r->magic.store(kMagic, std::memory_order_release);
}
}  // namespace

std::shared_ptr<SharedSession> SharedSession::attach(const std::string& requested) {
    const std::string path = requested.empty() ? defaultPath() : requested;
    std::lock_guard<std::mutex> lock(registryMutex());
    if (auto existing = registry()[path].lock()) return existing;

    std::shared_ptr<SharedSession> s(new SharedSession());
    s->size_ = sizeof(Region);
    s->path_ = path;

    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd >= 0) {
        if (::flock(fd, LOCK_EX) == 0) {
            // A file of another size or layout (or none yet) is reset to zeros,
            // which ftruncate does without writing (the file stays sparse).
            struct stat st {};
            bool ok = ::fstat(fd, &st) == 0;
            uint64_t magic = 0;
            uint32_t version = 0;
            const bool sameSize = ok && static_cast<size_t>(st.st_size) == s->size_;
            if (sameSize) {
                ok = ::pread(fd, &magic, sizeof magic, 0) == sizeof magic &&
                     ::pread(fd, &version, sizeof version, offsetof(Region, layoutVersion)) == sizeof version;
            }
            const bool fresh = !sameSize || magic != kMagic || version != kLayoutVersion;
            if (ok && fresh) ok = ::ftruncate(fd, 0) == 0 && ::ftruncate(fd, static_cast<off_t>(s->size_)) == 0;
            void* p = ok ? ::mmap(nullptr, s->size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) : MAP_FAILED;
            if (p != MAP_FAILED) {
                s->r_ = static_cast<Region*>(p);
                s->mapped_ = true;
                if (fresh) initialise(s->r_);
            }
            ::flock(fd, LOCK_UN);
        }
        ::close(fd);
    }
    if (!s->r_) {
        s->local_.reset(new uint8_t[s->size_]());
        s->r_ = reinterpret_cast<Region*>(s->local_.get());
        initialise(s->r_);
    }
    registry()[path] = s;
    return s;
}

void SharedSession::detachAllForTesting() {
    std::lock_guard<std::mutex> lock(registryMutex());
    registry().clear();
}

SharedSession::~SharedSession() {
    if (mapped_ && r_) ::munmap(r_, size_);
}

uint64_t SharedSession::nowMs() { return static_cast<uint64_t>(juce::Time::currentTimeMillis()); }

std::atomic<float>& SharedSession::live_(int p) const { return r_->live[p]; }

// --------------------------------------------------------------- ownership

bool SharedSession::claimScene(uint64_t token, bool force, uint64_t staleMs) {
    if (token == 0) return false;
    uint64_t cur = r_->owner.load();
    for (;;) {
        if (cur == token) return true;
        const bool stale = nowMs() - r_->ownerHeartbeat.load() > staleMs;
        if (cur != 0 && !stale && !force) return false;
        if (r_->owner.compare_exchange_weak(cur, token)) break;
    }
    r_->ownerHeartbeat.store(nowMs());
    r_->ownerGeneration.fetch_add(1);
    return true;
}

void SharedSession::releaseScene(uint64_t token) {
    uint64_t expected = token;
    if (r_->owner.compare_exchange_strong(expected, 0)) r_->ownerGeneration.fetch_add(1);
}

uint64_t SharedSession::sceneOwner() const { return r_->owner.load(std::memory_order_acquire); }

bool SharedSession::sceneAlive(uint64_t staleMs) const {
    return sceneOwner() != 0 && nowMs() - r_->ownerHeartbeat.load() <= staleMs;
}

void SharedSession::heartbeatScene(uint64_t token) {
    if (r_->owner.load() == token) r_->ownerHeartbeat.store(nowMs());
}

uint32_t SharedSession::ownerGeneration() const { return r_->ownerGeneration.load(std::memory_order_acquire); }

// ------------------------------------------------------------------- scene

uint64_t SharedSession::publishScene(uint64_t token, const std::string& json) {
    if (r_->owner.load() != token || json.size() > kMaxSceneBytes - 1) return 0;
    const uint32_t s = r_->sceneSeq.load(std::memory_order_relaxed);
    r_->sceneSeq.store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    const size_t words = (json.size() + 7) / 8;
    for (size_t w = 0; w < words; ++w) {
        uint64_t v = 0;
        const size_t n = std::min<size_t>(8, json.size() - w * 8);
        std::memcpy(&v, json.data() + w * 8, n);
        r_->scene[w].store(v, std::memory_order_relaxed);
    }
    r_->sceneLength.store(static_cast<uint32_t>(json.size()), std::memory_order_relaxed);
    const uint64_t rev = r_->sceneRevision.load(std::memory_order_relaxed) + 1;
    r_->sceneRevision.store(rev, std::memory_order_relaxed);
    r_->sceneSeq.store(s + 2, std::memory_order_release);
    return rev;
}

uint64_t SharedSession::sceneRevision() const { return r_->sceneRevision.load(std::memory_order_acquire); }

bool SharedSession::readScene(uint64_t& revision, std::string& json) const {
    for (int attempt = 0; attempt < 100; ++attempt) {
        const uint32_t s1 = r_->sceneSeq.load(std::memory_order_acquire);
        if (s1 & 1u) { std::this_thread::yield(); continue; }
        const uint64_t rev = r_->sceneRevision.load(std::memory_order_relaxed);
        if (rev == revision) return false;
        const uint32_t len = std::min<uint32_t>(r_->sceneLength.load(std::memory_order_relaxed), kMaxSceneBytes - 1);
        std::string text(len, '\0');
        for (size_t w = 0; w * 8 < len; ++w) {
            const uint64_t v = r_->scene[w].load(std::memory_order_relaxed);
            std::memcpy(&text[w * 8], &v, std::min<size_t>(8, len - w * 8));
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        if (r_->sceneSeq.load(std::memory_order_relaxed) != s1) continue;
        revision = rev;
        json = std::move(text);
        return true;
    }
    return false;
}

// ------------------------------------------------------------------- slots

int SharedSession::claimSlot(uint64_t token, const std::string& id, const std::string& cloneOf) {
    if (idInUse(id, token)) return -1;
    const uint64_t now = nowMs();
    for (int i = 0; i < kMaxSlots; ++i) {
        auto& sl = r_->slots[i];
        uint64_t cur = sl.owner.load();
        const bool free = cur == 0 || cur == token || now - sl.heartbeat.load() > 5000;
        if (!free) continue;
        if (cur != token && !sl.owner.compare_exchange_strong(cur, token)) continue;
        sl.heartbeat.store(now);
        sl.meterBits.store(0);
        sl.channels.store(1);
        const uint32_t s = sl.seq.load(std::memory_order_relaxed);
        sl.seq.store(s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        storeString(sl.id, kIdWords, id);
        storeString(sl.cloneOf, kIdWords, cloneOf);
        storeString(sl.name, kNameWords, "");
        sl.seq.store(s + 2, std::memory_order_release);
        return i;
    }
    return -2;  // all slots taken
}

void SharedSession::releaseSlot(int slot, uint64_t token) {
    if (slot < 0 || slot >= kMaxSlots) return;
    uint64_t expected = token;
    r_->slots[slot].owner.compare_exchange_strong(expected, 0);
}

bool SharedSession::slotOwnedBy(int slot, uint64_t token) const {
    return slot >= 0 && slot < kMaxSlots && r_->slots[slot].owner.load() == token;
}

void SharedSession::heartbeatSlot(int slot, uint64_t token) {
    if (slotOwnedBy(slot, token)) r_->slots[slot].heartbeat.store(nowMs());
}

void SharedSession::setSlotName(int slot, uint64_t token, const std::string& name) {
    if (!slotOwnedBy(slot, token)) return;
    auto& sl = r_->slots[slot];
    const uint32_t s = sl.seq.load(std::memory_order_relaxed);
    sl.seq.store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    storeString(sl.name, kNameWords, name);
    sl.seq.store(s + 2, std::memory_order_release);
}

void SharedSession::setSlotChannels(int slot, uint64_t token, int channels) {
    if (!slotOwnedBy(slot, token)) return;
    r_->slots[slot].channels.store(static_cast<uint32_t>(std::max(1, std::min(channels, 16))), std::memory_order_release);
}

void SharedSession::addMeter(int slot, float peak) {
    if (slot < 0 || slot >= kMaxSlots || !(peak > 0)) return;
    auto& m = r_->slots[slot].meterBits;
    uint32_t cur = m.load(std::memory_order_relaxed);
    while (bitsFloat(cur) < peak && !m.compare_exchange_weak(cur, floatBits(peak), std::memory_order_relaxed)) {}
}

std::vector<SharedSession::LayerInfo> SharedSession::liveLayers(uint64_t staleMs, bool takeMeters) const {
    std::vector<LayerInfo> out;
    const uint64_t now = nowMs();
    for (int i = 0; i < kMaxSlots; ++i) {
        auto& sl = r_->slots[i];
        if (sl.owner.load() == 0) continue;
        const uint64_t hb = sl.heartbeat.load();
        if (now > hb && now - hb > staleMs) continue;
        LayerInfo info;
        for (int attempt = 0; attempt < 50; ++attempt) {
            const uint32_t s1 = sl.seq.load(std::memory_order_acquire);
            if (s1 & 1u) continue;
            info.id = loadString(sl.id, kIdWords);
            info.name = loadString(sl.name, kNameWords);
            info.cloneOf = loadString(sl.cloneOf, kIdWords);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (sl.seq.load(std::memory_order_relaxed) == s1) break;
        }
        if (info.id.empty()) continue;
        info.slot = i;
        info.channels = std::max(1u, std::min(16u, sl.channels.load(std::memory_order_acquire)));
        info.heartbeatMs = hb;
        const uint32_t m = takeMeters ? sl.meterBits.exchange(0, std::memory_order_relaxed) : sl.meterBits.load(std::memory_order_relaxed);
        info.meterPeak = bitsFloat(m);
        out.push_back(std::move(info));
    }
    return out;
}

bool SharedSession::idInUse(const std::string& id, uint64_t exceptToken, uint64_t staleMs) const {
    const uint64_t now = nowMs();
    for (int i = 0; i < kMaxSlots; ++i) {
        auto& sl = r_->slots[i];
        const uint64_t owner = sl.owner.load();
        if (owner == 0 || owner == exceptToken) continue;
        if (now - sl.heartbeat.load() > staleMs) continue;
        if (loadString(sl.id, kIdWords) == id) return true;
    }
    return false;
}

// ----------------------------------------------------------------- history

void SharedSession::record(int param, double t0, double t1, float v) {
    if (param < 0 || param >= kNumListenerParams || !(t1 > t0)) return;
    // A bin belongs to the block that contains its start, so each bin is
    // written by exactly one block whatever the host's block size, and a
    // replay writes the same values in the same places.
    const int b0 = static_cast<int>(std::ceil(t0 / kBinSeconds - 1e-9));
    const int b1 = std::min(static_cast<int>(std::ceil(t1 / kBinSeconds - 1e-9)) - 1, kMaxBins - 1);
    const uint32_t e = encodeBin(v);
    for (int b = b0; b <= b1; ++b) {
        auto& bin = r_->bins[param][b];
        if (bin.load(std::memory_order_relaxed) == e) continue;
        bin.store(e, std::memory_order_relaxed);
        if (param == kSpeed) r_->chunkVersion[b / kChunkBins].fetch_add(1, std::memory_order_release);
    }
}

float SharedSession::binValue(int param, int bin) const {
    if (bin < 0 || bin >= kMaxBins) return std::numeric_limits<float>::quiet_NaN();
    return decodeBin(r_->bins[param][bin].load(std::memory_order_relaxed));
}

void SharedSession::setBin(int param, int bin, float v) {
    if (bin < 0 || bin >= kMaxBins) return;
    r_->bins[param][bin].store(encodeBin(v), std::memory_order_relaxed);
    if (param == kSpeed) r_->chunkVersion[bin / kChunkBins].fetch_add(1, std::memory_order_release);
}

void SharedSession::clearHistory() {
    for (int p = 0; p < kNumListenerParams; ++p)
        for (int b = 0; b < kMaxBins; ++b)
            if (r_->bins[p][b].load(std::memory_order_relaxed) != 0) r_->bins[p][b].store(0, std::memory_order_relaxed);
    for (int c = 0; c < kNumChunks; ++c) r_->chunkVersion[c].fetch_add(1, std::memory_order_release);
    r_->historyEpoch.fetch_add(1, std::memory_order_release);
}

uint32_t SharedSession::chunkVersion(int chunk) const {
    return chunk >= 0 && chunk < kNumChunks ? r_->chunkVersion[chunk].load(std::memory_order_acquire) : 0;
}

uint32_t SharedSession::historyEpoch() const { return r_->historyEpoch.load(std::memory_order_acquire); }

// Format: lines "param start count value" (value as a float bit pattern in
// hex), one per run of equal recorded values.
std::string SharedSession::encodeHistory() const {
    std::ostringstream os;
    for (int p = 0; p < kNumListenerParams; ++p) {
        int b = 0;
        while (b < kMaxBins) {
            const uint32_t e = r_->bins[p][b].load(std::memory_order_relaxed);
            if (e == 0) { ++b; continue; }
            int n = 1;
            while (b + n < kMaxBins && r_->bins[p][b + n].load(std::memory_order_relaxed) == e) ++n;
            os << p << ' ' << b << ' ' << n << ' ' << std::hex << ~e << std::dec << '\n';
            b += n;
        }
    }
    return os.str();
}

void SharedSession::decodeHistory(const std::string& text) {
    clearHistory();
    std::istringstream is(text);
    int p = 0, b = 0, n = 0;
    uint32_t bits = 0;
    while (is >> p >> b >> n >> std::hex >> bits >> std::dec) {
        if (p < 0 || p >= kNumListenerParams || b < 0 || n <= 0) continue;
        const float v = bitsFloat(bits);
        for (int k = b; k < std::min(b + n, kMaxBins); ++k) r_->bins[p][k].store(encodeBin(v), std::memory_order_relaxed);
    }
    for (int c = 0; c < kNumChunks; ++c) r_->chunkVersion[c].fetch_add(1, std::memory_order_release);
}

}  // namespace spplug
