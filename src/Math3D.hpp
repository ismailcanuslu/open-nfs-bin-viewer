#pragma once

#include <cmath>
#include <cstring>
#include <algorithm>

namespace NFSU2 {

struct Vec2 {
    float x{0.0f}, y{0.0f};
    Vec2() = default;
    Vec2(float x, float y) : x(x), y(y) {}
};

struct Vec3 {
    float x{0.0f}, y{0.0f}, z{0.0f};
    Vec3() = default;
    Vec3(float x, float y, float z) : x(x), y(y), z(z) {}

    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3 operator/(float s) const { return {x / s, y / s, z / s}; }

    Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    Vec3& operator-=(const Vec3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
    Vec3& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }

    float lengthSq() const { return x * x + y * y + z * z; }
    float length() const { return std::sqrt(lengthSq()); }

    Vec3 normalized() const {
        float l = length();
        if (l > 1e-6f) return *this / l;
        return {0.0f, 0.0f, 0.0f};
    }

    static float dot(const Vec3& a, const Vec3& b) {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    static Vec3 cross(const Vec3& a, const Vec3& b) {
        return {
            a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x
        };
    }
};

struct Vec4 {
    float x{0.0f}, y{0.0f}, z{0.0f}, w{1.0f};
    Vec4() = default;
    Vec4(float x, float y, float z, float w = 1.0f) : x(x), y(y), z(z), w(w) {}
    Vec4(const Vec3& v, float w = 1.0f) : x(v.x), y(v.y), z(v.z), w(w) {}
};

// 4x4 Matrix, Column-Major storage (standard for OpenGL/Vulkan/GLSL)
struct Mat4 {
    float m[16]{};

    Mat4() {
        std::memset(m, 0, sizeof(m));
    }

    static Mat4 identity() {
        Mat4 res;
        res.m[0] = 1.0f;
        res.m[5] = 1.0f;
        res.m[10] = 1.0f;
        res.m[15] = 1.0f;
        return res;
    }

    Mat4 operator*(const Mat4& b) const {
        Mat4 res;
        for (int col = 0; col < 4; ++col) {
            for (int row = 0; row < 4; ++row) {
                float sum = 0.0f;
                for (int k = 0; k < 4; ++k) {
                    sum += m[k * 4 + row] * b.m[col * 4 + k];
                }
                res.m[col * 4 + row] = sum;
            }
        }
        return res;
    }

    static Mat4 translate(const Vec3& v) {
        Mat4 res = identity();
        res.m[12] = v.x;
        res.m[13] = v.y;
        res.m[14] = v.z;
        return res;
    }

    static Mat4 scale(const Vec3& s) {
        Mat4 res = identity();
        res.m[0] = s.x;
        res.m[5] = s.y;
        res.m[10] = s.z;
        return res;
    }

    static Mat4 rotateX(float rad) {
        Mat4 res = identity();
        float c = std::cos(rad), s = std::sin(rad);
        res.m[5] = c;
        res.m[6] = s;
        res.m[9] = -s;
        res.m[10] = c;
        return res;
    }

    static Mat4 rotateY(float rad) {
        Mat4 res = identity();
        float c = std::cos(rad), s = std::sin(rad);
        res.m[0] = c;
        res.m[2] = -s;
        res.m[8] = s;
        res.m[10] = c;
        return res;
    }

    static Mat4 rotateZ(float rad) {
        Mat4 res = identity();
        float c = std::cos(rad), s = std::sin(rad);
        res.m[0] = c;
        res.m[1] = s;
        res.m[4] = -s;
        res.m[5] = c;
        return res;
    }

    // Vulkan perspective projection: depth maps to [0, 1] and Y points down
    static Mat4 perspectiveVK(float fovYRad, float aspect, float zNear, float zFar) {
        Mat4 res;
        float tanHalfFov = std::tan(fovYRad * 0.5f);
        res.m[0] = 1.0f / (aspect * tanHalfFov);
        res.m[5] = -1.0f / tanHalfFov; // Vulkan Y-flipped
        res.m[10] = zFar / (zNear - zFar);
        res.m[11] = -1.0f;
        res.m[14] = -(zFar * zNear) / (zFar - zNear);
        return res;
    }

    static Mat4 lookAt(const Vec3& eye, const Vec3& target, const Vec3& up) {
        Vec3 f = (target - eye).normalized();
        Vec3 s = Vec3::cross(f, up).normalized();
        Vec3 u = Vec3::cross(s, f);

        Mat4 res = identity();
        res.m[0] = s.x;
        res.m[4] = s.y;
        res.m[8] = s.z;

        res.m[1] = u.x;
        res.m[5] = u.y;
        res.m[9] = u.z;

        res.m[2] = -f.x;
        res.m[6] = -f.y;
        res.m[10] = -f.z;

        res.m[12] = -Vec3::dot(s, eye);
        res.m[13] = -Vec3::dot(u, eye);
        res.m[14] = Vec3::dot(f, eye);
        return res;
    }
};

} // namespace NFSU2
