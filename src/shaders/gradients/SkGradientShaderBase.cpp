/*
 * Copyright 2022 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "src/shaders/gradients/SkGradientShaderBase.h"

#include "include/core/SkColorSpace.h"
#include "include/private/SkVx.h"
#include "src/core/SkConvertPixels.h"
#include "src/core/SkMatrixProvider.h"
#include "src/core/SkRasterPipeline.h"
#include "src/core/SkReadBuffer.h"
#include "src/core/SkVM.h"
#include "src/core/SkWriteBuffer.h"

enum GradientSerializationFlags {
    // Bits 29:31 used for various boolean flags
    kHasPosition_GSF          = 0x80000000,
    kHasLegacyLocalMatrix_GSF = 0x40000000,
    kHasColorSpace_GSF        = 0x20000000,

    // Bits 12:28 unused

    // Bits 8:11 for fTileMode
    kTileModeShift_GSF  = 8,
    kTileModeMask_GSF   = 0xF,

    // Bits 0:7 for fGradFlags
    kGradFlagsShift_GSF = 0,
    kGradFlagsMask_GSF  = 0xFF,
};

static uint32_t interpolation_to_grad_flags(const SkGradientShader::Interpolation& interpolation) {
    uint32_t flags = 0;
    if (interpolation.fInPremul == SkGradientShader::Interpolation::InPremul::kYes) {
        flags |= SkGradientShader::kInterpolateColorsInPremul_Flag;
    }
    return flags;
}

static SkGradientShader::Interpolation grad_flags_to_interpolation(uint32_t flags) {
    SkGradientShader::Interpolation interpolation;
    if (flags & SkGradientShader::kInterpolateColorsInPremul_Flag) {
        interpolation.fInPremul = SkGradientShader::Interpolation::InPremul::kYes;
    }
    return interpolation;
}

SkGradientShaderBase::Descriptor::Descriptor() {
    sk_bzero(this, sizeof(*this));
    fTileMode = SkTileMode::kClamp;
}
SkGradientShaderBase::Descriptor::~Descriptor() = default;

void SkGradientShaderBase::Descriptor::flatten(SkWriteBuffer& buffer) const {
    uint32_t flags = 0;
    if (fPos) {
        flags |= kHasPosition_GSF;
    }
    sk_sp<SkData> colorSpaceData = fColorSpace ? fColorSpace->serialize() : nullptr;
    if (colorSpaceData) {
        flags |= kHasColorSpace_GSF;
    }
    SkASSERT(static_cast<uint32_t>(fTileMode) <= kTileModeMask_GSF);
    flags |= ((unsigned)fTileMode << kTileModeShift_GSF);
    const uint32_t gradFlags = interpolation_to_grad_flags(fInterpolation);
    SkASSERT(gradFlags <= kGradFlagsMask_GSF);
    flags |= (gradFlags << kGradFlagsShift_GSF);

    buffer.writeUInt(flags);

    buffer.writeColor4fArray(fColors, fCount);
    if (colorSpaceData) {
        buffer.writeDataAsByteArray(colorSpaceData.get());
    }
    if (fPos) {
        buffer.writeScalarArray(fPos, fCount);
    }
}

template <int N, typename T, bool MEM_MOVE>
static bool validate_array(SkReadBuffer& buffer, size_t count, SkSTArray<N, T, MEM_MOVE>* array) {
    if (!buffer.validateCanReadN<T>(count)) {
        return false;
    }

    array->resize_back(count);
    return true;
}

bool SkGradientShaderBase::DescriptorScope::unflatten(SkReadBuffer& buffer,
                                                      SkMatrix* legacyLocalMatrix) {
    // New gradient format. Includes floating point color, color space, densely packed flags
    uint32_t flags = buffer.readUInt();

    fTileMode = (SkTileMode)((flags >> kTileModeShift_GSF) & kTileModeMask_GSF);
    uint32_t gradFlags = (flags >> kGradFlagsShift_GSF) & kGradFlagsMask_GSF;
    fInterpolation = grad_flags_to_interpolation(gradFlags);

    fCount = buffer.getArrayCount();

    if (!(validate_array(buffer, fCount, &fColorStorage) &&
          buffer.readColor4fArray(fColorStorage.begin(), fCount))) {
        return false;
    }
    fColors = fColorStorage.begin();

    if (SkToBool(flags & kHasColorSpace_GSF)) {
        sk_sp<SkData> data = buffer.readByteArrayAsData();
        fColorSpace = data ? SkColorSpace::Deserialize(data->data(), data->size()) : nullptr;
    } else {
        fColorSpace = nullptr;
    }
    if (SkToBool(flags & kHasPosition_GSF)) {
        if (!(validate_array(buffer, fCount, &fPosStorage) &&
              buffer.readScalarArray(fPosStorage.begin(), fCount))) {
            return false;
        }
        fPos = fPosStorage.begin();
    } else {
        fPos = nullptr;
    }
    if (SkToBool(flags & kHasLegacyLocalMatrix_GSF)) {
        SkASSERT(buffer.isVersionLT(SkPicturePriv::Version::kNoShaderLocalMatrix));
        buffer.readMatrix(legacyLocalMatrix);
    } else {
        *legacyLocalMatrix = SkMatrix::I();
    }
    return buffer.isValid();
}

////////////////////////////////////////////////////////////////////////////////////////////

SkGradientShaderBase::SkGradientShaderBase(const Descriptor& desc, const SkMatrix& ptsToUnit)
        : fPtsToUnit(ptsToUnit)
        , fColorSpace(desc.fColorSpace ? desc.fColorSpace : SkColorSpace::MakeSRGB())
        , fColorsAreOpaque(true) {
    fPtsToUnit.getType();  // Precache so reads are threadsafe.
    SkASSERT(desc.fCount > 1);

    fInterpolation = desc.fInterpolation;

    SkASSERT((unsigned)desc.fTileMode < kSkTileModeCount);
    fTileMode = desc.fTileMode;

    /*  Note: we let the caller skip the first and/or last position.
        i.e. pos[0] = 0.3, pos[1] = 0.7
        In these cases, we insert entries to ensure that the final data
        will be bracketed by [0, 1].
        i.e. our_pos[0] = 0, our_pos[1] = 0.3, our_pos[2] = 0.7, our_pos[3] = 1

        Thus colorCount (the caller's value, and fColorCount (our value) may
        differ by up to 2. In the above example:
            colorCount = 2
            fColorCount = 4
     */
    fColorCount = desc.fCount;
    // check if we need to add in start and/or end position/colors
    bool needsFirst = false;
    bool needsLast = false;
    if (desc.fPos) {
        needsFirst = desc.fPos[0] != 0;
        needsLast = desc.fPos[desc.fCount - 1] != SK_Scalar1;
        fColorCount += needsFirst + needsLast;
    }

    size_t storageSize = fColorCount * (sizeof(SkColor4f) + (desc.fPos ? sizeof(SkScalar) : 0));
    fOrigColors4f      = reinterpret_cast<SkColor4f*>(fStorage.reset(storageSize));
    fOrigPos           = desc.fPos ? reinterpret_cast<SkScalar*>(fOrigColors4f + fColorCount)
                                   : nullptr;

    // Now copy over the colors, adding the dummies as needed
    SkColor4f* origColors = fOrigColors4f;
    if (needsFirst) {
        *origColors++ = desc.fColors[0];
    }
    for (int i = 0; i < desc.fCount; ++i) {
        origColors[i] = desc.fColors[i];
        fColorsAreOpaque = fColorsAreOpaque && (desc.fColors[i].fA == 1);
    }
    if (needsLast) {
        origColors += desc.fCount;
        *origColors = desc.fColors[desc.fCount - 1];
    }

    if (desc.fPos) {
        SkScalar prev = 0;
        SkScalar* origPosPtr = fOrigPos;
        *origPosPtr++ = prev; // force the first pos to 0

        int startIndex = needsFirst ? 0 : 1;
        int count = desc.fCount + needsLast;

        bool uniformStops = true;
        const SkScalar uniformStep = desc.fPos[startIndex] - prev;
        for (int i = startIndex; i < count; i++) {
            // Pin the last value to 1.0, and make sure pos is monotonic.
            auto curr = (i == desc.fCount) ? 1 : SkTPin(desc.fPos[i], prev, 1.0f);
            uniformStops &= SkScalarNearlyEqual(uniformStep, curr - prev);

            *origPosPtr++ = prev = curr;
        }

        // If the stops are uniform, treat them as implicit.
        if (uniformStops) {
            fOrigPos = nullptr;
        }
    }
}

