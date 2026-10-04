// What the plugin instances in one Logic session share, and nothing else:
//
//   * the scene document, published by the scene instance (JSON text),
//   * a table of layer slots, one per layer instance (its id, track name,
//     level meter), so the scene instance knows which tracks exist,
//   * the listener automation the scene instance has played, indexed by
//     timeline time ("history"), so every layer instance evaluates the same
//     listener controls at the same timeline time, whatever order the host
//     processes tracks in and however far ahead it renders some of them.
//
// No audio crosses instances: every layer instance renders itself from the
// scene and the host playhead (docs/plugin.md).
//
// The region is a fixed-layout block of lock-free atomics in a memory-mapped
// file, so it works whether the host loads all instances into one process
// (the usual case) or spreads them over several (Logic's AUHostingService).
// When the file cannot be mapped (a sandbox, a read-only home), the region
// falls back to process memory and only instances in this process share it.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace spplug {

// Listener controls the scene instance automates, recorded per time bin.
enum ListenerParam : int {
    kSpeed = 0,     // speed multiplier on the scene's speed curve
    kPosition,      // 0..1 along the active path ("position along path" mode)
    kYaw,           // degrees, added to the scene's head direction
    kPitch,
    kRoll,
    kActivePath,    // -1 = the scene's own choice, else a path index
    kNumListenerParams
};

class SharedSession {
public:
    static constexpr int kMaxSlots = 256;
    static constexpr int kIdWords = 5;           // 40 bytes
    static constexpr int kNameWords = 12;        // 96 bytes
    static constexpr double kBinSeconds = 0.01;  // history resolution
    static constexpr int kMaxBins = 2 * 3600 * 100;  // two hours
    static constexpr int kChunkBins = 256;       // speed integration cache granularity
    static constexpr int kNumChunks = (kMaxBins + kChunkBins - 1) / kChunkBins;
    static constexpr size_t kMaxSceneBytes = 2u << 20;

    // The session every instance in this process attaches to. `path` empty =
    // the default file (~/Library/Application Support/Spatial Panner).
    static std::shared_ptr<SharedSession> attach(const std::string& path = {});
    // Tests: forget the cached session so the next attach() starts fresh.
    static void detachAllForTesting();

    ~SharedSession();
    bool isShared() const { return mapped_; }   // false: process-local fallback
    const std::string& path() const { return path_; }

    static uint64_t nowMs();

    // ---- scene ownership (one scene instance per session)
    // Takes ownership if nobody holds it, or the holder stopped refreshing
    // its heartbeat for `staleMs`, or `force`. Returns true when `token` owns it.
    bool claimScene(uint64_t token, bool force, uint64_t staleMs = 3000);
    void releaseScene(uint64_t token);
    uint64_t sceneOwner() const;
    bool sceneAlive(uint64_t staleMs = 3000) const;  // an owner that is refreshing its heartbeat
    void heartbeatScene(uint64_t token);
    uint32_t ownerGeneration() const;   // bumps on every change of owner

    // ---- the scene document (owner writes, everyone reads)
    // Returns the new revision. Text longer than kMaxSceneBytes is rejected (0).
    uint64_t publishScene(uint64_t token, const std::string& json);
    uint64_t sceneRevision() const;
    // Copies the published text if its revision differs from `known`.
    bool readScene(uint64_t& revision, std::string& json) const;

    // ---- layer slots
    struct LayerInfo {
        int slot = -1;
        std::string id, name, cloneOf;
        int channels = 1;      // the track's input channels: 1 (mono) or 2 (stereo)
        float meterPeak = 0;
        uint64_t heartbeatMs = 0;
    };
    // Claims a slot for `id`. If another live instance already uses `id`
    // (a duplicated track), returns -1 so the caller picks a new id.
    int claimSlot(uint64_t token, const std::string& id, const std::string& cloneOf = {});
    void releaseSlot(int slot, uint64_t token);
    bool slotOwnedBy(int slot, uint64_t token) const;
    void heartbeatSlot(int slot, uint64_t token);
    void setSlotName(int slot, uint64_t token, const std::string& name);
    void setSlotChannels(int slot, uint64_t token, int channels);
    void addMeter(int slot, float peak);      // audio thread; peak hold until read
    std::vector<LayerInfo> liveLayers(uint64_t staleMs = 5000, bool takeMeters = true) const;
    bool idInUse(const std::string& id, uint64_t exceptToken, uint64_t staleMs = 5000) const;

    // ---- listener automation history (owner writes on its audio thread)
    // Current values (what an unwritten bin falls back to).
    void setLive(int param, float v) { live_(param).store(v, std::memory_order_relaxed); }
    float live(int param) const { return live_(param).load(std::memory_order_relaxed); }

    // Records `v` for every bin that starts in [t0, t1) seconds.
    void record(int param, double t0, double t1, float v);
    // The recorded value of a bin, or NaN when it was never played.
    float binValue(int param, int bin) const;
    void setBin(int param, int bin, float v);   // NaN clears it
    void clearHistory();
    uint32_t chunkVersion(int chunk) const;
    uint32_t historyEpoch() const;  // bumps on clearHistory()

    static int binOf(double t) { return t <= 0 ? 0 : static_cast<int>(t / kBinSeconds); }

    // Run-length encoding of the written bins (for the scene instance's state).
    std::string encodeHistory() const;
    void decodeHistory(const std::string& text);

    struct Region;  // the mapped layout (SharedSession.cpp)

private:
    SharedSession() = default;
    Region* r_ = nullptr;
    size_t size_ = 0;
    bool mapped_ = false;
    std::string path_;
    std::unique_ptr<uint8_t[]> local_;

    std::atomic<float>& live_(int p) const;
};

}  // namespace spplug
