// granny_anim.hpp -- faithful reader/sampler for the Granny animation blob GW2
// stores in a model's ANIM chunk (PackGrannyAnimationType.animation).
//
// The blob is a serialized 32-bit Granny 2.9 `granny_animation` with self-relative
// 4-byte offsets (0 == null); the fixup table is not needed because every internal
// reference is already a blob-relative byte offset.
//
// This is a direct port of the Granny 2.9 runtime sampling path (RAD Game Tools),
// derived from the SDK source shipped under SDK/Granny/.../source:
//   granny_curve.cpp / granny_curve_fast.cpp  -> per-format CurveExtractKnotValues
//   granny_bspline.cpp / granny_bspline_inlines.h -> SampleBSpline + coefficients
//   granny_bspline_buffers.cpp                -> ConstructBSplineBuffers window logic
//   granny_find_knot.cpp                      -> CurveFindKnot (upper_bound + clamp)
//   granny_math.cpp                           -> EnsureQuaternionContinuity, Normalize4
//   granny_quaternion_scaleoffset.cpp         -> QuaternionCurveScaleOffsetTable
//
// Design: every curve is DEQUANTIZED at parse time into plain float knots+controls
// (== the DaK32fC32f representation). Sampling is then a single uniform B-spline
// evaluator of the curve's degree. This exactly matches Granny's own pipeline,
// which likewise treats every non-constant curve as (knots, controls, degree).
//
// Curve formats (CurveTypeTable order, granny_curve.cpp):
//   0 DaKeyframes32f  1 DaK32fC32f   2 DaIdentity   3 DaConstant32f
//   4 D3Constant32f   5 D4Constant32f
//   6 DaK16uC16u      7 DaK8uC8u     8 D4nK16uC15u  9 D4nK8uC7u
//  10 D3K16uC16u     11 D3K8uC8u
//  12 D9I1K16uC16u   13 D9I3K16uC16u 14 D9I1K8uC8u  15 D9I3K8uC8u
//  16 D3I1K32fC32f   17 D3I1K16uC16u 18 D3I1K8uC8u
#ifndef GRANNY_ANIM_HPP
#define GRANNY_ANIM_HPP

#include <cstdint>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>