SkGradientShaderBase::~SkGradientShaderBase() {}

void SkGradientShaderBase::flatten(SkWriteBuffer& buffer) const {
    Descriptor desc;
    desc.fColors = fOrigColors4f;
    desc.fColorSpace = fColorSpace;
    desc.fPos = fOrigPos;
    desc.fCount = fColorCount;
    desc.fTileMode = fTileMode;
    desc.fInterpolation = fInterpolation;

    desc.flatten(buffer);
}

static void add_stop_color(SkRasterPipeline_GradientCtx* ctx, size_t stop,
                           SkPMColor4f Fs, SkPMColor4f Bs) {
    (ctx->fs[0])[stop] = Fs.fR;
    (ctx->fs[1])[stop] = Fs.fG;
    (ctx->fs[2])[stop] = Fs.fB;
    (ctx->fs[3])[stop] = Fs.fA;

    (ctx->bs[0])[stop] = Bs.fR;
    (ctx->bs[1])[stop] = Bs.fG;
    (ctx->bs[2])[stop] = Bs.fB;
    (ctx->bs[3])[stop] = Bs.fA;
}

static void add_const_color(SkRasterPipeline_GradientCtx* ctx, size_t stop, SkPMColor4f color) {
    add_stop_color(ctx, stop, { 0, 0, 0, 0 }, color);
}

