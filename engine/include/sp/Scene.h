// Scene model: layers, room, listener path and head control, environment.
//
// This is the data the 3D editor (thread 3) edits and the plugin (thread 4)
// persists. The engine only reads it. Every field here is plain data; the
// renderer takes an immutable copy. Serialization lives in SceneJson.h.
#pragma once

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "sp/Math.h"

namespace sp {

// ------------------------------------------------------------------ Layers

// Six octave bands used for materials: 125, 250, 500, 1k, 2k, 4k Hz.
constexpr int kNumBands = 6;
constexpr std::array<float, kNumBands> kBandCentresHz{125, 250, 500, 1000, 2000, 4000};

struct Layer {
    std::string name;
    std::string audioFile;          // used by the tools; the host supplies audio in a plugin
    Vec3 position{0, 1.6f, -2};     // metres, world frame
    float levelDb = 0;              // overall level
    bool mute = false;
    float dopplerAmount = 1.0f;     // 0 = none, 1 = physical
    float spreadDeg = 0;            // apparent source width, 0 = point source
    float directivity = 0;          // 0 = omni, 1 = cardioid
    Vec3 directivityForward{0, 0, 1};  // direction the source faces, world frame
    float referenceDistance = 1.0f; // distance at which level is 0 dB
    float minDistance = 0.25f;      // gain is clamped inside this radius
    float rolloff = 1.0f;           // 1 = inverse distance (-6 dB / doubling)
    float reverbSendDb = 0;         // trim for this layer's send into the room
    int reflectionOrder = -1;       // image-source order, -1 = use room default
    // Ray-traced back-end only: the source is a sphere of this radius for
    // partial occlusion by geometry between it and the listener.
    bool occlusion = true;
    float occlusionRadius = 0.5f;
    float startTime = 0;            // seconds on the timeline when the audio starts
    bool loop = false;
};

// -------------------------------------------------------------------- Room

struct Material {
    std::string name;
    std::array<float, kNumBands> absorption{0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f};
    // Ray-traced back-end only (Steam Audio): how much of a reflection is
    // scattered diffusely (0 = mirror, 1 = fully diffuse), and the energy
    // fraction transmitted through the surface at low / mid / high
    // frequencies when it sits between a layer and the listener.
    float scattering = 0.1f;
    std::array<float, 3> transmission{0, 0, 0};
};

namespace materials {
Material byName(const std::string& name);  // falls back to "plaster" for unknown names
std::vector<std::string> names();
}  // namespace materials

enum class RoomType { None, Box, Outdoor, Mesh };

// Triangle mesh in world space, metres: what the ray tracer sees. Three
// vertex indices per triangle and one material index per triangle.
struct MeshGeometry {
    std::vector<Vec3> vertices;
    std::vector<std::array<int, 3>> triangles;
    std::vector<int> materialIndices;
    std::vector<Material> materials;

    bool empty() const { return triangles.empty(); }
    void append(const MeshGeometry& other);
    int addMaterial(const Material& m);  // returns its index (de-duplicated by name + values)
    void addBox(const Vec3& minCorner, const Vec3& maxCorner, int material, bool normalsInward);
    void addQuad(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d, int material);  // a-b-c-d in order
    Vec3 minCorner() const;
    Vec3 maxCorner() const;
    float surfaceArea() const;
};

// An axis-aligned box placed in the scene: a wall segment, a pillar, a
// piece of furniture. Blocks and reflects sound in the ray-traced back-end;
// the image-source model ignores it.
struct SceneObject {
    std::string name;
    Vec3 minCorner{-0.5f, 0, -0.5f};
    Vec3 maxCorner{0.5f, 1, 0.5f};
    Material material = materials::byName("wood_panel");
};

// Walls of a box room, indexed by the axis they are perpendicular to.
enum Wall : int { WallNegX = 0, WallPosX, WallNegY /*floor*/, WallPosY /*ceiling*/, WallNegZ, WallPosZ, kNumWalls };

// A measured or library impulse response as the late reverb of a box room,
// in place of the built-in FDN. The moving image-source reflections stay
// (turn `reflectionsEnabled` off if the IR carries its own early part). The
// IR is normalised to unit energy, so the room's calibrated reverb level and
// the per-layer sends still apply and `gainDb` is a trim.
struct ImpulseResponse {
    std::string file;       // WAV; a relative path is resolved against the scene file when loaded from JSON
    float gainDb = 0;
    int channels = 0;       // 0 = from the file; 1 mono (diffuse), 2 stereo L/R (head-relative), 4 first-order ambiX (ACN/SN3D, world-fixed)
    bool enabled = true;    // false keeps the file in the scene but plays the built-in reverb
    bool active() const { return enabled && !file.empty(); }
};

struct Room {
    RoomType type = RoomType::Box;
    Vec3 size{8, 3, 10};            // width (x), height (y), depth (z) in metres
    Vec3 origin{0, 0, 0};           // position of the floor centre in world space
    std::array<Material, kNumWalls> materials = {
        materials::byName("plaster"), materials::byName("plaster"),
        materials::byName("wood_floor"), materials::byName("plaster"),
        materials::byName("plaster"), materials::byName("plaster")};
    int reflectionOrder = 2;        // image-source order (2 -> about 25 images)
    float reflectionsLevelDb = 0;   // trim for early reflections
    float reverbLevelDb = 0;        // trim for late reverb
    float reverbTimeScale = 1.0f;   // multiplies the Eyring RT60
    bool reflectionsEnabled = true;
    bool reverbEnabled = true;
    ImpulseResponse impulseResponse;  // optional; replaces the FDN when active() and the file loads

