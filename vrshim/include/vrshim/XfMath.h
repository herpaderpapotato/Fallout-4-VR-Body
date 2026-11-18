#pragma once
// FRIK-flat: small double-precision rigid-transform math in fo4flatvr Hands.cpp's convention (column vectors; NiTransform
// storage entry[i] = local axis i in the parent = column i of R, see FromNi / ToNi). Hands verified this convention in game
// (WEAPON residual 0.000), so vrshim composes transforms with it rather than with NiTransform's operators.
#include <cmath>

namespace vrshim::xf
{
    struct V
    {
        double x{}, y{}, z{};

        V operator+(const V& o) const
        {
            return { x + o.x, y + o.y, z + o.z };
        }

        V operator-(const V& o) const
        {
            return { x - o.x, y - o.y, z - o.z };
        }

        V operator*(double s) const
        {
            return { x * s, y * s, z * s };
        }
    };

    struct M3
    {
        double m[3][3]{ { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
    };

    struct Xf
    {
        M3 R;
        V t;
        double s{ 1.0 };
    };

    inline M3 Mul(const M3& a, const M3& b)
    {
        M3 r;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
        return r;
    }

    inline V Mul(const M3& a, const V& v)
    {
        return { a.m[0][0] * v.x + a.m[0][1] * v.y + a.m[0][2] * v.z, a.m[1][0] * v.x + a.m[1][1] * v.y + a.m[1][2] * v.z, a.m[2][0] * v.x + a.m[2][1] * v.y + a.m[2][2] * v.z };
    }

    inline M3 T(const M3& a)
    {
        M3 r;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                r.m[i][j] = a.m[j][i];
        return r;
    }

    inline Xf Mul(const Xf& a, const Xf& b)
    {
        return { Mul(a.R, b.R), a.t + Mul(a.R, b.t) * a.s, a.s * b.s };
    }

    inline Xf Inv(const Xf& a)
    {
        Xf r;
        r.R = T(a.R);
        r.s = 1.0 / a.s;
        r.t = Mul(r.R, a.t) * (-r.s);
        return r;
    }

    inline M3 Rx(double a)
    {
        M3 r;
        const double c = std::cos(a), s = std::sin(a);
        r.m[1][1] = c, r.m[1][2] = -s, r.m[2][1] = s, r.m[2][2] = c;
        return r;
    }

    inline M3 Ry(double a)
    {
        M3 r;
        const double c = std::cos(a), s = std::sin(a);
        r.m[0][0] = c, r.m[0][2] = s, r.m[2][0] = -s, r.m[2][2] = c;
        return r;
    }

    inline M3 Rz(double a)
    {
        M3 r;
        const double c = std::cos(a), s = std::sin(a);
        r.m[0][0] = c, r.m[0][1] = -s, r.m[1][0] = s, r.m[1][1] = c;
        return r;
    }

    // Gram-Schmidt on the columns (the snapshot is float).
    inline M3 Ortho(const M3& a)
    {
        V c0{ a.m[0][0], a.m[1][0], a.m[2][0] }, c1{ a.m[0][1], a.m[1][1], a.m[2][1] };
        auto dot = [](const V& p, const V& q) {
            return p.x * q.x + p.y * q.y + p.z * q.z;
        };
        auto nrm = [&](const V& p) {
            const double l = std::sqrt(dot(p, p));
            return l > 1e-9 ? p * (1.0 / l) : V{ 1, 0, 0 };
        };
        c0 = nrm(c0);
        c1 = nrm(c1 - c0 * dot(c0, c1));
        const V c2{ c0.y * c1.z - c0.z * c1.y, c0.z * c1.x - c0.x * c1.z, c0.x * c1.y - c0.y * c1.x };
        M3 r;
        r.m[0][0] = c0.x, r.m[1][0] = c0.y, r.m[2][0] = c0.z;
        r.m[0][1] = c1.x, r.m[1][1] = c1.y, r.m[2][1] = c1.z;
        r.m[0][2] = c2.x, r.m[1][2] = c2.y, r.m[2][2] = c2.z;
        return r;
    }

    // NiTransform storage: entry[i] = local axis i in the parent (= column i of R)
    inline Xf FromNi(const RE::NiTransform& n)
    {
        Xf x;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                x.R.m[r][c] = n.rotate.entry[c][r];
        x.t = { n.translate.x, n.translate.y, n.translate.z };
        x.s = n.scale;
        return x;
    }

    inline RE::NiTransform ToNi(const Xf& x)
    {
        RE::NiTransform n;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                n.rotate.entry[c][r] = static_cast<float>(x.R.m[r][c]);
        n.translate = { static_cast<float>(x.t.x), static_cast<float>(x.t.y), static_cast<float>(x.t.z) };
        n.scale = static_cast<float>(x.s);
        return n;
    }
}