// Calculate a factor F and a bias B so that color = F*t + B when t is in range of
// the stop. Assume that the distance between stops is 1/gapCount.
static void init_stop_evenly(SkRasterPipeline_GradientCtx* ctx, float gapCount, size_t stop,
                             SkPMColor4f c_l, SkPMColor4f c_r) {
    // Clankium's GCC 4.9 targeting ARMv7 is barfing when we use Sk4f math here, so go scalar...
    SkPMColor4f Fs = {
        (c_r.fR - c_l.fR) * gapCount,
        (c_r.fG - c_l.fG) * gapCount,
        (c_r.fB - c_l.fB) * gapCount,
        (c_r.fA - c_l.fA) * gapCount,
    };
    SkPMColor4f Bs = {
        c_l.fR - Fs.fR*(stop/gapCount),
        c_l.fG - Fs.fG*(stop/gapCount),
        c_l.fB - Fs.fB*(stop/gapCount),
        c_l.fA - Fs.fA*(stop/gapCount),
    };
    add_stop_color(ctx, stop, Fs, Bs);
}

// For each stop we calculate a bias B and a scale factor F, such that
// for any t between stops n and n+1, the color we want is B[n] + F[n]*t.
static void init_stop_pos(SkRasterPipeline_GradientCtx* ctx, size_t stop, float t_l, float t_r,
                          SkPMColor4f c_l, SkPMColor4f c_r) {
    // See note about Clankium's old compiler in init_stop_evenly().
    SkPMColor4f Fs = {
        (c_r.fR - c_l.fR) / (t_r - t_l),
        (c_r.fG - c_l.fG) / (t_r - t_l),
        (c_r.fB - c_l.fB) / (t_r - t_l),
        (c_r.fA - c_l.fA) / (t_r - t_l),
    };
    SkPMColor4f Bs = {
        c_l.fR - Fs.fR*t_l,
        c_l.fG - Fs.fG*t_l,
        c_l.fB - Fs.fB*t_l,
        c_l.fA - Fs.fA*t_l,
    };
    ctx->ts[stop] = t_l;
    add_stop_color(ctx, stop, Fs, Bs);
}

