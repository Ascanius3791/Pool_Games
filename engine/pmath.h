// pmath.h — freestanding math (no libc), so the identical code compiles
// natively (training) and to WebAssembly (browser game). Deterministic.
#pragma once

namespace pool {

typedef double real;
typedef unsigned long long u64;

const real PI = 3.14159265358979323846;
const real TWO_PI = 6.28318530717958647692;
const real LN2 = 0.69314718055994530942;

inline real psqrt(real x) { return __builtin_sqrt(x); }
inline real pabs(real x) { return __builtin_fabs(x); }
inline real pfloor(real x) { return __builtin_floor(x); }
inline real pmin(real a, real b) { return a < b ? a : b; }
inline real pmax(real a, real b) { return a > b ? a : b; }
inline real pclamp(real x, real lo, real hi) { return x < lo ? lo : (x > hi ? hi : x); }

// sin via range reduction to [-pi/2, pi/2] + Taylor series (error < 1e-15)
inline real psin(real x) {
    x -= TWO_PI * pfloor(x / TWO_PI + 0.5);           // [-pi, pi]
    if (x > PI / 2) x = PI - x;
    else if (x < -PI / 2) x = -PI - x;
    real x2 = x * x, term = x, sum = x;
    for (int k = 1; k <= 11; ++k) {
        term *= -x2 / ((2 * k) * (2 * k + 1));
        sum += term;
    }
    return sum;
}
inline real pcos(real x) { return psin(x + PI / 2); }

inline real plog(real x) {
    if (x <= 0) return -1e300;
    u64 bits;
    __builtin_memcpy(&bits, &x, 8);
    int e = (int)((bits >> 52) & 0x7ff) - 1023;
    bits = (bits & 0x000fffffffffffffULL) | 0x3ff0000000000000ULL;
    real m;
    __builtin_memcpy(&m, &bits, 8);
    if (m > 1.4142135623730951) { m *= 0.5; e += 1; }
    real s = (m - 1) / (m + 1), s2 = s * s, t = s, sum = 0;
    for (int k = 0; k < 12; ++k) { sum += t / (2 * k + 1); t *= s2; }
    return 2 * sum + e * LN2;
}

inline real pexp(real x) {
    if (x < -700) return 0;
    if (x > 700) x = 700;
    int k = (int)pfloor(x / LN2 + 0.5);
    real r = x - k * LN2, term = 1, sum = 1;
    for (int i = 1; i < 20; ++i) { term *= r / i; sum += term; }
    u64 bits = (u64)(k + 1023) << 52;
    real sc;
    __builtin_memcpy(&sc, &bits, 8);
    return sum * sc;
}

inline real patan(real x) {
    bool neg = x < 0; if (neg) x = -x;
    bool inv = x > 1; if (inv) x = 1 / x;
    // two half-angle reductions: atan(x) = 2 atan(x / (1 + sqrt(1+x^2)))
    x = x / (1 + psqrt(1 + x * x));
    x = x / (1 + psqrt(1 + x * x));
    real x2 = x * x, t = x, sum = 0;
    for (int k = 0; k < 14; ++k) { sum += ((k & 1) ? -t : t) / (2 * k + 1); t *= x2; }
    sum *= 4;
    if (inv) sum = PI / 2 - sum;
    return neg ? -sum : sum;
}

inline real patan2(real y, real x) {
    if (x > 0) return patan(y / x);
    if (x < 0) return y >= 0 ? patan(y / x) + PI : patan(y / x) - PI;
    return y > 0 ? PI / 2 : (y < 0 ? -PI / 2 : 0);
}

// xoshiro256** PRNG + Gaussian (Box–Muller)
struct Rng {
    u64 s[4];
    static u64 rotl(u64 x, int k) { return (x << k) | (x >> (64 - k)); }
    void seed(u64 v) {
        for (int i = 0; i < 4; ++i) {   // splitmix64
            v += 0x9e3779b97f4a7c15ULL;
            u64 z = v;
            z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
            z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
            s[i] = z ^ (z >> 31);
        }
    }
    u64 next() {
        u64 r = rotl(s[1] * 5, 7) * 9, t = s[1] << 17;
        s[2] ^= s[0]; s[3] ^= s[1]; s[1] ^= s[2]; s[0] ^= s[3];
        s[2] ^= t; s[3] = rotl(s[3], 45);
        return r;
    }
    real uniform() { return (next() >> 11) * (1.0 / 9007199254740992.0); }   // [0,1)
    real uniform(real a, real b) { return a + (b - a) * uniform(); }
    int below(int n) { return (int)(uniform() * n); }
    real gauss() {
        real u1 = 1.0 - uniform(), u2 = uniform();
        return psqrt(-2 * plog(u1)) * pcos(TWO_PI * u2);
    }
};

} // namespace pool
