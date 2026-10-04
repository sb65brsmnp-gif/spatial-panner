// Triangle geometry for the ray-traced back-end: box rooms and objects as
// meshes, ground planes, and a small Wavefront OBJ reader.
#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "sp/Scene.h"

namespace sp {

void MeshGeometry::append(const MeshGeometry& o) {
    const int vBase = static_cast<int>(vertices.size());
    std::vector<int> matMap(o.materials.size());
    for (size_t i = 0; i < o.materials.size(); ++i) matMap[i] = addMaterial(o.materials[i]);
    vertices.insert(vertices.end(), o.vertices.begin(), o.vertices.end());
    for (size_t t = 0; t < o.triangles.size(); ++t) {
        triangles.push_back({o.triangles[t][0] + vBase, o.triangles[t][1] + vBase, o.triangles[t][2] + vBase});
        const int m = t < o.materialIndices.size() ? o.materialIndices[t] : 0;
        materialIndices.push_back(m >= 0 && m < static_cast<int>(matMap.size()) ? matMap[m] : 0);
    }
}

int MeshGeometry::addMaterial(const Material& m) {
    for (size_t i = 0; i < materials.size(); ++i) {
        const Material& e = materials[i];
        if (e.name == m.name && e.absorption == m.absorption && e.scattering == m.scattering && e.transmission == m.transmission)
            return static_cast<int>(i);
    }
    materials.push_back(m);
    return static_cast<int>(materials.size()) - 1;
}

void MeshGeometry::addQuad(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d, int material) {
    const int base = static_cast<int>(vertices.size());
    vertices.insert(vertices.end(), {a, b, c, d});
    triangles.push_back({base, base + 1, base + 2});
    triangles.push_back({base, base + 2, base + 3});
    materialIndices.push_back(material);
    materialIndices.push_back(material);
}

void MeshGeometry::addBox(const Vec3& mn, const Vec3& mx, int material, bool inward) {
    // Vertices: bit 0 = x, bit 1 = y, bit 2 = z (0 = min, 1 = max).
    Vec3 v[8];
    for (int i = 0; i < 8; ++i) v[i] = {(i & 1) ? mx.x : mn.x, (i & 2) ? mx.y : mn.y, (i & 4) ? mx.z : mn.z};
    // Faces wound counter-clockwise seen from outside.
    const int faces[6][4] = {
        {0, 4, 6, 2},  // -x
        {1, 3, 7, 5},  // +x
        {0, 1, 5, 4},  // -y (floor)
        {2, 6, 7, 3},  // +y (ceiling)
        {0, 2, 3, 1},  // -z
        {4, 5, 7, 6},  // +z
    };
    for (const auto& f : faces) {
        if (inward) addQuad(v[f[3]], v[f[2]], v[f[1]], v[f[0]], material);
        else addQuad(v[f[0]], v[f[1]], v[f[2]], v[f[3]], material);
    }
}

Vec3 MeshGeometry::minCorner() const {
    Vec3 m{1e30f, 1e30f, 1e30f};
    for (const auto& v : vertices) m = {std::min(m.x, v.x), std::min(m.y, v.y), std::min(m.z, v.z)};
    return vertices.empty() ? Vec3{0, 0, 0} : m;
}

Vec3 MeshGeometry::maxCorner() const {
    Vec3 m{-1e30f, -1e30f, -1e30f};
    for (const auto& v : vertices) m = {std::max(m.x, v.x), std::max(m.y, v.y), std::max(m.z, v.z)};
    return vertices.empty() ? Vec3{0, 0, 0} : m;
}

float MeshGeometry::surfaceArea() const {
    float a = 0;
    for (const auto& t : triangles) {
        const Vec3& p0 = vertices[t[0]];
        const Vec3 e1 = vertices[t[1]] - p0, e2 = vertices[t[2]] - p0;
        a += 0.5f * e1.cross(e2).length();
    }
    return a;
}

MeshGeometry roomGeometry(const Room& room) {
    MeshGeometry g;
    switch (room.type) {
        case RoomType::Box: {
            // One material per wall; faces point into the room.
            const Vec3 mn = room.minCorner(), mx = room.maxCorner();
            MeshGeometry box;
            int mat[kNumWalls];
            for (int w = 0; w < kNumWalls; ++w) mat[w] = box.addMaterial(room.materials[w]);
            // Same face order as addBox: -x +x -y +y -z +z == Wall enum order.
            Vec3 v[8];
            for (int i = 0; i < 8; ++i) v[i] = {(i & 1) ? mx.x : mn.x, (i & 2) ? mx.y : mn.y, (i & 4) ? mx.z : mn.z};
            const int faces[6][4] = {{0, 4, 6, 2}, {1, 3, 7, 5}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 2, 3, 1}, {4, 5, 7, 6}};
            for (int w = 0; w < kNumWalls; ++w) {
                const int* f = faces[w];
                box.addQuad(v[f[3]], v[f[2]], v[f[1]], v[f[0]], mat[w]);
            }
            g.append(box);
            break;
        }
        case RoomType::Outdoor: {
            // Ground plane, 4 km across, at the room origin's height.
            MeshGeometry ground;
            const int m = ground.addMaterial(room.materials[WallNegY]);
            const float R = 2000.0f, y = room.origin.y;
            ground.addQuad({room.origin.x - R, y, room.origin.z - R}, {room.origin.x - R, y, room.origin.z + R},
                           {room.origin.x + R, y, room.origin.z + R}, {room.origin.x + R, y, room.origin.z - R}, m);
            g.append(ground);
            break;
        }
        case RoomType::Mesh:
            g.append(room.mesh);
            break;
        case RoomType::None:
            break;
    }
    for (const auto& o : room.objects) {
        MeshGeometry b;
        const int m = b.addMaterial(o.material);
        const Vec3 mn{std::min(o.minCorner.x, o.maxCorner.x), std::min(o.minCorner.y, o.maxCorner.y), std::min(o.minCorner.z, o.maxCorner.z)};
        const Vec3 mx{std::max(o.minCorner.x, o.maxCorner.x), std::max(o.minCorner.y, o.maxCorner.y), std::max(o.minCorner.z, o.maxCorner.z)};
        b.addBox(mn, mx, m, false);
        g.append(b);
    }
    return g;
}