bool SkGradientShaderBase::onAppendStages(const SkStageRec& rec) const {
    SkRasterPipeline* p = rec.fPipeline;
    SkArenaAlloc* alloc = rec.fAlloc;
    SkRasterPipeline_DecalTileCtx* decal_ctx = nullptr;

    SkMatrix matrix;
    if (!this->computeTotalInverse(rec.fMatrixProvider.localToDevice(), rec.fLocalM, &matrix)) {
        return false;
    }
    matrix.postConcat(fPtsToUnit);

    SkRasterPipeline_<256> postPipeline;

    p->append(SkRasterPipeline::seed_shader);
    p->append_matrix(alloc, matrix);
    this->appendGradientStages(alloc, p, &postPipeline);

    switch(fTileMode) {
        case SkTileMode::kMirror: p->append(SkRasterPipeline::mirror_x_1); break;
        case SkTileMode::kRepeat: p->append(SkRasterPipeline::repeat_x_1); break;
        case SkTileMode::kDecal:
            decal_ctx = alloc->make<SkRasterPipeline_DecalTileCtx>();
            decal_ctx->limit_x = SkBits2Float(SkFloat2Bits(1.0f) + 1);
            // reuse mask + limit_x stage, or create a custom decal_1 that just stores the mask
            p->append(SkRasterPipeline::decal_x, decal_ctx);
            [[fallthrough]];

        case SkTileMode::kClamp:
            if (!fOrigPos) {
                // We clamp only when the stops are evenly spaced.
                // If not, there may be hard stops, and clamping ruins hard stops at 0 and/or 1.
                // In that case, we must make sure we're using the general "gradient" stage,
                // which is the only stage that will correctly handle unclamped t.
                p->append(SkRasterPipeline::clamp_x_1);
            }
            break;
    }

    const bool premulGrad = this->interpolateInPremul();

    // Transform all of the colors to destination color space, possibly premultiplied
    SkColor4fXformer xformedColors(fOrigColors4f, fColorCount, fInterpolation,
                                   fColorSpace.get(), rec.fDstCS);
    const SkPMColor4f* pmColors = xformedColors.fColors.begin();

    // The two-stop case with stops at 0 and 1.
    if (fColorCount == 2 && fOrigPos == nullptr) {
        const SkPMColor4f c_l = pmColors[0],
                          c_r = pmColors[1];

        // See F and B below.
        auto ctx = alloc->make<SkRasterPipeline_EvenlySpaced2StopGradientCtx>();
        (skvx::float4::Load(c_r.vec()) - skvx::float4::Load(c_l.vec())).store(ctx->f);
        (                                skvx::float4::Load(c_l.vec())).store(ctx->b);
        ctx->interpolatedInPremul = premulGrad;

        p->append(SkRasterPipeline::evenly_spaced_2_stop_gradient, ctx);
    } else {
        auto* ctx = alloc->make<SkRasterPipeline_GradientCtx>();
        ctx->interpolatedInPremul = premulGrad;

        // Note: In order to handle clamps in search, the search assumes a stop conceptully placed
        // at -inf. Therefore, the max number of stops is fColorCount+1.
        for (int i = 0; i < 4; i++) {
            // Allocate at least at for the AVX2 gather from a YMM register.
            ctx->fs[i] = alloc->makeArray<float>(std::max(fColorCount+1, 8));
            ctx->bs[i] = alloc->makeArray<float>(std::max(fColorCount+1, 8));
        }

        if (fOrigPos == nullptr) {
            // Handle evenly distributed stops.

            size_t stopCount = fColorCount;
            float gapCount = stopCount - 1;

            SkPMColor4f c_l = pmColors[0];
            for (size_t i = 0; i < stopCount - 1; i++) {
                SkPMColor4f c_r = pmColors[i + 1];
                init_stop_evenly(ctx, gapCount, i, c_l, c_r);
                c_l = c_r;
            }
            add_const_color(ctx, stopCount - 1, c_l);

            ctx->stopCount = stopCount;
            p->append(SkRasterPipeline::evenly_spaced_gradient, ctx);
        } else {
            // Handle arbitrary stops.

            ctx->ts = alloc->makeArray<float>(fColorCount+1);

            // Remove the default stops inserted by SkGradientShaderBase::SkGradientShaderBase
            // because they are naturally handled by the search method.
            int firstStop;
            int lastStop;
            if (fColorCount > 2) {
                firstStop = fOrigColors4f[0] != fOrigColors4f[1] ? 0 : 1;
                lastStop = fOrigColors4f[fColorCount - 2] != fOrigColors4f[fColorCount - 1]
                           ? fColorCount - 1 : fColorCount - 2;
            } else {
                firstStop = 0;
                lastStop = 1;
            }

            size_t stopCount = 0;
            float  t_l = fOrigPos[firstStop];
            SkPMColor4f c_l = pmColors[firstStop];
            add_const_color(ctx, stopCount++, c_l);
            // N.B. lastStop is the index of the last stop, not one after.
            for (int i = firstStop; i < lastStop; i++) {
                float  t_r = fOrigPos[i + 1];
                SkPMColor4f c_r = pmColors[i + 1];
                SkASSERT(t_l <= t_r);
                if (t_l < t_r) {
                    init_stop_pos(ctx, stopCount, t_l, t_r, c_l, c_r);
                    stopCount += 1;
                }
                t_l = t_r;
                c_l = c_r;
            }

            ctx->ts[stopCount] = t_l;
            add_const_color(ctx, stopCount++, c_l);

            ctx->stopCount = stopCount;
            p->append(SkRasterPipeline::gradient, ctx);
        }
    }

    if (decal_ctx) {
        p->append(SkRasterPipeline::check_decal_mask, decal_ctx);
    }

    if (!premulGrad && !this->colorsAreOpaque()) {
        p->append(SkRasterPipeline::premul);
    }

    p->extend(postPipeline);

    return true;
}