namespace granny {

enum Format {
    F_Keyframes32f = 0,
    F_K32fC32f     = 1,
    F_Identity     = 2,
    F_Constant32f  = 3,
    F_D3Constant32f= 4,
    F_D4Constant32f= 5,
    F_DaK16uC16u   = 6,
    F_DaK8uC8u     = 7,
    F_D4nK16uC15u  = 8,
    F_D4nK8uC7u    = 9,
    F_D3K16uC16u   = 10,
    F_D3K8uC8u     = 11,
    F_D9I1K16uC16u = 12,
    F_D9I3K16uC16u = 13,
    F_D9I1K8uC8u   = 14,
    F_D9I3K8uC8u   = 15,
    F_D3I1K32fC32f = 16,
    F_D3I1K16uC16u = 17,
    F_D3I1K8uC8u   = 18,
};

// One transform component's curve, fully dequantized to float knots+controls.
//   identity   : knots.empty() && controls.empty()
//   constant   : knots.size()==1  (controls = dim values)
//   keyframed  : keyframed==true  (DaKeyframes32f; controls = dim*frameCount, frame-indexed)
//   spline     : knots.size()>1   (degree>=0; controls = dim*knots)
struct Curve {
    int   fmt      = F_Identity;
    int   degree   = 0;
    int   dim      = 0;          // output components (3 pos / 4 ori / 9 scaleshear)
    bool  keyframed= false;      // DaKeyframes32f: sample by frame index, not time
    std::vector<float> knots;    // key times
    std::vector<float> controls; // dim per key
};

struct Track {
    std::string name;
    Curve ori;  // orientation quaternion (x,y,z,w)
    Curve pos;  // position (x,y,z)
    Curve sca;  // scaleshear (row-major 3x3)
};

struct Anim {
    bool valid = false;
    std::string name;
    float duration = 0, timeStep = 0, oversampling = 1;
    std::vector<Track> tracks;
};

// QuaternionCurveScaleOffsetTable (granny_quaternion_scaleoffset.cpp): 16 entries
// of {scale, offset}, applied to the D4n dimension-reduced quaternion formats.
namespace detail {
inline const float* quatScaleOffsetTable() {
    static const float k = 0.70710678118654752440f; // 1/sqrt(2)
    static const float T[16 * 2] = {
        k*2.0f,   -k,        k*1.0f,  -k*0.5f,   k*0.5f, -k*0.75f,  k*0.5f, -k*0.25f,
        k*0.5f,    k*0.25f,  k*0.25f, -k*0.25f,  k*0.25f,-k*0.125f, k*0.25f, k*0.0f,
       -k*2.0f,    k,       -k*1.0f,   k*0.5f,  -k*0.5f,  k*0.75f, -k*0.5f,  k*0.25f,
       -k*0.5f,   -k*0.25f, -k*0.25f,  k*0.25f, -k*0.25f, k*0.125f,-k*0.25f,-k*0.0f,
    };
    return T;
}

// ---- blob-relative readers (32-bit offsets) ----
inline uint32_t u32(const uint8_t* b, size_t n, size_t o) {
    return (o + 4 <= n) ? (b[o] | (b[o+1]<<8) | (b[o+2]<<16) | ((uint32_t)b[o+3]<<24)) : 0;
}
inline uint16_t u16(const uint8_t* b, size_t n, size_t o) {
    return (o + 2 <= n) ? (uint16_t)(b[o] | (b[o+1]<<8)) : 0;
}
inline float f32(const uint8_t* b, size_t n, size_t o) {
    uint32_t u = u32(b, n, o); float f; std::memcpy(&f, &u, 4); return f;
}
inline std::string cstr(const uint8_t* b, size_t n, size_t o) {
    if (o == 0 || o >= n) return {};
    std::string s;
    for (size_t i = o; i < n && b[i]; ++i) s.push_back((char)b[i]);
    return s;
}
// OneOverKnotScale reconstructed from the truncated top-16-bits-of-IEEE store.
inline float knotScaleFromTrunc(uint16_t trunc) {
    uint32_t bits = (uint32_t)trunc << 16; float f; std::memcpy(&f, &bits, 4);
    return (f != 0.0f) ? 1.0f / f : 0.0f;
}

// Fill a Curve by dequantizing the curve object at byte offset `obj`.
// `expectDim` is the component count expected by the owning slot (3/4/9); used
// only for the constant/identity fallbacks where the format doesn't imply it.
// `p` is the blob's pointer size (4 for 32-bit packfiles, 8 for 64-bit). The
// serialized curve_data structs are 4-byte packed with p-byte pointers, so any
// field that follows an embedded pointer shifts by (p-4) between the two widths.
// Only DaK*/K32fC32f have a scalar *after* a pointer; every other format's sole
// pointer sits at a p-independent offset, so its layout is unchanged. Pointer
// VALUES are read with u32() for both widths: blob offsets fit in 32 bits and
// the high dword of a 64-bit self-blob-relative offset is always zero (LE).
inline void decodeCurve(const uint8_t* b, size_t n, size_t obj, int expectDim, Curve& c, int p) {
    if (obj == 0 || obj + 2 > n) { c.fmt = F_Identity; c.dim = expectDim; return; }
    c.fmt    = b[obj];
    c.degree = b[obj + 1];
    c.dim    = expectDim;

    auto readKnotsU = [&](size_t knotPtr, int knotCount, int bytes, float knotScale) {
        c.knots.reserve(knotCount);
        for (int i = 0; i < knotCount; ++i) {
            uint32_t raw = (bytes == 2) ? u16(b, n, knotPtr + 2*i) : (knotPtr + i < n ? b[knotPtr + i] : 0);
            c.knots.push_back(knotScale * (float)raw);
        }
    };

    switch (c.fmt) {
    case F_Identity:
        break; // knots/controls empty

    case F_Constant32f: {
        uint32_t cnt = u32(b, n, obj + 4);
        size_t ctrls = u32(b, n, obj + 8);
        c.dim = (int)cnt ? (int)cnt : expectDim;
        c.knots.push_back(0.0f);
        for (uint32_t i = 0; i < cnt; ++i) c.controls.push_back(f32(b, n, ctrls + 4*i));
        break;
    }
    case F_D3Constant32f:
        c.dim = 3; c.knots.push_back(0.0f);
        for (int i = 0; i < 3; ++i) c.controls.push_back(f32(b, n, obj + 4 + 4*i));
        break;
    case F_D4Constant32f:
        c.dim = 4; c.knots.push_back(0.0f);
        for (int i = 0; i < 4; ++i) c.controls.push_back(f32(b, n, obj + 4 + 4*i));
        break;

    case F_K32fC32f: { // DaK32fC32f: float knots + float controls
        // {i16 Pad@2, i32 KC@4, real32* Knots@8, i32 CC@(8+p), real32* Ctrls@(12+p)}
        uint32_t kc = u32(b, n, obj + 4);
        size_t   kp = u32(b, n, obj + 8);
        uint32_t cc = u32(b, n, obj + 8 + p);
        size_t   cp = u32(b, n, obj + 12 + p);
        int dim = kc ? (int)(cc / kc) : expectDim;
        c.dim = dim ? dim : expectDim;
        for (uint32_t i = 0; i < kc; ++i) c.knots.push_back(f32(b, n, kp + 4*i));
        for (uint32_t i = 0; i < cc; ++i) c.controls.push_back(f32(b, n, cp + 4*i));
        break;
    }
    case F_Keyframes32f: { // DaKeyframes32f: {int16 Dimension@2, int32 CC@4, ptr@8}
        int dim = (int16_t)u16(b, n, obj + 2);
        uint32_t cc = u32(b, n, obj + 4);
        size_t   cp = u32(b, n, obj + 8);
        c.dim = dim ? dim : expectDim;
        c.keyframed = true;
        for (uint32_t i = 0; i < cc; ++i) c.controls.push_back(f32(b, n, cp + 4*i));
        break;
    }

    // ---- generic quantized: {u16 trunc@2, i32 SOC@4, ptr SO@8, i32 KCC@12, ptr KC@16}
    case F_DaK16uC16u:
    case F_DaK8uC8u: {
        // {u16 trunc@2, i32 SOC@4, real32* SO@8, i32 KCC@(8+p), K/C* KC@(12+p)}
        int bytes = (c.fmt == F_DaK16uC16u) ? 2 : 1;
        float knotScale = knotScaleFromTrunc(u16(b, n, obj + 2));
        int soc = (int)u32(b, n, obj + 4);
        size_t soPtr = u32(b, n, obj + 8);
        int kcc = (int)u32(b, n, obj + 8 + p);
        size_t kcPtr = u32(b, n, obj + 12 + p);
        int dim = soc / 2; if (dim <= 0) dim = expectDim;
        c.dim = dim;
        int knotCount = kcc / (1 + dim);
        readKnotsU(kcPtr, knotCount, bytes, knotScale);
        size_t ctrlBase = kcPtr + (size_t)knotCount * bytes;
        c.controls.reserve((size_t)knotCount * dim);
        for (int i = 0; i < knotCount; ++i)
            for (int d = 0; d < dim; ++d) {
                float scale  = f32(b, n, soPtr + 4*d);
                float offset = f32(b, n, soPtr + 4*(dim + d));
                uint32_t raw = (bytes == 2) ? u16(b, n, ctrlBase + 2*((size_t)i*dim + d))
                                            : b[ctrlBase + (size_t)i*dim + d];
                c.controls.push_back(offset + scale * (float)raw);
            }
        break;
    }

    // ---- D3 position quantized: {u16 trunc@2, f32 scales[3]@4, f32 offs[3]@16, i32 KCC@28, ptr@32}
    case F_D3K16uC16u:
    case F_D3K8uC8u: {
        int bytes = (c.fmt == F_D3K16uC16u) ? 2 : 1;
        float knotScale = knotScaleFromTrunc(u16(b, n, obj + 2));
        float sc[3] = { f32(b,n,obj+4),  f32(b,n,obj+8),  f32(b,n,obj+12) };
        float of[3] = { f32(b,n,obj+16), f32(b,n,obj+20), f32(b,n,obj+24) };
        int kcc = (int)u32(b, n, obj + 28);
        size_t kcPtr = u32(b, n, obj + 32);
        c.dim = 3;
        int knotCount = kcc / 4; // KC + KC*3
        readKnotsU(kcPtr, knotCount, bytes, knotScale);
        size_t ctrlBase = kcPtr + (size_t)knotCount * bytes;
        c.controls.reserve((size_t)knotCount * 3);
        for (int i = 0; i < knotCount; ++i)
            for (int d = 0; d < 3; ++d) {
                uint32_t raw = (bytes == 2) ? u16(b, n, ctrlBase + 2*((size_t)i*3 + d))
                                            : b[ctrlBase + (size_t)i*3 + d];
                c.controls.push_back(of[d] + sc[d] * (float)raw);
            }
        break;
    }

    // ---- D4n reduced quaternion: {u16 tableEntries@2, f32 OneOverKnotScale@4, i32 KCC@8, ptr@12}
    case F_D4nK16uC15u:
    case F_D4nK8uC7u: {
        bool k16 = (c.fmt == F_D4nK16uC15u);
        int bytes = k16 ? 2 : 1;
        uint16_t tableEntries = u16(b, n, obj + 2);
        float oneOverKnotScale = f32(b, n, obj + 4);
        float knotScale = (oneOverKnotScale != 0.0f) ? 1.0f / oneOverKnotScale : 0.0f;
        int kcc = (int)u32(b, n, obj + 8);
        size_t kcPtr = u32(b, n, obj + 12);
        c.dim = 4;
        int knotCount = kcc >> 2; // KC + KC*3
        readKnotsU(kcPtr, knotCount, bytes, knotScale);

        const float* Q = quatScaleOffsetTable();
        float cs[4], co[4];
        uint16_t te = tableEntries;
        float unit = k16 ? (1.0f / (float)((1 << 15) - 1)) : (1.0f / (float)((1 << 7) - 1));
        for (int d = 0; d < 4; ++d) {
            int ti = te & 0xf; te >>= 4;
            cs[d] = Q[ti * 2 + 0] * unit;
            co[d] = Q[ti * 2 + 1];
        }
        size_t ctrlBase = kcPtr + (size_t)knotCount * bytes;
        c.controls.assign((size_t)knotCount * 4, 0.0f);
        for (int i = 0; i < knotCount; ++i) {
            uint32_t c0, c1, c2;
            if (k16) {
                c0 = u16(b, n, ctrlBase + 2*((size_t)i*3 + 0));
                c1 = u16(b, n, ctrlBase + 2*((size_t)i*3 + 1));
                c2 = u16(b, n, ctrlBase + 2*((size_t)i*3 + 2));
            } else {
                c0 = b[ctrlBase + (size_t)i*3 + 0];
                c1 = b[ctrlBase + (size_t)i*3 + 1];
                c2 = b[ctrlBase + (size_t)i*3 + 2];
            }
            uint32_t signBit = k16 ? 0x8000u : 0x80u;
            int highShift1   = k16 ? 14 : 6;   // for c1 -> bit1 of missing index
            int highShift2   = k16 ? 15 : 7;   // for c2 -> bit0 of missing index
            uint32_t valMask = k16 ? 0x7fffu : 0x7fu;
            bool missingNeg  = (c0 & signBit) != 0;
            int  missingIdx  = (int)(((c1 >> highShift1) & 0x2) | (c2 >> highShift2));
            uint32_t raw[3]  = { c0 & valMask, c1 & valMask, c2 & valMask };

            int dst = missingIdx;
            float summedSq = 0.0f;
            float* out = &c.controls[(size_t)i * 4];
            for (int s = 0; s < 3; ++s) {
                dst = (dst + 1) & 0x3;
                // UnsignedShortToFloat / ByteToFloatTable == identity value cast.
                float v = co[dst] + cs[dst] * (float)raw[s];
                summedSq += v * v;
                out[dst] = v;
            }
            float missing = std::sqrt(std::max(0.0f, 1.0f - summedSq));
            if (missingNeg) missing = -missing;
            out[missingIdx] = missing;
        }
        break;
    }

    // ---- D9I1: 9-dim scaleshear as uniform scale (1 control/knot)
    //      {u16 trunc@2, f32 ControlScale@4, f32 ControlOffset@8, i32 KCC@12, ptr@16}
    case F_D9I1K16uC16u:
    case F_D9I1K8uC8u: {
        int bytes = (c.fmt == F_D9I1K16uC16u) ? 2 : 1;
        float knotScale = knotScaleFromTrunc(u16(b, n, obj + 2));
        float cscale  = f32(b, n, obj + 4);
        float coffset = f32(b, n, obj + 8);
        int kcc = (int)u32(b, n, obj + 12);
        size_t kcPtr = u32(b, n, obj + 16);
        c.dim = 9;
        int knotCount = kcc / 2; // KC + KC
        readKnotsU(kcPtr, knotCount, bytes, knotScale);
        size_t ctrlBase = kcPtr + (size_t)knotCount * bytes;
        c.controls.reserve((size_t)knotCount * 9);
        for (int i = 0; i < knotCount; ++i) {
            uint32_t raw = (bytes == 2) ? u16(b, n, ctrlBase + 2*i) : b[ctrlBase + i];
            float s = coffset + cscale * (float)raw;
            float m[9] = { s,0,0, 0,s,0, 0,0,s };
            for (int d = 0; d < 9; ++d) c.controls.push_back(m[d]);
        }
        break;
    }

    // ---- D9I3: 9-dim scaleshear as non-uniform diagonal scale (3 controls/knot)
    //      {u16 trunc@2, f32 scales[3]@4, f32 offs[3]@16, i32 KCC@28, ptr@32}
    case F_D9I3K16uC16u:
    case F_D9I3K8uC8u: {
        int bytes = (c.fmt == F_D9I3K16uC16u) ? 2 : 1;
        float knotScale = knotScaleFromTrunc(u16(b, n, obj + 2));
        float sc[3] = { f32(b,n,obj+4),  f32(b,n,obj+8),  f32(b,n,obj+12) };
        float of[3] = { f32(b,n,obj+16), f32(b,n,obj+20), f32(b,n,obj+24) };
        int kcc = (int)u32(b, n, obj + 28);
        size_t kcPtr = u32(b, n, obj + 32);
        c.dim = 9;
        int knotCount = kcc / 4; // KC + KC*3
        readKnotsU(kcPtr, knotCount, bytes, knotScale);
        size_t ctrlBase = kcPtr + (size_t)knotCount * bytes;
        c.controls.reserve((size_t)knotCount * 9);
        for (int i = 0; i < knotCount; ++i) {
            float d0, d1, d2;
            if (bytes == 2) {
                d0 = of[0] + sc[0] * (float)u16(b, n, ctrlBase + 2*((size_t)i*3+0));
                d1 = of[1] + sc[1] * (float)u16(b, n, ctrlBase + 2*((size_t)i*3+1));
                d2 = of[2] + sc[2] * (float)u16(b, n, ctrlBase + 2*((size_t)i*3+2));
            } else {
                d0 = of[0] + sc[0] * (float)b[ctrlBase + (size_t)i*3+0];
                d1 = of[1] + sc[1] * (float)b[ctrlBase + (size_t)i*3+1];
                d2 = of[2] + sc[2] * (float)b[ctrlBase + (size_t)i*3+2];
            }
            float m[9] = { d0,0,0, 0,d1,0, 0,0,d2 };
            for (int d = 0; d < 9; ++d) c.controls.push_back(m[d]);
        }
        break;
    }

    // ---- D3I1: 3-dim position on a line (1 control/knot)
    case F_D3I1K32fC32f: {
        // {int16 pad@2, f32 scales[3]@4, f32 offs[3]@16, i32 KCC@28, f32* KC@32}
        float sc[3] = { f32(b,n,obj+4),  f32(b,n,obj+8),  f32(b,n,obj+12) };
        float of[3] = { f32(b,n,obj+16), f32(b,n,obj+20), f32(b,n,obj+24) };
        int kcc = (int)u32(b, n, obj + 28);
        size_t kcPtr = u32(b, n, obj + 32);
        c.dim = 3;
        int knotCount = kcc / 2;
        for (int i = 0; i < knotCount; ++i) c.knots.push_back(f32(b, n, kcPtr + 4*i));
        size_t ctrlBase = kcPtr + (size_t)knotCount * 4;
        for (int i = 0; i < knotCount; ++i) {
            float p = f32(b, n, ctrlBase + 4*i);
            for (int d = 0; d < 3; ++d) c.controls.push_back(of[d] + sc[d] * p);
        }
        break;
    }
    case F_D3I1K16uC16u:
    case F_D3I1K8uC8u: {
        int bytes = (c.fmt == F_D3I1K16uC16u) ? 2 : 1;
        float knotScale = knotScaleFromTrunc(u16(b, n, obj + 2));
        float sc[3] = { f32(b,n,obj+4),  f32(b,n,obj+8),  f32(b,n,obj+12) };
        float of[3] = { f32(b,n,obj+16), f32(b,n,obj+20), f32(b,n,obj+24) };
        int kcc = (int)u32(b, n, obj + 28);
        size_t kcPtr = u32(b, n, obj + 32);
        c.dim = 3;
        int knotCount = kcc / 2; // KC + KC
        readKnotsU(kcPtr, knotCount, bytes, knotScale);
        size_t ctrlBase = kcPtr + (size_t)knotCount * bytes;
        for (int i = 0; i < knotCount; ++i) {
            uint32_t raw = (bytes == 2) ? u16(b, n, ctrlBase + 2*i) : b[ctrlBase + i];
            for (int d = 0; d < 3; ++d) c.controls.push_back(of[d] + sc[d] * (float)raw);
        }
        break;
    }

    default:
        c.fmt = F_Identity; // unknown -> hold identity
        break;
    }
}

// EnsureQuaternionContinuity over a contiguous window of `count` quats (granny_math.cpp).
inline void ensureQuatContinuity(float* q, int count) {
    float last[4] = {0,0,0,0};
    for (int i = 0; i < count; ++i) {
        float* v = q + i*4;
        if (last[0]*v[0] + last[1]*v[1] + last[2]*v[2] + last[3]*v[3] < 0.0f)
            for (int k = 0; k < 4; ++k) v[k] = -v[k];
        for (int k = 0; k < 4; ++k) last[k] = v[k];
    }
}

// B-spline basis coefficients (granny_bspline_inlines.h). `ci` receives d+1 values
// ci[0..d], where ci[e] weights control point at knot offset (e-d). `ti` points at
// the "current" knot so ti[-d .. d-1] are valid.
inline void coefficients(int d, const float* ti, float t, float* ci) {
    switch (d) {
    case 0: ci[0] = 1.0f; break;
    case 1: {
        float c = (t - ti[-1]) / (ti[0] - ti[-1]);
        ci[0] = 1.0f - c; ci[1] = c;
        break;
    }
    case 2: {
        float ti_2=ti[-2], ti_1=ti[-1], ti0=ti[0], ti1=ti[1];
        float L0   = (t - ti_1) / (ti0  - ti_1);
        float L1_1 = (t - ti_2) / (ti0  - ti_2);
        float L1_2 = (t - ti_1) / (ti1  - ti_1);
        float c2 = (L1_1 + L0) - L0*L1_1;
        float c0 = L0*L1_2;
        float c1 = c2 - c0;
        c2 = 1.0f - c2;
        ci[0] = c2; ci[1] = c1; ci[2] = c0;
        break;
    }
    case 3: {
        float ti_3=ti[-3], ti_2=ti[-2], ti_1=ti[-1], ti0=ti[0], ti1=ti[1], ti2=ti[2];
        float L0  =(t-ti_1)/(ti0 -ti_1);
        float L1_1=(t-ti_2)/(ti0 -ti_2);
        float L1_2=(t-ti_1)/(ti1 -ti_1);
        float L2_1=(t-ti_3)/(ti0 -ti_3);
        float L2_2=(t-ti_2)/(ti1 -ti_2);
        float L2_3=(t-ti_1)/(ti2 -ti_1);
        float mL0=1-L0, mL1_1=1-L1_1, mL1_2=1-L1_2, mL2_1=1-L2_1, mL2_2=1-L2_2, mL2_3=1-L2_3;
        float mL0mL1_1=mL0*mL1_1, mL0L1_1=mL0*L1_1, L0mL1_2=L0*mL1_2, L0L1_2=L0*L1_2;
        ci[0]=mL0mL1_1*mL2_1;
        ci[1]=mL0mL1_1*L2_1 + mL0L1_1*mL2_2 + L0mL1_2*mL2_2;
        ci[2]=mL0L1_1*L2_2 + L0mL1_2*L2_2 + L0L1_2*mL2_3;
        ci[3]=L0L1_2*L2_3;
        break;
    }
    default: {
        // Higher order not used by GW2; fall back to holding the current control.
        for (int e = 0; e <= d; ++e) ci[e] = 0.0f;
        ci[d] = 1.0f;
        break;
    }
    }
}
} // namespace detail

// `ptrSize` is the serialized blob's pointer width: 4 for 32-bit packfiles,
// 8 for 64-bit ones (GW2's `pfv & 4` flag; gw2model exposes it as `ptr_`). The
// embedded granny_animation is 4-byte packed with ptrSize-byte pointers, so
// every struct offset past a pointer scales with it. Passing the wrong width
// makes Duration read the Name pointer's high dword (== 0 -> "0.0s") and the
// TransformTracks stride wrong (== no tracks bound) -- the exact 64-bit failure.
inline Anim parse(const uint8_t* b, size_t n, int ptrSize = 4) {
    using namespace detail;
    Anim a;
    const size_t p = (ptrSize == 8) ? 8 : 4;
    if (!b || n < 3*p + 4) return a;
    a.name        = cstr(b, n, u32(b, n, 0));   // char* Name @0
    a.duration    = f32(b, n, p);               // real32 Duration @p
    a.timeStep    = f32(b, n, p + 4);           // real32 TimeStep @p+4
    a.oversampling= f32(b, n, p + 8);           // real32 Oversampling @p+8
    uint32_t tgc  = u32(b, n, p + 12);          // int32 TrackGroupCount @p+12
    size_t tgs    = u32(b, n, p + 16);          // track_group** TrackGroups @p+16
    if (!tgc || !tgs) return a;

    // Collect transform tracks from every track group (GW2 puts the merged
    // skeletal tracks in group 0 "(merged)"; other groups hold vector tracks).
    // track_group: Name@0, VectorTrackCount@p, VectorTracks@p+4,
    //              TransformTrackCount@2p+4, TransformTracks@2p+8.
    // transform_track (stride 7p+4): Name@0, Flags@p, then 3 curve2{Type,Object},
    //   so Ori.Object@2p+4, Pos.Object@4p+4, Sca.Object@6p+4.
    const size_t STRIDE = 7*p + 4;
    for (uint32_t g = 0; g < tgc && g < 64; ++g) {
        size_t tg = u32(b, n, tgs + p*g);       // TrackGroups[g] (p-byte pointer)
        if (!tg) continue;
        uint32_t ttc = u32(b, n, tg + 2*p + 4);
        size_t   tt  = u32(b, n, tg + 2*p + 8);
        if (!tt || !ttc) continue;
        if (ttc > 65535) ttc = 65535;
        a.tracks.reserve(a.tracks.size() + ttc);
        for (uint32_t i = 0; i < ttc; ++i) {
            size_t t = tt + (size_t)i * STRIDE;
            Track tr;
            tr.name = cstr(b, n, u32(b, n, t));
            decodeCurve(b, n, u32(b, n, t + 2*p + 4), 4, tr.ori, (int)p); // OrientationCurve.Object
            decodeCurve(b, n, u32(b, n, t + 4*p + 4), 3, tr.pos, (int)p); // PositionCurve.Object
            decodeCurve(b, n, u32(b, n, t + 6*p + 4), 9, tr.sca, (int)p); // ScaleShearCurve.Object
            a.tracks.push_back(std::move(tr));
        }
    }
    a.valid = true;
    return a;
}

// Sample a curve at time t into out[expectDim]. Caller pre-fills identity defaults
// (pos 0,0,0 / quat 0,0,0,1 / scaleshear identity); F_Identity leaves them untouched.
// Faithful Granny 2.9 evaluation: degree-d B-spline over the dequantized knots, with
// boundary-knot replication (no cross-clip looping) and quaternion continuity+normalize.
inline void sample(const Curve& c, float t, float* out, int expectDim) {
    using namespace detail;
    if (c.fmt == F_Identity || (c.knots.empty() && c.controls.empty())) return;
    int dim = c.dim ? c.dim : expectDim;
    int wdim = dim; // stride of stored controls
    int odim = std::min(dim, expectDim);

    if (c.keyframed) { // DaKeyframes32f: frame-indexed step
        int frames = wdim ? (int)(c.controls.size() / wdim) : 0;
        if (frames <= 0) return;
        int fi = 0; // t already local; frame index chosen by nearest (timeStep handled by caller)
        if (frames > 1) { fi = (int)(t + 0.5f); if (fi < 0) fi = 0; if (fi >= frames) fi = frames - 1; }
        for (int i = 0; i < odim; ++i) out[i] = c.controls[(size_t)fi*wdim + i];
        return;
    }

    if (c.knots.size() <= 1) { // constant
        for (int i = 0; i < odim && i < (int)c.controls.size(); ++i) out[i] = c.controls[i];
        return;
    }

    // spline
    int d = c.degree;
    int KC = (int)c.knots.size();
    // CurveFindKnot: first knot strictly greater than t, clamped to KC-1.
    int ki = (int)(std::upper_bound(c.knots.begin(), c.knots.end(), t) - c.knots.begin());
    if (ki >= KC) ki = KC - 1;
    if (ki < 0) ki = 0;

    if (d <= 0) { // degree 0: piecewise-constant, hold the control at ki
        int idx = ki; if (idx >= KC) idx = KC - 1;
        for (int i = 0; i < odim; ++i) out[i] = c.controls[(size_t)idx*wdim + i];
        return;
    }

    // Build the local knot/control window with boundary replication (Prev/Next NULL).
    const int W = 2 * d;                 // window knot count
    int base = ki - d;
    float ti[8];
    float pi[6 * 9];                     // W<=6, dim<=9
    for (int j = 0; j < W; ++j) {
        int idx = base + j; if (idx < 0) idx = 0; if (idx >= KC) idx = KC - 1;
        ti[j] = c.knots[idx];
        for (int k = 0; k < wdim; ++k) pi[j*wdim + k] = c.controls[(size_t)idx*wdim + k];
    }
    if (wdim == 4) ensureQuatContinuity(pi, W); // quaternion continuity over window

    const float* tip = ti + d;
    const float* pip = pi + d*wdim;
    float ci[8];
    coefficients(d, tip, t, ci);         // ci[0..d], ci[e] weights knot offset (e-d)
    float res[9];
    for (int k = 0; k < wdim; ++k) {
        float acc = 0.0f;
        for (int e = 0; e <= d; ++e) acc += ci[e] * pip[(e - d)*wdim + k];
        res[k] = acc;
    }
    if (wdim == 4) { // normalize quaternion
        float len = std::sqrt(res[0]*res[0]+res[1]*res[1]+res[2]*res[2]+res[3]*res[3]);
        if (len > 1e-12f) { float inv = 1.0f/len; for (int k=0;k<4;++k) res[k]*=inv; }
    }
    for (int i = 0; i < odim; ++i) out[i] = res[i];
}

} // namespace granny
#endif // GRANNY_ANIM_HPP