MeshGeometry loadObjMesh(const std::string& path, const std::function<Material(const std::string&)>& materialFor) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("Cannot open mesh file '" + path + "'");
    MeshGeometry g;
    int currentMaterial = g.addMaterial(materialFor(""));
    std::string line;
    int lineNo = 0;
    auto parseIndex = [&](const std::string& tok) {
        // "v", "v/vt", "v//vn", "v/vt/vn"; negative indices count from the end.
        const size_t slash = tok.find('/');
        const std::string vs = slash == std::string::npos ? tok : tok.substr(0, slash);
        int idx = std::stoi(vs);
        if (idx < 0) idx = static_cast<int>(g.vertices.size()) + idx;
        else idx -= 1;
        if (idx < 0 || idx >= static_cast<int>(g.vertices.size()))
            throw std::runtime_error(path + ":" + std::to_string(lineNo) + ": vertex index out of range");
        return idx;
    };
    while (std::getline(in, line)) {
        ++lineNo;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream ss(line);
        std::string key;
        if (!(ss >> key) || key[0] == '#') continue;
        if (key == "v") {
            Vec3 v;
            if (!(ss >> v.x >> v.y >> v.z)) throw std::runtime_error(path + ":" + std::to_string(lineNo) + ": bad vertex");
            g.vertices.push_back(v);
        } else if (key == "f") {
            std::vector<int> idx;
            std::string tok;
            while (ss >> tok) idx.push_back(parseIndex(tok));
            if (idx.size() < 3) throw std::runtime_error(path + ":" + std::to_string(lineNo) + ": face needs 3+ vertices");
            for (size_t i = 1; i + 1 < idx.size(); ++i) {
                g.triangles.push_back({idx[0], idx[i], idx[i + 1]});
                g.materialIndices.push_back(currentMaterial);
            }
        } else if (key == "usemtl") {
            std::string name;
            ss >> name;
            currentMaterial = g.addMaterial(materialFor(name));
        }
        // vn, vt, g, o, s, mtllib: ignored.
    }
    if (g.triangles.empty()) throw std::runtime_error("Mesh file '" + path + "' has no faces");
    return g;
}

}  // namespace sp