skvm::Color SkGradientShaderBase::onProgram(skvm::Builder* p,
                                            skvm::Coord device, skvm::Coord local,
                                            skvm::Color /*paint*/,
                                            const SkMatrixProvider& mats, const SkMatrix* localM,
                                            const SkColorInfo& dstInfo,
                                            skvm::Uniforms* uniforms, SkArenaAlloc* alloc) const {
    SkMatrix inv;
    if (!this->computeTotalInverse(mats.localToDevice(), localM, &inv)) {
        return {};
    }
    inv.postConcat(fPtsToUnit);
    inv.normalizePerspective();

    local = SkShaderBase::ApplyMatrix(p, inv, local, uniforms);

    skvm::I32 mask = p->splat(~0);
    skvm::F32 t = this->transformT(p,uniforms, local, &mask);

    // Perhaps unexpectedly, clamping is handled naturally by our search, so we
    // don't explicitly clamp t to [0,1].  That clamp would break hard stops
    // right at 0 or 1 boundaries in kClamp mode.  (kRepeat and kMirror always
    // produce values in [0,1].)
    switch(fTileMode) {
        case SkTileMode::kClamp:
            break;

        case SkTileMode::kDecal:
            mask &= (t == clamp01(t));
            break;

        case SkTileMode::kRepeat:
            t = fract(t);
            break;

        case SkTileMode::kMirror: {
            // t = | (t-1) - 2*(floor( (t-1)*0.5 )) - 1 |
            //       {-A-}      {--------B-------}
            skvm::F32 A = t - 1.0f,
                      B = floor(A * 0.5f);
            t = abs(A - (B + B) - 1.0f);
        } break;
    }

    // Transform our colors as we want them interpolated, in dst color space, possibly premul.
    SkColor4fXformer xformedColors(fOrigColors4f, fColorCount, fInterpolation,
                                   fColorSpace.get(), dstInfo.colorSpace());
    const SkPMColor4f* rgba = xformedColors.fColors.begin();

    // Transform our colors into a scale factor f and bias b such that for
    // any t between stops i and i+1, the color we want is mad(t, f[i], b[i]).
    using F4 = skvx::Vec<4,float>;
    struct FB { F4 f,b; };
    skvm::Color color;

    auto uniformF = [&](float x) { return p->uniformF(uniforms->pushF(x)); };

    if (fColorCount == 2) {
        // 2-stop gradients have colors at 0 and 1, and so must be evenly spaced.
        SkASSERT(fOrigPos == nullptr);

        // With 2 stops, we upload the single FB as uniforms and interpolate directly with t.
        F4 lo = F4::Load(rgba + 0),
           hi = F4::Load(rgba + 1);
        F4 F = hi - lo,
           B = lo;

        auto T = clamp01(t);
        color = {
            T * uniformF(F[0]) + uniformF(B[0]),
            T * uniformF(F[1]) + uniformF(B[1]),
            T * uniformF(F[2]) + uniformF(B[2]),
            T * uniformF(F[3]) + uniformF(B[3]),
        };
    } else {
        // To handle clamps in search we add a conceptual stop at t=-inf, so we
        // may need up to fColorCount+1 FBs and fColorCount t stops between them:
        //
        //   FBs:         [color 0]  [color 0->1]  [color 1->2]  [color 2->3]  ...
        //   stops:  (-inf)        t0            t1            t2  ...
        //
        // Both these arrays could end up shorter if any hard stops share the same t.
        FB* fb = alloc->makeArrayDefault<FB>(fColorCount+1);
        std::vector<float> stops;  // TODO: SkSTArray?
        stops.reserve(fColorCount);

        // Here's our conceptual stop at t=-inf covering all t<=0, clamping to our first color.
        float  t_lo = this->getPos(0);
        F4 color_lo = F4::Load(rgba);
        fb[0] = { 0.0f, color_lo };
        // N.B. No stops[] entry for this implicit -inf.

        // Now the non-edge cases, calculating scale and bias between adjacent normal stops.
        for (int i = 1; i < fColorCount; i++) {
            float  t_hi = this->getPos(i);
            F4 color_hi = F4::Load(rgba + i);

            // If t_lo == t_hi, we're on a hard stop, and transition immediately to the next color.
            SkASSERT(t_lo <= t_hi);
            if (t_lo < t_hi) {
                F4 f = (color_hi - color_lo) / (t_hi - t_lo),
                   b = color_lo - f*t_lo;
                stops.push_back(t_lo);
                fb[stops.size()] = {f,b};
            }

            t_lo = t_hi;
            color_lo = color_hi;
        }
        // Anything >= our final t clamps to our final color.
        stops.push_back(t_lo);
        fb[stops.size()] = { 0.0f, color_lo };

        // We'll gather FBs from that array we just created.
        skvm::Uniform fbs = uniforms->pushPtr(fb);

        // Find the two stops we need to interpolate.
        skvm::I32 ix;
        if (fOrigPos == nullptr) {
            // Evenly spaced stops... we can calculate ix directly.
            // Of note: we need to clamp t and skip over that conceptual -inf stop we made up.
            ix = trunc(clamp01(t) * uniformF(stops.size() - 1) + 1.0f);
        } else {
            // Starting ix at 0 bakes in our conceptual first stop at -inf.
            // TODO: good place to experiment with a loop in skvm.... stops.size() can be huge.
            ix = p->splat(0);
            for (float stop : stops) {
                // ix += (t >= stop) ? +1 : 0 ~~>
                // ix -= (t >= stop) ? -1 : 0
                ix -= (t >= uniformF(stop));
            }
            // TODO: we could skip any of the default stops GradientShaderBase's ctor added
            // to ensure the full [0,1] span is covered.  This linear search doesn't need
            // them for correctness, and it'd be up to two fewer stops to check.
            // N.B. we do still need those stops for the fOrigPos == nullptr direct math path.
        }

        // A scale factor and bias for each lane, 8 total.
        // TODO: simpler, faster, tidier to push 8 uniform pointers, one for each struct lane?
        ix = shl(ix, 3);
        skvm::F32 Fr = gatherF(fbs, ix + 0);
        skvm::F32 Fg = gatherF(fbs, ix + 1);
        skvm::F32 Fb = gatherF(fbs, ix + 2);
        skvm::F32 Fa = gatherF(fbs, ix + 3);

        skvm::F32 Br = gatherF(fbs, ix + 4);
        skvm::F32 Bg = gatherF(fbs, ix + 5);
        skvm::F32 Bb = gatherF(fbs, ix + 6);
        skvm::F32 Ba = gatherF(fbs, ix + 7);

        // This is what we've been building towards!
        color = {
            t * Fr + Br,
            t * Fg + Bg,
            t * Fb + Bb,
            t * Fa + Ba,
        };
    }

    // If we interpolated unpremul, premul now to match our output convention.
    if (!this->interpolateInPremul() && !fColorsAreOpaque) {
        color = premul(color);
    }

    return {
        pun_to_F32(mask & pun_to_I32(color.r)),
        pun_to_F32(mask & pun_to_I32(color.g)),
        pun_to_F32(mask & pun_to_I32(color.b)),
        pun_to_F32(mask & pun_to_I32(color.a)),
    };
}


