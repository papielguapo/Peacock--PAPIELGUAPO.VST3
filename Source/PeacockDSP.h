#pragma once

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include "PeacockDSPData.h"

namespace peacock
{

// ============================================================================
// CRANE SONG PEACOCK — 1:1 REVERSE-ENGINEERED DSP ENGINE (FOR STUDY & EXECUTION)
// ============================================================================
// Source binary: Peacock.aaxplugin (Windows x64)
// Functions decoded 1:1 from x86_64 machine code:
//   - GenerateCoefficients             @ 0x180027450
//   - Peacock_InitializeProcessState   @ 0x180028C00 .. 0x180028FD0
//   - Peacock_InitializeProcessPrivMem @ 0x1800198B0
//   - Peacock_LookupWavetable          @ 0x180019A70
//   - Peacock_AlgorithmProcessFunction @ 0x180017870 (Mono) & 0x180019B80 (Stereo)
//   - Peacock_Table_Data               @ 0x180420DA0 (16,386 IEEE-754 floats)
//
// SIGNAL FLOW & ARCHITECTURAL STUDY GUIDE:
// ----------------------------------------------------------------------------
// 1. WAVETABLE ARCHITECTURE (`Peacock_Table_Data` + `Peacock_LookupWavetable`):
//    - Contains two 8192-point transfer curves stored consecutively (total 16,386 floats):
//      * Table 0 (baseOffset = 0x0000 = 0):    Identity / reference curve [-1.0 .. +1.0]
//      * Table 1 (baseOffset = 0x2000 = 8192): Asymmetric Vinyl / Triode-Pentode Transfer Curve
//    - `Peacock_LookupWavetable(x, baseOffset)` maps `x in [-1.0, +1.0]` to a 32-bit unsigned
//      phase `u = (uint32_t)((x + 1.0f) * 2147483648.0f)` with saturation at 0 and 0xFFFFFFFF:
//      * Integer index: `idx = baseOffset + (u >> 19)` (top 13 bits -> 0..8191)
//      * Fractional part: `frac = (float)(u & 0x7FFFF) / 524288.0f` (low 19 bits)
//      * Linear interpolation: `table[idx] + frac * (table[idx + 1] - table[idx])`
//
// 2. SAMPLE-RATE ADAPTIVE FILTER & ENVELOPE COEFFICIENTS (`srIndex` 1..6):
//    - Supports 6 canonical sample-rate tiers:
//      1: 44.1 kHz | 2: 48 kHz | 3: 88.2 kHz | 4: 96 kHz | 5: 176.4 kHz | 6: 192 kHz
//    - Defines 16 one-pole IIR coefficients (`c_5C`, `c_E0`, `c_84`, `c_24`, `c_FC`, `c_DC`,
//      `c_10C`, `c_D4`, `c_88`, `c_44`, `c_108`, `c_AC`, `c_80`, `c_D8`, `c_40`, `c_20`)
//      plus delay buffer modulo length `delayLen` (`13` @ 1x, `25` @ 2x, `49` @ 4x) and center
//      tap offset `6 * srMult` (`6` @ 1x, `12` @ 2x, `24` @ 4x).
//
// 3. 5 COLOR PROFILES (`color` 1..5 = Silver, Gold, Rich, Fat, Deep):
//    - Each Color preset configures 11 circuit voicing parameters:
//      * `col_38`: Nonlinear waveshaper harmonic injection weight (0.05 to 0.40)
//      * `col_90`: High-pass / coupling pole multiplier (`16x`, `30x`, or `18x` * `c_40`)
//      * `col_54`: Rectified 2nd-harmonic envelope injection weight (0.25 to 0.60)
//      * `col_A0`: Odd-order distortion residual blend into pre-drive stage
//      * `col_AC` & `col_D8`: Time-constant scalars for input high-pass and harmonic envelopes
//      * `col_6C` & `col_9C`: Drive / inverse-drive scaling into the second wavetable stage (`3.0 / 0.333`, `2.0 / 0.5`, `4.0 / 0.25`)
//      * `col_FC`: Output low-pass shelf scalar (`1.1`, `0.95`, `1.0`, `0.8`)
//      * `col_34`: Level-dependent drive curve factor (`0.25` for Silver, `0.0` for others)
//
// 4. PER-SAMPLE PROCESSING LOOP:
//    - Stage A (5-Pole Noise / Flutter & Dither Generator):
//      White noise `[-0.5, +0.5]` is filtered through a 5-stage cascaded one-pole low-pass
//      chain (`s40 -> s44 -> s48 -> s4C -> s50`) to produce ultra-low-frequency vinyl groove
//      flutter (`s50`), combined with first-stage filtered noise (`0.011265 * s40`) to form
//      `modSignal` (`rsp+0xA4`), used both for 16-bit dither (`ditherGain = 0.00948`) and
//      stylus delay-line pitch/timing modulation (`flutterDepth = 0.3` when Dither/16Bit is On).
//    - Stage B (Nonlinear Residual Extraction):
//      Input `x` passes through `LookupWavetable(x, 0x2000)`. The nonlinear residual
//      `diff = LookupWavetable(x, 0x2000) - x` is shaped by cubic/quadratic polynomial
//      `poly = diff^3 + 0.15 * diff + 0.25 * diff^2` and high-pass filtered via `s20` (`hp1`),
//      while `diff^2` is high-pass filtered via `s1C` and scaled by `4.0` (`hp2`).
//    - Stage C (Pre-Emphasis & Second Wavetable Stage):
//      Input `x` is high-pass filtered (`s14`), processed through two cascaded lead/lag shelf
//      stages (`s34` with `0.0999` direct blend, `s30` with `0.04495` direct blend), scaled by
//      `5.6577 * driveNorm * col_6C`, and passed through `LookupWavetable(..., 0x2000)`.
//    - Stage D (Dynamic Envelope Sidechain):
//      Blends the second wavetable output with `hp1`, applies two cascaded shelf filters (`s2C`
//      and `s28`), scales by `6.31`, and extracts the AC dynamic modulation signal via `s18`.
//      The `Dynamic` parameter crossfades between raw input `x` and this dynamic path, adds
//      dither, and writes into the 49-sample circular delay buffer.
//    - Stage E (Quantized Stylus Delay Tap & Harmonic Reconstruction):
//      Reads the center tap (`delayIdx + 6 * srMult`) and computes a 13-level quantized tap
//      offset (`-6 .. +6` samples) driven by `2.0 * harmSq * centerTap + flutterDepth * modSignal`.
//      The modulated tap is waveshaped through `LookupWavetable(..., 0x2000)`, combined with a
//      parabolic bass/groove component (`-1.8 * (tap - s24)^2`) and `hp2`, filtered through
//      output tone stages (`s10`, `s38`, `s3C`), mixed with `centerTap` weighted by `2.5 * harmSq`,
//      and scaled by `outputGain`.
// ============================================================================

struct SPeacock_Context
{
    float dynamic;     // [0x00] Dynamic / 100.0f (0.0 .. 1.0)
    float harmonic;    // [0x04] Harmonic / 100.0f (0.0 .. 1.0)
    float outputGain;  // [0x08] 10^(OutputTrim_dB / 20.0f)
    int32_t dither;    // [0x0C] 1 = Off, 2 = On (16Bit)
    int32_t color;     // [0x10] 1 = Silver, 2 = Gold, 3 = Rich, 4 = Fat, 5 = Deep
};

struct SPeacock_State
{
    float f00;            // [0x00]
    float srRatio;        // [0x04] 96000.0f / sampleRate
    int32_t srMult;       // [0x08] (int)(sampleRate / 44100.0f) -> 1, 2, or 4
    int32_t srIndex;      // [0x0C] 1=44.1k, 2=48k, 3=88.2k, 4=96k, 5=176.4k, 6=192k
    float s10;            // [0x10] Output high-pass state
    float s14;            // [0x14] Input DC / subsonic high-pass state
    float s18;            // [0x18] Dynamic path high-pass state
    float s1C;            // [0x1C] Even-order (diff^2) envelope state
    float s20;            // [0x20] Odd/even polynomial (diff^3 + 0.25*diff^2 + 0.15*diff) state
    float s24;            // [0x24] Delayed tap high-pass state (for -1.8 * hp^2)
    float s28;            // [0x28] Dynamic sidechain shelf state 2
    float s2C;            // [0x2C] Dynamic sidechain shelf state 1
    float s30;            // [0x30] Pre-emphasis shelf state 2
    float s34;            // [0x34] Pre-emphasis shelf state 1
    float s38;            // [0x38] Harmonic output filter state 1
    float s3C;            // [0x3C] Harmonic output filter state 2
    float s40;            // [0x40] Noise filter pole 1
    float s44;            // [0x44] Noise filter pole 2
    float s48;            // [0x48] Noise filter pole 3
    float s4C;            // [0x4C] Noise filter pole 4
    float s50;            // [0x50] Noise filter pole 5
    float delayBuf[49];   // [0x54..0x114] Circular delay line (max 49 samples @ 192 kHz)
    int32_t delayIdx;     // [0x118] Circular write index
};

static_assert(sizeof(SPeacock_State) == 0x11C, "SPeacock_State must match exact 0x11C byte layout of Peacock.aaxplugin");

// Exact 1:1 Wavetable Lookup (`0x180019A70`)
inline float Peacock_LookupWavetable(float x, int32_t baseOffset) noexcept
{
    // cvttss2si rax, (x + 1.0f) * 2.14748365e9f
    const float scaled = (x + 1.0f) * 2147483648.0f;
    uint32_t u = static_cast<uint32_t>( static_cast<int64_t>(scaled) );
    if (x < -1.0f)
        u = 0u;
    if (x > 1.0f)
        u = 0xFFFFFFFFu;

    const int32_t idx0 = baseOffset + static_cast<int32_t>(u >> 19);
    const int32_t idx1 = idx0 + 1;
    const float y0 = PeacockDSPData::Peacock_Table_Data[idx0];
    const float y1 = PeacockDSPData::Peacock_Table_Data[idx1];
    const float frac = static_cast<float>(u & 0x7FFFFu) * (1.0f / 524288.0f);
    return y0 + frac * (y1 - y0);
}

// Exact 1:1 State Initialization (`0x1800198B0` + `0x180028C00..0x180028FD0`)
inline void Peacock_InitializeState(SPeacock_State& st, float sampleRate) noexcept
{
    std::memset(&st, 0, sizeof(SPeacock_State));

    const float sr = (sampleRate > 8000.0f) ? sampleRate : 48000.0f;
    st.f00 = 0.0f;
    st.srRatio = 96000.0f / sr;

    int32_t mult = static_cast<int32_t>(sr / 44100.0f);
    if (mult >= 4)
        mult = 4;
    else if (mult >= 2)
        mult = 2;
    else
        mult = 1;
    st.srMult = mult;

    const int32_t srInt = static_cast<int32_t>(sr + 0.5f);
    if (srInt <= 44100)
        st.srIndex = 1;
    else if (srInt <= 48000)
        st.srIndex = 2;
    else if (srInt <= 88200)
        st.srIndex = 3;
    else if (srInt <= 96000)
        st.srIndex = 4;
    else if (srInt <= 176400)
        st.srIndex = 5;
    else
        st.srIndex = 6;
}

// Exact 1:1 Coefficient Generator (`0x180027450`)
inline SPeacock_Context Peacock_GenerateCoefficients(
    float harmonicPct,
    float dynamicPct,
    float outputTrimDb,
    int ditherChoice, // 0 = Off, 1 = On (16Bit)
    int colorChoice,  // 0 = Silver, 1 = Gold, 2 = Rich, 3 = Fat, 4 = Deep
    bool bypassed = false) noexcept
{
    SPeacock_Context ctx{};
    if (bypassed)
    {
        ctx.dynamic    = 0.0f;
        ctx.harmonic   = 0.0f;
        ctx.outputGain = 1.0f;
        ctx.dither     = 1;
        ctx.color      = 1;
        return ctx;
    }

    ctx.dynamic    = std::clamp(dynamicPct, 0.0f, 100.0f) * 0.01f;
    ctx.harmonic   = std::clamp(harmonicPct, 0.0f, 100.0f) * 0.01f;
    ctx.outputGain = std::pow(10.0f, std::clamp(outputTrimDb, -6.0f, 6.0f) * 0.05f);
    ctx.dither     = (ditherChoice != 0) ? 2 : 1;
    ctx.color      = std::clamp(colorChoice + 1, 1, 5);
    return ctx;
}

// Exact 1:1 Algorithm Process Function (`0x180017870`)
// Note: We pass a deterministic per-instance PRNG state matching MSVC `rand()` (0..32767)
// so each channel or plugin instance behaves identically to the original CRT `rand()` call at `0x1800188DB`.
inline int32_t Peacock_ReadQuantizedTapOffset(double modVal) noexcept
{
    if (modVal > 0.5)      return  6;
    if (modVal > 0.4165)   return  5;
    if (modVal > 0.3325)   return  4;
    if (modVal > 0.2465)   return  3;
    if (modVal > 0.11655)  return  2;
    if (modVal > 0.0415)   return  1;
    if (modVal > 0.0)      return  0;
    if (modVal > -0.0415)  return -1;
    if (modVal > -0.11655) return -2;
    if (modVal > -0.2465)  return -3;
    if (modVal > -0.3325)  return -4;
    if (modVal > -0.4165)  return -5;
    return -6;
}

inline void Peacock_ProcessChannel(
    const SPeacock_Context& ctx,
    SPeacock_State& st,
    const float* inBuf,
    float* outBuf,
    int32_t numSamples,
    uint32_t& rngState) noexcept
{
    // Load context parameters
    const float dyn      = ctx.dynamic;               // [rsp+0x1A0]
    const float harmSq   = ctx.harmonic * ctx.harmonic; // [rsp+0x148]
    const float outGain  = ctx.outputGain;            // [rsp+0x144]
    const int32_t color  = ctx.color;                 // [rsp+0x13C]
    const int32_t dither = ctx.dither;                // [rsp+0x138]

    const int32_t srMult  = st.srMult;                // [rsp+0x188]
    const int32_t srIndex = st.srIndex;               // [rsp+0x190]

    // Sample-rate dependent filter coefficients (`0x180017CB4 .. 0x1800182D3`)
    float c_5C  = 0.888499975f;
    float c_E0  = 0.0749299973f;
    float c_84  = 0.226940006f;
    float c_24  = 0.00590000022f;
    float c_FC  = 0.269400001f;
    float c_DC  = 0.00590000022f;
    float c_10C = 0.421799988f;
    float c_D4  = 0.00400000019f;
    float c_88  = 0.00800000038f;
    float c_44  = 0.0120000001f;
    float c_108 = 0.0160000008f;
    float c_AC  = 0.00150000001f;
    float c_80  = 0.00150000001f;
    float c_D8  = 0.119999997f;
    float c_40  = 0.00120000006f;
    float c_20  = 0.00120000006f;

    switch (srIndex)
    {
        case 1: // 44.1 kHz
            c_5C  = 0.888499975f;
            c_E0  = 0.0749299973f;
            c_84  = 0.226940006f;
            c_24  = 0.00590000022f;
            c_FC  = 0.269400001f;
            c_DC  = 0.00590000022f;
            c_10C = 0.421799988f;
            c_D4  = 0.00400000019f;
            c_88  = 0.00800000038f;
            c_44  = 0.0120000001f;
            c_108 = 0.0160000008f;
            c_AC  = 0.00150000001f;
            c_80  = 0.00150000001f;
            c_D8  = 0.119999997f;
            c_40  = 0.00120000006f;
            c_20  = 0.00120000006f;
            break;

        case 2: // 48 kHz
            c_5C  = 0.889999986f;
            c_E0  = 0.074409999f;
            c_84  = 0.226940006f;
            c_24  = 0.00590000022f;
            c_FC  = 0.269400001f;
            c_DC  = 0.00590000022f;
            c_10C = 0.421799988f;
            c_D4  = 0.00400000019f;
            c_88  = 0.00800000038f;
            c_44  = 0.0120000001f;
            c_108 = 0.0160000008f;
            c_AC  = 0.00150000001f;
            c_80  = 0.00150000001f;
            c_D8  = 0.119999997f;
            c_40  = 0.00120000006f;
            c_20  = 0.00120000006f;
            break;

        case 3: // 88.2 kHz
            c_5C  = 0.783800006f;
            c_E0  = 0.0387299992f;
            c_84  = 0.113470003f;
            c_24  = 0.00295000011f;
            c_FC  = 0.1347f;
            c_DC  = 0.00295000011f;
            c_10C = 0.231999993f;
            c_D4  = 0.00200000009f;
            c_88  = 0.00400000019f;
            c_44  = 0.00600000005f;
            c_108 = 0.00800000038f;
            c_AC  = 0.00100000005f;
            c_80  = 0.000750000007f;
            c_D8  = 0.0599999987f;
            c_40  = 0.000600000028f;
            c_20  = 0.000600000028f;
            break;

        case 4: // 96 kHz
            c_5C  = 0.77609998f;
            c_E0  = 0.0401000008f;
            c_84  = 0.113470003f;
            c_24  = 0.00295000011f;
            c_FC  = 0.1347f;
            c_DC  = 0.00295000011f;
            c_10C = 0.231999993f;
            c_D4  = 0.00200000009f;
            c_88  = 0.00400000019f;
            c_44  = 0.00600000005f;
            c_108 = 0.00800000038f;
            c_AC  = 0.00100000005f;
            c_80  = 0.000750000007f;
            c_D8  = 0.0599999987f;
            c_40  = 0.000600000028f;
            c_20  = 0.000600000028f;
            break;

        case 5: // 176.4 kHz
            c_5C  = 0.613799989f;
            c_E0  = 0.0190399997f;
            c_84  = 0.0567350015f;
            c_24  = 0.00147500006f;
            c_FC  = 0.0673500001f;
            c_DC  = 0.00147500006f;
            c_10C = 0.149499997f;
            c_D4  = 0.00100000005f;
            c_88  = 0.00200000009f;
            c_44  = 0.00300000003f;
            c_108 = 0.00400000019f;
            c_AC  = 0.000530000019f;
            c_80  = 0.000375000003f;
            c_D8  = 0.0299999993f;
            c_40  = 0.000300000014f;
            c_20  = 0.000300000014f;
            break;

        case 6: // 192 kHz
        default:
            c_5C  = 0.61589998f;
            c_E0  = 0.0197500009f;
            c_84  = 0.0567350015f;
            c_24  = 0.00147500006f;
            c_FC  = 0.0673500001f;
            c_DC  = 0.00147500006f;
            c_10C = 0.149499997f;
            c_D4  = 0.00100000005f;
            c_88  = 0.00200000009f;
            c_44  = 0.00300000003f;
            c_108 = 0.00400000019f;
            c_AC  = 0.000375000003f;
            c_80  = 0.000375000003f;
            c_D8  = 0.0299999993f;
            c_40  = 0.000300000014f;
            c_20  = 0.000300000014f;
            break;
    }

    // Dither & flutter depth (`0x1800182D3 .. 0x18001830D`)
    const float ditherGain   = static_cast<float>(static_cast<double>(dither - 1) * 0.00948);
    const float flutterDepth = static_cast<float>(static_cast<double>(dither - 1) * 0.3);

    // Color voicing switch (`0x180018313 .. 0x180018777`)
    float col_38 = 0.0f;
    float col_90 = c_40 * 16.0f;
    float col_54 = 0.5f;
    float col_A0 = 0.0500000007f;
    float col_6C = 3.0f;
    float col_9C = 0.333000004f;
    float col_34 = 0.0f;

    switch (color)
    {
        case 1: // Silver
            col_38 = 0.0500000007f;
            col_90 = c_40 * 16.0f;
            col_54 = 0.5f;
            col_A0 = 0.0500000007f;
            c_AC   = c_AC * 2.0f;
            c_D8   = c_D8 * 3.0f;
            col_6C = 3.0f;
            col_9C = 0.333000004f;
            c_FC   = static_cast<float>(static_cast<double>(c_FC) * 1.1);
            col_34 = 0.25f;
            break;

        case 2: // Gold
            col_38 = 0.0500000007f;
            col_90 = c_40 * 30.0f;
            col_54 = 0.25f;
            col_A0 = 0.150000006f;
            c_AC   = c_AC;
            c_D8   = static_cast<float>(static_cast<double>(c_D8) * 0.75);
            col_6C = 3.0f;
            col_9C = 0.333000004f;
            c_FC   = static_cast<float>(static_cast<double>(c_FC) * 0.95);
            col_34 = 0.0f;
            break;

        case 3: // Rich
            col_38 = 0.100000001f;
            col_90 = c_40 * 16.0f;
            col_54 = 0.400000006f;
            col_A0 = 0.400000006f;
            c_AC   = static_cast<float>(static_cast<double>(c_AC) * 0.125);
            c_D8   = static_cast<float>(static_cast<double>(c_D8) * 0.25);
            col_6C = 2.0f;
            col_9C = 0.5f;
            c_FC   = c_FC * 1.0f;
            col_34 = 0.0f;
            break;

        case 4: // Fat
            col_38 = 0.200000003f;
            col_90 = c_40 * 16.0f;
            col_54 = 0.600000024f;
            col_A0 = 0.5f;
            c_AC   = static_cast<float>(static_cast<double>(c_AC) * 0.5);
            c_D8   = static_cast<float>(static_cast<double>(c_D8) * 0.25);
            col_6C = 3.0f;
            col_9C = 0.333299994f;
            c_FC   = c_FC * 1.0f;
            col_34 = 0.0f;
            break;

        case 5: // Deep
            col_38 = 0.400000006f;
            col_90 = c_40 * 18.0f;
            col_54 = 0.600000024f;
            col_A0 = static_cast<float>(0.35 + static_cast<double>(1.0f - harmSq) * 0.45);
            c_AC   = static_cast<float>(static_cast<double>(c_AC) * 0.01);
            c_D8   = static_cast<float>(static_cast<double>(c_D8) * 0.125);
            col_6C = 4.0f;
            col_9C = 0.25f;
            c_FC   = static_cast<float>(static_cast<double>(c_FC) * 0.8);
            col_34 = 0.0f;
            break;

        default:
            col_38 = 0.0f;
            break;
    }

    // Drive normalization (`0x180018777 .. 0x1800187DF`)
    const float driveNorm = static_cast<float>(
        static_cast<double>(harmSq * harmSq) * (1.5 + static_cast<double>(col_34))
        + (0.5 - static_cast<double>(col_34)));
    const float invDriveNorm = 1.0f / driveNorm;

    // Circular delay line length (`0x1800187E8 .. 0x180018848`)
    int32_t delayLen = 13;
    if (srMult == 1)
        delayLen = 13;
    else if (srMult == 2)
        delayLen = 25;
    else if (srMult == 4)
        delayLen = 49;

    // Unpack state variables into registers for the sample loop
    float s10 = st.s10;
    float s14 = st.s14;
    float s18 = st.s18;
    float s1C = st.s1C;
    float s20 = st.s20;
    float s24 = st.s24;
    float s28 = st.s28;
    float s2C = st.s2C;
    float s30 = st.s30;
    float s34 = st.s34;
    float s38 = st.s38;
    float s3C = st.s3C;
    float s40 = st.s40;
    float s44 = st.s44;
    float s48 = st.s48;
    float s4C = st.s4C;
    float s50 = st.s50;
    int32_t delayIdx = st.delayIdx;

    for (int32_t i = 0; i < numSamples; ++i)
    {
        const float x = inBuf[i];

        // 1. MSVC CRT `rand()` (15-bit PRNG) -> white noise in [-0.5, +0.5]
        rngState = rngState * 214013u + 2531011u;
        const int32_t r15 = static_cast<int32_t>((rngState >> 16) & 0x7FFFu);
        const float white = static_cast<float>(static_cast<double>(static_cast<float>(r15) / 32767.0f) - 0.5);

        // 5-pole cascaded low-pass filter on white noise
        s40 = s40 + (white - s40) * c_10C;
        s44 = s44 + (s40   - s44) * c_D4;
        s48 = s48 + (s44   - s48) * c_88;
        s4C = s4C + (s48   - s4C) * c_44;
        s50 = s50 + (s4C   - s50) * c_108;

        const float modSignal = static_cast<float>(
            static_cast<double>(s50) + static_cast<double>(s40) * 0.00751 * 1.5);

        // 2. First Wavetable Lookup & Polynomial Harmonic Extraction
        const float wt1  = Peacock_LookupWavetable(x, 0x2000);
        const float diff = wt1 - x;

        const float poly = static_cast<float>(
            static_cast<double>(diff * diff * diff)
            + static_cast<double>(diff) * 0.15
            + static_cast<double>(diff * diff) * 0.25);

        s20 = s20 + (poly - s20) * c_D8;
        const float hp1 = (poly - s20) * 1.0f;

        const float diffSq = diff * diff;
        s1C = s1C + (diffSq - s1C) * c_D8;
        const float hp2 = (diffSq - s1C) * 4.0f;

        // 3. Input High-Pass & Pre-Emphasis Shelving into Second Wavetable
        s14 = s14 + (x - s14) * c_AC;
        const float x_hp = x - s14;

        s34 = s34 + (x_hp - s34) * c_E0;
        const float hp_s34 = x_hp - s34;
        const float x_emp1 = static_cast<float>(
            static_cast<double>(x_hp) * 0.0999 + static_cast<double>(hp_s34));

        s30 = s30 + (x_emp1 - s30) * c_5C;
        const float hp_s30 = x_emp1 - s30;
        const float x_emp2 = static_cast<float>(
            static_cast<double>(x_emp1) * 0.04495 + static_cast<double>(hp_s30));

        const float wt2_in = static_cast<float>(static_cast<double>(x_emp2) * 5.6577) * driveNorm * col_6C;
        const float wt2    = Peacock_LookupWavetable(wt2_in, 0x2000);

        // 4. Dynamic Sidechain Path
        const float dyn_mix1 = wt2 * invDriveNorm * col_9C + hp1 * col_A0;
        s2C = s2C + (dyn_mix1 - s2C) * c_24;
        const float dyn_mix2 = static_cast<float>(
            static_cast<double>(s2C) + static_cast<double>(dyn_mix1) * 0.09);
        const float dyn_scaled = static_cast<float>(
            static_cast<double>(dyn_mix2) * 2.4 * 1.24 * 0.89);

        s28 = s28 + (dyn_scaled - s28) * c_84;
        const float dyn_mix3 = static_cast<float>(
            static_cast<double>(dyn_scaled) * 0.071 + static_cast<double>(s28));
        const float dyn_pre = static_cast<float>(
            static_cast<double>(dyn_mix3 * 5.0f * 2.0f) * 0.631);

        s18 = s18 + (dyn_pre - s18) * c_80;
        const float dyn_hp = dyn_pre - s18;

        // 5. Dynamic Blend + Dither into Delay Buffer
        const float delayedIn = x * (1.0f - dyn) + dyn_hp * dyn + modSignal * ditherGain;
        st.delayBuf[delayIdx] = delayedIn;

        // 6. Center Tap & Quantized Stylus Modulation Tap
        const int32_t centerIdx = (delayIdx + srMult * 6) % delayLen;
        const float centerTap   = st.delayBuf[centerIdx];

        const float tapMod = centerTap * harmSq * 2.0f + modSignal * flutterDepth;
        const int32_t tapOffset = Peacock_ReadQuantizedTapOffset(static_cast<double>(tapMod));

        // Note: In x86 `idiv`, `(delayIdx + srMult * 6 + tapOffset)` is always >= 0 because
        // `srMult * 6 >= 6` and `tapOffset >= -6`.
        const int32_t modIdx = (delayIdx + srMult * 6 + tapOffset) % delayLen;
        const float modTap   = st.delayBuf[modIdx];

        delayIdx = (delayIdx + 1) % delayLen;

        // 7. Third Wavetable & Harmonic Reconstruction Filter Chain
        const float wt3 = Peacock_LookupWavetable(modTap, 0x2000);

        s24 = s24 + (modTap - s24) * c_20;
        const float modTap_hp = modTap - s24;
        const float sqTerm = static_cast<float>(
            static_cast<double>(modTap_hp * modTap_hp) * -1.8);

        const float harm1 = sqTerm + wt3 * col_38 * harmSq;

        s10 = s10 + (harm1 - s10) * col_90;
        const float harm2 = (harm1 - s10) + hp2 * col_54;

        s38 = s38 + (harm2 - s38) * c_DC;
        const float harm3 = static_cast<float>(
            static_cast<double>(harm2) * 0.091 + static_cast<double>(s38));

        s3C = s3C + (harm3 - s3C) * c_FC;
        const float harm4 = static_cast<float>(
            static_cast<double>(s3C) + static_cast<double>(harm3) * 0.09);

        // 8. Final Output Mix & Output Trim
        const float outSample = static_cast<float>(
            static_cast<double>(harm4) * 2.5 * static_cast<double>(harmSq)
            + static_cast<double>(centerTap)) * outGain;

        outBuf[i] = outSample;
    }

    // Store state back
    st.s10 = s10;
    st.s14 = s14;
    st.s18 = s18;
    st.s1C = s1C;
    st.s20 = s20;
    st.s24 = s24;
    st.s28 = s28;
    st.s2C = s2C;
    st.s30 = s30;
    st.s34 = s34;
    st.s38 = s38;
    st.s3C = s3C;
    st.s40 = s40;
    st.s44 = s44;
    st.s48 = s48;
    st.s4C = s4C;
    st.s50 = s50;
    st.delayIdx = delayIdx;
}

} // namespace peacock
