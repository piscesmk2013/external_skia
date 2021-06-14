/*
 * Copyright 2019 Google LLC.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef GrTessellationPathRenderer_DEFINED
#define GrTessellationPathRenderer_DEFINED

#include "src/core/SkIPoint16.h"
#include "src/gpu/GrDynamicAtlas.h"
#include "src/gpu/GrOnFlushResourceProvider.h"
#include "src/gpu/GrPathRenderer.h"
#include <map>

// This is the tie-in point for path rendering via GrPathTessellateOp. This path renderer draws
// paths using a hybrid Red Book "stencil, then cover" method. Curves get linearized by GPU
// tessellation shaders. This path renderer doesn't apply analytic AA, so it requires MSAA if AA is
// desired.
class GrTessellationPathRenderer : public GrPathRenderer, public GrOnFlushCallbackObject {
public:
    // Don't allow linearized segments to be off by more than 1/4th of a pixel from the true curve.
    constexpr static float kLinearizationPrecision = 4;

    // We send these flags to the internal path filling Ops to control how a path gets rendered.
    enum class PathFlags {
        kNone = 0,
        kStencilOnly = (1 << 0),
        kWireframe = (1 << 1)
    };

    static bool IsSupported(const GrCaps&);

    GrTessellationPathRenderer(GrRecordingContext*);
    const char* name() const final { return "GrTessellationPathRenderer"; }

    StencilSupport onGetStencilSupport(const GrStyledShape&) const override;
    CanDrawPath onCanDrawPath(const CanDrawPathArgs&) const override;

    bool onDrawPath(const DrawPathArgs&) override;
    void onStencilPath(const StencilPathArgs&) override;

    void preFlush(GrOnFlushResourceProvider*, SkSpan<const uint32_t> taskIDs) override;

private:
    SkPath* getAtlasUberPath(SkPathFillType fillType, bool antialias) {
        int idx = (int)antialias << 1;
        idx |= (int)fillType & 1;
        return &fAtlasUberPaths[idx];
    }
    // Adds the filled path to fAtlas if the path is small enough, and if the atlas isn't full.
    // Currently, "small enough" means 128*128 total pixels or less, and no larger than half the
    // atlas size in either dimension.
    bool tryAddPathToAtlas(const GrCaps&, const SkMatrix&, const SkPath&,
                           const SkRect& pathDevBounds, bool antialias, SkIRect* devIBounds,
                           SkIPoint16* locationInAtlas, bool* transposedInAtlas);
    void renderAtlas(GrOnFlushResourceProvider*);

    GrDynamicAtlas fAtlas;
    int fMaxAtlasPathWidth = 0;
    SkPath fAtlasUberPaths[4];  // 2 fillTypes * 2 antialias modes.

    // This simple cache remembers the locations of cacheable path masks in the atlas. Its main
    // motivation is for clip paths.
    struct AtlasPathKey {
        void set(const SkMatrix&, bool antialias, const SkPath&);
        bool operator==(const AtlasPathKey& k) const {
            static_assert(sizeof(*this) == sizeof(uint32_t) * 6);
            return !memcmp(this, &k, sizeof(*this));
        }
        float fAffineMatrix[4];
        uint8_t fSubpixelPositionKey[2];
        uint8_t fAntialias;
        uint8_t fFillRule;
        uint32_t fPathGenID;
    };
    SkTHashMap<AtlasPathKey, SkIPoint16> fAtlasPathCache;
};

GR_MAKE_BITFIELD_CLASS_OPS(GrTessellationPathRenderer::PathFlags);

#endif