bool SkGradientShaderBase::isOpaque() const {
    return fColorsAreOpaque && (this->getTileMode() != SkTileMode::kDecal);
}

static unsigned rounded_divide(unsigned numer, unsigned denom) {
    return (numer + (denom >> 1)) / denom;
}

bool SkGradientShaderBase::onAsLuminanceColor(SkColor* lum) const {
    // we just compute an average color.
    // possibly we could weight this based on the proportional width for each color
    //   assuming they are not evenly distributed in the fPos array.
    int r = 0;
    int g = 0;
    int b = 0;
    const int n = fColorCount;
    // TODO: use linear colors?
    for (int i = 0; i < n; ++i) {
        SkColor c = this->getLegacyColor(i);
        r += SkColorGetR(c);
        g += SkColorGetG(c);
        b += SkColorGetB(c);
    }
    *lum = SkColorSetRGB(rounded_divide(r, n), rounded_divide(g, n), rounded_divide(b, n));
    return true;
}

SkColor4fXformer::SkColor4fXformer(const SkColor4f* colors, int colorCount,
                                   const SkGradientShader::Interpolation& interpolation,
                                   SkColorSpace* src, SkColorSpace* dst) {
    fColors.reset(colorCount);

    auto info = SkImageInfo::Make(colorCount, 1, kRGBA_F32_SkColorType, kUnpremul_SkAlphaType);

    auto dstInfo = info.makeColorSpace(sk_ref_sp(dst));
    auto srcInfo = info.makeColorSpace(sk_ref_sp(src));
    if (interpolation.fInPremul == SkGradientShader::Interpolation::InPremul::kYes) {
        dstInfo = dstInfo.makeAlphaType(kPremul_SkAlphaType);
    }
    SkAssertResult(SkConvertPixels(dstInfo, fColors.begin(), info.minRowBytes(),
                                   srcInfo, colors         , info.minRowBytes()));
}

