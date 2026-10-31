#include "render/MeshGeometry.h"

#include <cmath>
#include <iterator>

using namespace DirectX;

namespace lm {

namespace {

constexpr float kTwoPi = 6.28318530718f;
constexpr uint32_t kWhite = 0xFFFFFFFFu; // built-in meshes take their color from the instance

XMFLOAT3 Add(XMFLOAT3 a, XMFLOAT3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
XMFLOAT3 Scale(XMFLOAT3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
XMFLOAT3 Cross(XMFLOAT3 a, XMFLOAT3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
XMFLOAT3 Normalize(XMFLOAT3 a)
{
    const float length = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
    return length > 0.0f ? Scale(a, 1.0f / length) : a;
}

// Appends shapes to one mesh. Indices are relative to the mesh's first vertex.
class MeshBuilder {
public:
    MeshBuilder(MeshGeometry& geometry, MeshId mesh) : m_geometry(geometry), m_mesh(mesh)
    {
        m_range.firstIndex = static_cast<uint32_t>(geometry.indices.size());
        m_range.baseVertex = static_cast<int32_t>(geometry.vertices.size());
    }
    ~MeshBuilder()
    {
        m_range.indexCount = static_cast<uint32_t>(m_geometry.indices.size()) - m_range.firstIndex;
        m_range.vertexCount = static_cast<uint32_t>(m_geometry.vertices.size()) - static_cast<uint32_t>(m_range.baseVertex);
        m_geometry.ranges[static_cast<uint32_t>(m_mesh)] = m_range;
    }

    uint32_t Vertex(XMFLOAT3 position, XMFLOAT3 normal)
    {
        m_geometry.vertices.push_back({position, normal, kWhite});
        return static_cast<uint32_t>(m_geometry.vertices.size() - 1 - static_cast<size_t>(m_range.baseVertex));
    }
    void Triangle(uint32_t a, uint32_t b, uint32_t c)
    {
        m_geometry.indices.insert(m_geometry.indices.end(), {a, b, c});
    }

    // Flat quad facing `normal`. `u` and `v` are half-size edge vectors with cross(u, v) along normal.
    void Quad(XMFLOAT3 center, XMFLOAT3 normal, XMFLOAT3 u, XMFLOAT3 v)
    {
        const uint32_t a = Vertex(Add(center, Scale(Add(u, v), -1.0f)), normal);
        const uint32_t b = Vertex(Add(center, Add(u, Scale(v, -1.0f))), normal);
        const uint32_t c = Vertex(Add(center, Add(u, v)), normal);
        const uint32_t d = Vertex(Add(center, Add(Scale(u, -1.0f), v)), normal);
        Triangle(a, b, c);
        Triangle(a, c, d);
    }

    void Box(XMFLOAT3 center, XMFLOAT3 half)
    {
        const float h[3] = {half.x, half.y, half.z};
        for (int axis = 0; axis < 3; ++axis) {
            for (float sign : {1.0f, -1.0f}) {
                XMFLOAT3 n{0, 0, 0};
                (&n.x)[axis] = sign;
                XMFLOAT3 uDir{0, 0, 0};
                (&uDir.x)[(axis + 1) % 3] = 1.0f;
                const XMFLOAT3 vDir = Cross(n, uDir); // cross(uDir, vDir) == n
                Quad(Add(center, Scale(n, h[axis])), n, Scale(uDir, h[(axis + 1) % 3]), Scale(vDir, h[(axis + 2) % 3]));
            }
        }
    }

    // Unit sphere around the origin.
    void Sphere(int rings, int segments)
    {
        const uint32_t first = Vertex({0, 0, -1}, {0, 0, -1});
        for (int i = 0; i <= rings; ++i) {
            const float phi = -XM_PIDIV2 + XM_PI * float(i) / float(rings);
            for (int j = 0; j <= segments; ++j) {
                const float theta = kTwoPi * float(j) / float(segments);
                const XMFLOAT3 p{std::cos(phi) * std::cos(theta), std::cos(phi) * std::sin(theta), std::sin(phi)};
                if (i == 0 && j == 0)
                    continue; // already added as `first`
                Vertex(p, p);
            }
        }
        const int stride = segments + 1;
        for (int i = 0; i < rings; ++i) {
            for (int j = 0; j < segments; ++j) {
                const auto a = static_cast<uint32_t>(first + i * stride + j);
                const auto b = static_cast<uint32_t>(a + 1);
                const auto c = static_cast<uint32_t>(b + stride);
                const auto d = static_cast<uint32_t>(a + stride);
                if (i != 0)
                    Triangle(a, b, c); // degenerate at the south pole
                if (i != rings - 1)
                    Triangle(a, c, d); // degenerate at the north pole
            }
        }
    }

    // Cylinder from `base` along unit `axis`; `u`, `v` unit vectors with cross(u, v) == axis.
    void Cylinder(XMFLOAT3 base, XMFLOAT3 axis, XMFLOAT3 u, XMFLOAT3 v, float radius, float length, int segments)
    {
        const XMFLOAT3 top = Add(base, Scale(axis, length));
        uint32_t firstSide = 0;
        for (int j = 0; j <= segments; ++j) {
            const XMFLOAT3 dir = RingDirection(u, v, j, segments);
            const uint32_t bottom = Vertex(Add(base, Scale(dir, radius)), dir);
            Vertex(Add(top, Scale(dir, radius)), dir);
            if (j == 0)
                firstSide = bottom;
        }
        for (int j = 0; j < segments; ++j) {
            const auto b0 = static_cast<uint32_t>(firstSide + 2 * j);
            const auto t0 = static_cast<uint32_t>(b0 + 1);
            const auto b1 = static_cast<uint32_t>(b0 + 2);
            const auto t1 = static_cast<uint32_t>(b0 + 3);
            Triangle(b0, b1, t1);
            Triangle(b0, t1, t0);
        }
        Disc(top, axis, u, v, radius, segments, true);
        Disc(base, axis, u, v, radius, segments, false);
    }

    // Cone from a base disc at `base` to an apex `length` along unit `axis`.
    void Cone(XMFLOAT3 base, XMFLOAT3 axis, XMFLOAT3 u, XMFLOAT3 v, float radius, float length, int segments)
    {
        const XMFLOAT3 apex = Add(base, Scale(axis, length));
        for (int j = 0; j < segments; ++j) {
            const XMFLOAT3 dir0 = RingDirection(u, v, j, segments);
            const XMFLOAT3 dir1 = RingDirection(u, v, j + 1, segments);
            const XMFLOAT3 n0 = Normalize(Add(Scale(dir0, length), Scale(axis, radius)));
            const XMFLOAT3 n1 = Normalize(Add(Scale(dir1, length), Scale(axis, radius)));
            const XMFLOAT3 nMid = Normalize(Add(n0, n1));
            const uint32_t a = Vertex(Add(base, Scale(dir0, radius)), n0);
            const uint32_t b = Vertex(Add(base, Scale(dir1, radius)), n1);
            const uint32_t c = Vertex(apex, nMid);
            Triangle(a, b, c);
        }
        Disc(base, axis, u, v, radius, segments, false);
    }

private:
    static XMFLOAT3 RingDirection(XMFLOAT3 u, XMFLOAT3 v, int j, int segments)
    {
        const float theta = kTwoPi * float(j % segments) / float(segments);
        return Add(Scale(u, std::cos(theta)), Scale(v, std::sin(theta)));
    }

    // Flat disc; faces +axis if `facingAxis`, otherwise -axis.
    void Disc(XMFLOAT3 center, XMFLOAT3 axis, XMFLOAT3 u, XMFLOAT3 v, float radius, int segments, bool facingAxis)
    {
        const XMFLOAT3 normal = facingAxis ? axis : Scale(axis, -1.0f);
        const uint32_t c = Vertex(center, normal);
        const uint32_t ring = Vertex(Add(center, Scale(RingDirection(u, v, 0, segments), radius)), normal);
        for (int j = 1; j < segments; ++j)
            Vertex(Add(center, Scale(RingDirection(u, v, j, segments), radius)), normal);
        for (int j = 0; j < segments; ++j) {
            const auto p0 = static_cast<uint32_t>(ring + j);
            const auto p1 = static_cast<uint32_t>(ring + (j + 1) % segments);
            if (facingAxis)
                Triangle(c, p0, p1);
            else
                Triangle(c, p1, p0);
        }
    }

    MeshGeometry& m_geometry;
    MeshId m_mesh;
    MeshRange m_range;
};

} // namespace

MeshGeometry BuildBuiltinMeshes()
{
    MeshGeometry geometry;
    const XMFLOAT3 x{1, 0, 0}, y{0, 1, 0}, z{0, 0, 1};

    { MeshBuilder b(geometry, MeshId::Cube); b.Box({0, 0, 0}, {1, 1, 1}); }
    { MeshBuilder b(geometry, MeshId::Sphere); b.Sphere(16, 28); }
    { MeshBuilder b(geometry, MeshId::Cylinder); b.Cylinder({0, 0, -1}, z, x, y, 1.0f, 2.0f, 28); }
    {
        // Spawn marker: body + rounded head, and an arrow along +X showing the facing direction.
        MeshBuilder b(geometry, MeshId::Spawn);
        b.Cylinder({0, 0, -1}, z, x, y, 0.7f, 1.45f, 20);
        b.Cone({0, 0, 0.45f}, z, x, y, 0.7f, 0.55f, 20);
        b.Cylinder({0.55f, 0, -0.55f}, x, y, z, 0.14f, 0.6f, 12);
        b.Cone({1.15f, 0, -0.55f}, x, y, z, 0.32f, 0.5f, 12);
    }
    {
        // Wire box: 8 corners (bit 0 = +x, bit 1 = +y, bit 2 = +z) and 12 edges as a line list.
        MeshRange& range = geometry.wireBox;
        range.firstIndex = static_cast<uint32_t>(geometry.indices.size());
        range.baseVertex = static_cast<int32_t>(geometry.vertices.size());
        for (int i = 0; i < 8; ++i)
            geometry.vertices.push_back({{(i & 1) ? 1.0f : -1.0f, (i & 2) ? 1.0f : -1.0f, (i & 4) ? 1.0f : -1.0f}, {0, 0, 0}});
        static constexpr uint32_t kEdges[24] = {0, 1, 2, 3, 4, 5, 6, 7, 0, 2, 1, 3, 4, 6, 5, 7, 0, 4, 1, 5, 2, 6, 3, 7};
        geometry.indices.insert(geometry.indices.end(), std::begin(kEdges), std::end(kEdges));
        range.indexCount = 24;
        range.vertexCount = 8;
    }
    return geometry;
}

} // namespace lm