    // RoomType::Mesh: the enclosure as a triangle mesh, loaded by SceneJson
    // from `meshFile` (Wavefront OBJ, `usemtl` names pick materials) or
    // given inline. Only the ray-traced back-end can use it; without Steam
    // Audio a Mesh room renders as free field.
    std::string meshFile;
    MeshGeometry mesh;
    // Extra geometry inside any room type (occluders, reflectors). Having
    // any selects the ray-traced back-end when the renderer is on Auto.
    std::vector<SceneObject> objects;

    // Bounds in world space (Mesh rooms: of the mesh).
    Vec3 minCorner() const { return type == RoomType::Mesh && !mesh.empty() ? mesh.minCorner() : origin - Vec3{size.x * 0.5f, 0, size.z * 0.5f}; }
    Vec3 maxCorner() const { return type == RoomType::Mesh && !mesh.empty() ? mesh.maxCorner() : origin + Vec3{size.x * 0.5f, size.y, size.z * 0.5f}; }
};

// Everything the ray tracer should see for a room: box walls (Box), the
// ground plane (Outdoor) or the mesh (Mesh), plus the objects.
MeshGeometry roomGeometry(const Room& room);

// Wavefront OBJ loader for Mesh rooms: `v` and `f` (polygons are fanned),
// `usemtl NAME` picks the material via `materialFor(NAME)`. Throws on I/O
// or parse errors.
MeshGeometry loadObjMesh(const std::string& path, const std::function<Material(const std::string&)>& materialFor);

// ------------------------------------------------------------------- Paths

enum class SegmentType { Line, CubicBezier, CatmullRom, Arc };

// One path segment. Interpretation of `points` by type:
//   Line:        p0, p1
//   CubicBezier: p0, c0, c1, p1
//   CatmullRom:  control points (>= 2), the curve passes through all of them
//   Arc:         centre, start point, end point (circular arc in the plane
//                through the three, going the short way from start to end;
//                `arcTurns` adds whole extra revolutions)
struct PathSegment {
    SegmentType type = SegmentType::Line;
    std::vector<Vec3> points;
    int arcTurns = 0;
    bool arcClockwise = false;  // seen from +Y; only used when the arc spans > 180 degrees
};

struct Path {
    std::string name;
    std::vector<PathSegment> segments;
    bool closed = false;  // joins the end back to the start with a line
};

// ------------------------------------------------------------------ Speed

enum class Easing { Linear, SmoothStep, EaseIn, EaseOut, Hold };

struct SpeedKey {
    double time = 0;      // seconds
    float speed = 1.4f;   // m/s at this key
    Easing easing = Easing::Linear;  // interpolation towards the next key
};

struct SpeedCurve {
    std::vector<SpeedKey> keys;  // sorted by time; empty = constant 1.4 m/s
};

// ------------------------------------------------------------ Head control

enum class HeadMode { AlongPath, LookAt, Keyframed };

struct HeadKey {
    double time = 0;
    float yawDeg = 0;
    float pitchDeg = 0;
    float rollDeg = 0;
    Easing easing = Easing::SmoothStep;
};

struct HeadTrack {
    HeadMode mode = HeadMode::AlongPath;
    bool banking = false;               // AlongPath: roll into turns
    Vec3 lookAtPoint{0, 1.6f, 0};       // LookAt: target point...
    int lookAtLayer = -1;               // ...or a layer index (wins when >= 0)
    std::vector<HeadKey> keys;          // Keyframed: the head direction; other modes: a turn on top of it
    float yawOffsetDeg = 0;             // added on top of any mode
    float pitchOffsetDeg = 0;
    float rollOffsetDeg = 0;
};

// --------------------------------------------------------------- Listener

enum class PositionMode { Speed, AlongPath };

struct Listener {
    std::vector<Path> paths;
    int activePath = 0;
    PositionMode positionMode = PositionMode::Speed;
    SpeedCurve speed;
    double pathStartTime = 0;   // seconds; the listener waits at the start before this
    float pathFraction = 0;     // AlongPath mode: 0..1 along the active path
    bool loopPath = false;      // wrap around when the end is reached (Speed mode)
    Vec3 staticPosition{0, 1.6f, 0};  // used when there is no path
    HeadTrack head;
    float headRadius = 0.0875f; // metres, for near-field ear parallax
};

// ------------------------------------------------------------ Environment

struct Environment {
    float speedOfSound = 343.0f;  // m/s
    float temperatureC = 20.0f;
    float relativeHumidity = 50.0f;  // percent
    float pressureKPa = 101.325f;
    bool airAbsorption = true;
};

// ------------------------------------------------------------------ Scene

struct Scene {
    std::string name;
    std::vector<Layer> layers;
    Room room;
    Listener listener;
    Environment environment;
    double duration = 0;  // seconds; 0 = derived from the audio by the tools
};

}  // namespace sp