SkColorConverter::SkColorConverter(const SkColor* colors, int count) {
    const float ONE_OVER_255 = 1.f / 255;
    for (int i = 0; i < count; ++i) {
        fColors4f.push_back({ SkColorGetR(colors[i]) * ONE_OVER_255,
                              SkColorGetG(colors[i]) * ONE_OVER_255,
                              SkColorGetB(colors[i]) * ONE_OVER_255,
                              SkColorGetA(colors[i]) * ONE_OVER_255 });
    }
}

void SkGradientShaderBase::commonAsAGradient(GradientInfo* info) const {
    if (info) {
        if (info->fColorCount >= fColorCount) {
            if (info->fColors) {
                for (int i = 0; i < fColorCount; ++i) {
                    info->fColors[i] = this->getLegacyColor(i);
                }
            }
            if (info->fColorOffsets) {
                for (int i = 0; i < fColorCount; ++i) {
                    info->fColorOffsets[i] = this->getPos(i);
                }
            }
        }
        info->fColorCount = fColorCount;
        info->fTileMode = fTileMode;

        info->fGradientFlags =
                this->interpolateInPremul() ? SkGradientShader::kInterpolateColorsInPremul_Flag : 0;
    }
}

// Return true if these parameters are valid/legal/safe to construct a gradient
//
bool SkGradientShaderBase::ValidGradient(const SkColor4f colors[], const SkScalar pos[], int count,
                                         SkTileMode tileMode) {
    return nullptr != colors && count >= 1 && (unsigned)tileMode < kSkTileModeCount;
}

SkGradientShaderBase::Descriptor::Descriptor(const SkColor4f colors[],
                                             sk_sp<SkColorSpace> colorSpace,
                                             const SkScalar pos[],
                                             int colorCount,
                                             SkTileMode mode,
                                             const Interpolation& interpolation)
        : fColors(colors)
        , fColorSpace(std::move(colorSpace))
        , fPos(pos)
        , fCount(colorCount)
        , fTileMode(mode)
        , fInterpolation(interpolation) {
    SkASSERT(fCount > 1);
}

static SkColor4f average_gradient_color(const SkColor4f colors[], const SkScalar pos[],
                                        int colorCount) {
    // The gradient is a piecewise linear interpolation between colors. For a given interval,
    // the integral between the two endpoints is 0.5 * (ci + cj) * (pj - pi), which provides that
    // intervals average color. The overall average color is thus the sum of each piece. The thing
    // to keep in mind is that the provided gradient definition may implicitly use p=0 and p=1.
    skvx::float4 blend(0.0f);
    for (int i = 0; i < colorCount - 1; ++i) {
        // Calculate the average color for the interval between pos(i) and pos(i+1)
        auto c0 = skvx::float4::Load(&colors[i]);
        auto c1 = skvx::float4::Load(&colors[i + 1]);

        // when pos == null, there are colorCount uniformly distributed stops, going from 0 to 1,
        // so pos[i + 1] - pos[i] = 1/(colorCount-1)
        SkScalar w;
        if (pos) {
            // Match position fixing in SkGradientShader's constructor, clamping positions outside
            // [0, 1] and forcing the sequence to be monotonic
            SkScalar p0 = SkTPin(pos[i], 0.f, 1.f);
            SkScalar p1 = SkTPin(pos[i + 1], p0, 1.f);
            w = p1 - p0;

            // And account for any implicit intervals at the start or end of the positions
            if (i == 0) {
                if (p0 > 0.0f) {
                    // The first color is fixed between p = 0 to pos[0], so 0.5*(ci + cj)*(pj - pi)
                    // becomes 0.5*(c + c)*(pj - 0) = c * pj
                    auto c = skvx::float4::Load(&colors[0]);
                    blend += p0 * c;
                }
            }
            if (i == colorCount - 2) {
                if (p1 < 1.f) {
                    // The last color is fixed between pos[n-1] to p = 1, so 0.5*(ci + cj)*(pj - pi)
                    // becomes 0.5*(c + c)*(1 - pi) = c * (1 - pi)
                    auto c = skvx::float4::Load(&colors[colorCount - 1]);
                    blend += (1.f - p1) * c;
                }
            }
        } else {
            w = 1.f / (colorCount - 1);
        }

        blend += 0.5f * w * (c1 + c0);
    }

    SkColor4f avg;
    blend.store(&avg);
    return avg;
}

// Except for special circumstances of clamped gradients, every gradient shape--when degenerate--
// can be mapped to the same fallbacks. The specific shape factories must account for special
// clamped conditions separately, this will always return the last color for clamped gradients.
sk_sp<SkShader> SkGradientShaderBase::MakeDegenerateGradient(const SkColor4f colors[],
                                                             const SkScalar pos[],
                                                             int colorCount,
                                                             sk_sp<SkColorSpace> colorSpace,
                                                             SkTileMode mode) {
    switch(mode) {
        case SkTileMode::kDecal:
            // normally this would reject the area outside of the interpolation region, so since
            // inside region is empty when the radii are equal, the entire draw region is empty
            return SkShaders::Empty();
        case SkTileMode::kRepeat:
        case SkTileMode::kMirror:
            // repeat and mirror are treated the same: the border colors are never visible,
            // but approximate the final color as infinite repetitions of the colors, so
            // it can be represented as the average color of the gradient.
            return SkShaders::Color(
                    average_gradient_color(colors, pos, colorCount), std::move(colorSpace));
        case SkTileMode::kClamp:
            // Depending on how the gradient shape degenerates, there may be a more specialized
            // fallback representation for the factories to use, but this is a reasonable default.
            return SkShaders::Color(colors[colorCount - 1], std::move(colorSpace));
    }
    SkDEBUGFAIL("Should not be reached");
    return nullptr;
}

SkGradientShaderBase::ColorStopOptimizer::ColorStopOptimizer(const SkColor4f* colors,
                                                             const SkScalar* pos,
                                                             int count,
                                                             SkTileMode mode)
        : fColors(colors)
        , fPos(pos)
        , fCount(count) {

    if (!pos || count != 3) {
        return;
    }

    if (SkScalarNearlyEqual(pos[0], 0.0f) &&
        SkScalarNearlyEqual(pos[1], 0.0f) &&
        SkScalarNearlyEqual(pos[2], 1.0f)) {

        if (SkTileMode::kRepeat == mode || SkTileMode::kMirror == mode ||
            colors[0] == colors[1]) {

            // Ignore the leftmost color/pos.
            fColors += 1;
            fPos    += 1;
            fCount   = 2;
        }
    } else if (SkScalarNearlyEqual(pos[0], 0.0f) &&
               SkScalarNearlyEqual(pos[1], 1.0f) &&
               SkScalarNearlyEqual(pos[2], 1.0f)) {

        if (SkTileMode::kRepeat == mode || SkTileMode::kMirror == mode ||
            colors[1] == colors[2]) {

            // Ignore the rightmost color/pos.
            fCount  = 2;
        }
    }
}
