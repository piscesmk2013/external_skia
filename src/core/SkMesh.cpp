/*
 * Copyright 2021 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "include/core/SkMesh.h"

#ifdef SK_ENABLE_SKSL
#include "include/core/SkColorSpace.h"
#include "include/core/SkData.h"
#include "include/core/SkMath.h"
#include "include/private/SkOpts_spi.h"
#include "include/private/SkSLProgramElement.h"
#include "include/private/SkSLProgramKind.h"
#include "src/core/SkMeshPriv.h"
#include "src/core/SkRuntimeEffectPriv.h"
#include "src/core/SkSafeMath.h"
#include "src/sksl/SkSLAnalysis.h"
#include "src/sksl/SkSLBuiltinTypes.h"
#include "src/sksl/SkSLCompiler.h"
#include "src/sksl/SkSLUtil.h"
#include "src/sksl/analysis/SkSLProgramVisitor.h"
#include "src/sksl/ir/SkSLFieldAccess.h"
#include "src/sksl/ir/SkSLFunctionDeclaration.h"
#include "src/sksl/ir/SkSLFunctionDefinition.h"
#include "src/sksl/ir/SkSLProgram.h"
#include "src/sksl/ir/SkSLReturnStatement.h"
#include "src/sksl/ir/SkSLType.h"
#include "src/sksl/ir/SkSLVarDeclarations.h"
#include "src/sksl/ir/SkSLVariable.h"
#include "src/sksl/ir/SkSLVariableReference.h"

#if SK_SUPPORT_GPU
#include "src/gpu/ganesh/GrGpu.h"
#include "src/gpu/ganesh/GrStagingBufferManager.h"
#endif  // SK_SUPPORT_GPU

#include <locale>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

using Attribute = SkMeshSpecification::Attribute;
using Varying   = SkMeshSpecification::Varying;

using IndexBuffer  = SkMesh::IndexBuffer;
using VertexBuffer = SkMesh::VertexBuffer;

#define RETURN_FAILURE(...) return Result{nullptr, SkStringPrintf(__VA_ARGS__)}

#define RETURN_ERROR(...) return std::make_tuple(false, SkStringPrintf(__VA_ARGS__))

#define RETURN_SUCCESS return std::make_tuple(true, SkString{})

using Uniform = SkMeshSpecification::Uniform;

static std::vector<Uniform>::iterator find_uniform(std::vector<Uniform>& uniforms,
                                                   std::string_view name) {
    return std::find_if(uniforms.begin(), uniforms.end(),
                        [name](const SkMeshSpecification::Uniform& u) { return u.name == name; });
}

static std::tuple<bool, SkString>
gather_uniforms_and_check_for_main(const SkSL::Program& program,
                                   std::vector<Uniform>* uniforms,
                                   SkMeshSpecification::Uniform::Flags stage,
                                   size_t* offset) {
    bool foundMain = false;
    for (const SkSL::ProgramElement* elem : program.elements()) {
        if (elem->is<SkSL::FunctionDefinition>()) {
            const SkSL::FunctionDefinition& defn = elem->as<SkSL::FunctionDefinition>();
            const SkSL::FunctionDeclaration& decl = defn.declaration();
            if (decl.isMain()) {
                foundMain = true;
            }
        } else if (elem->is<SkSL::GlobalVarDeclaration>()) {
            const SkSL::GlobalVarDeclaration& global = elem->as<SkSL::GlobalVarDeclaration>();
            const SkSL::VarDeclaration& varDecl = global.declaration()->as<SkSL::VarDeclaration>();
            const SkSL::Variable& var = *varDecl.var();
            if (var.modifiers().fFlags & SkSL::Modifiers::kUniform_Flag) {
                auto iter = find_uniform(*uniforms, var.name());
                const auto& context = *program.fContext;
                if (iter == uniforms->end()) {
                    uniforms->push_back(SkRuntimeEffectPriv::VarAsUniform(var, context, offset));
                    uniforms->back().flags |= stage;
                } else {
                    // Check that the two declarations are equivalent
                    size_t ignoredOffset = 0;
                    auto uniform = SkRuntimeEffectPriv::VarAsUniform(var, context, &ignoredOffset);
                    if (uniform.isArray() != iter->isArray() ||
                        uniform.type      != iter->type      ||
                        uniform.count     != iter->count) {
                        return {false, SkStringPrintf("Uniform %.*s declared with different types"
                                                      " in vertex and fragment shaders.",
                                                      (int)iter->name.size(), iter->name.data())};
                    }
                    if (uniform.isColor() != iter->isColor()) {
                        return {false, SkStringPrintf("Uniform %.*s declared with different color"
                                                      " layout in vertex and fragment shaders.",
                                                      (int)iter->name.size(), iter->name.data())};
                    }
                    (*iter).flags |= stage;
                }
            }
        }
    }
    if (!foundMain) {
        return {false, SkString("No main function found.")};
    }
    return {true, {}};
}

using ColorType = SkMeshSpecificationPriv::ColorType;

ColorType get_fs_color_type(const SkSL::Program& fsProgram) {
    for (const SkSL::ProgramElement* elem : fsProgram.elements()) {
        if (elem->is<SkSL::FunctionDefinition>()) {
            const SkSL::FunctionDefinition& defn = elem->as<SkSL::FunctionDefinition>();
            const SkSL::FunctionDeclaration& decl = defn.declaration();
            if (decl.isMain()) {
                SkASSERT(decl.parameters().size() == 1 || decl.parameters().size() == 2);
                if (decl.parameters().size() == 1) {
                    return ColorType::kNone;
                }
                const SkSL::Type& paramType = decl.parameters()[1]->type();
                SkASSERT(paramType.matches(*fsProgram.fContext->fTypes.fHalf4) ||
                         paramType.matches(*fsProgram.fContext->fTypes.fFloat4));
                return paramType.matches(*fsProgram.fContext->fTypes.fHalf4) ? ColorType::kHalf4
                                                                             : ColorType::kFloat4;
            }
        }
    }
    SkUNREACHABLE;
}

// This is a non-exhaustive check for the validity of a variable name. The SkSL compiler will
// actually process the name. We're just guarding against having multiple tokens embedded in the
// name before we put it into a struct definition.
static bool check_name(const SkString& name) {
    if (name.isEmpty()) {
        return false;
    }
    for (size_t i = 0; i < name.size(); ++i) {
        if (name[i] != '_' && !std::isalnum(name[i], std::locale::classic())) {
            return false;
        }
    }
    return true;
}

static size_t attribute_type_size(Attribute::Type type) {
    switch (type) {
        case Attribute::Type::kFloat:         return 4;
        case Attribute::Type::kFloat2:        return 2*4;
        case Attribute::Type::kFloat3:        return 3*4;
        case Attribute::Type::kFloat4:        return 4*4;
        case Attribute::Type::kUByte4_unorm:  return 4;
    }
    SkUNREACHABLE;
};

static const char* attribute_type_string(Attribute::Type type) {
    switch (type) {
        case Attribute::Type::kFloat:         return "float";
        case Attribute::Type::kFloat2:        return "float2";
        case Attribute::Type::kFloat3:        return "float3";
        case Attribute::Type::kFloat4:        return "float4";
        case Attribute::Type::kUByte4_unorm:  return "half4";
    }
    SkUNREACHABLE;
};

static const char* varying_type_string(Varying::Type type) {
    switch (type) {
        case Varying::Type::kFloat:  return "float";
        case Varying::Type::kFloat2: return "float2";
        case Varying::Type::kFloat3: return "float3";
        case Varying::Type::kFloat4: return "float4";
        case Varying::Type::kHalf:   return "half";
        case Varying::Type::kHalf2:  return "half2";
        case Varying::Type::kHalf3:  return "half3";
        case Varying::Type::kHalf4:  return "half4";
    }
    SkUNREACHABLE;
};

std::tuple<bool, SkString>
check_vertex_offsets_and_stride(SkSpan<const Attribute> attributes,
                                size_t                  stride) {
    // Vulkan 1.0 has a minimum maximum attribute count of 2048.
    static_assert(SkMeshSpecification::kMaxStride       <= 2048);
    // ES 2 has a max of 8.
    static_assert(SkMeshSpecification::kMaxAttributes   <= 8);
    // Four bytes alignment is required by Metal.
    static_assert(SkMeshSpecification::kStrideAlignment >= 4);
    static_assert(SkMeshSpecification::kOffsetAlignment >= 4);
    // ES2 has a minimum maximum of 8. We may need one for a broken gl_FragCoord workaround and
    // one for local coords.
    static_assert(SkMeshSpecification::kMaxVaryings     <= 6);

    if (attributes.empty()) {
        RETURN_ERROR("At least 1 attribute is required.");
    }
    if (attributes.size() > SkMeshSpecification::kMaxAttributes) {
        RETURN_ERROR("A maximum of %zu attributes is allowed.",
                     SkMeshSpecification::kMaxAttributes);
    }
    static_assert(SkIsPow2(SkMeshSpecification::kStrideAlignment));
    if (stride == 0 || stride & (SkMeshSpecification::kStrideAlignment - 1)) {
        RETURN_ERROR("Vertex stride must be a non-zero multiple of %zu.",
                     SkMeshSpecification::kStrideAlignment);
    }
    if (stride > SkMeshSpecification::kMaxStride) {
        RETURN_ERROR("Stride cannot exceed %zu.", SkMeshSpecification::kMaxStride);
    }
    for (const auto& a : attributes) {
        if (a.offset & (SkMeshSpecification::kOffsetAlignment - 1)) {
            RETURN_ERROR("Attribute offset must be a multiple of %zu.",
                         SkMeshSpecification::kOffsetAlignment);
        }
        // This equivalent to vertexAttributeAccessBeyondStride==VK_FALSE in
        // VK_KHR_portability_subset. First check is to avoid overflow in second check.
        if (a.offset >= stride || a.offset + attribute_type_size(a.type) > stride) {
            RETURN_ERROR("Attribute offset plus size cannot exceed stride.");
        }
    }
    RETURN_SUCCESS;
}

static int check_for_passthrough_local_coords(const SkSL::Program& fsProgram) {
    using namespace SkSL;

    class Visitor final : public SkSL::ProgramVisitor {
    public:
        Visitor(const Context& context) : fContext(context) {}

        void visit(const Program& program) { ProgramVisitor::visit(program); }

        int fieldIndex() const { return fFieldIndex; }

    protected:
        bool visitProgramElement(const ProgramElement& p) override {
            if (p.is<FunctionDefinition>() && p.as<FunctionDefinition>().declaration().isMain()) {
                SkASSERT(!fVaryings);
                fVaryings = p.as<FunctionDefinition>().declaration().parameters()[0];
                return ProgramVisitor::visitProgramElement(p);
            }
            // We don't need to visit anything outside of main().
            return false;
        }

        bool visitStatement(const Statement& s) override {
            // We should only get here if are in main and therefore found the varyings parameter.
            SkASSERT(fVaryings);

            static constexpr int kFailed = -2;

            // If we've already detected a non-conforming individual or set of returns then we
            // should have bailed by returning true.
            SkASSERT(fFieldIndex != kFailed);

            if (!s.is<ReturnStatement>()) {
                return ProgramVisitor::visitStatement(s);
            }

            // We just detect simple cases like "return varyings.foo;"
            const auto& rs = s.as<ReturnStatement>();
            SkASSERT(rs.expression());
            if (!rs.expression()->is<FieldAccess>()) {
                fFieldIndex = kFailed;
                return true;
            }
            const auto& fa = rs.expression()->as<FieldAccess>();
            if (!fa.base()->is<VariableReference>()) {
                fFieldIndex = kFailed;
                return true;
            }
            const auto& baseRef = fa.base()->as<VariableReference>();
            if (baseRef.variable() != fVaryings) {
                fFieldIndex = kFailed;
                return true;
            }
            if (fFieldIndex >= 0) {
                // We already found an OK return statement. Check if this one returns the same
                // field.
                if (fa.fieldIndex() != fFieldIndex) {
                    fFieldIndex = kFailed;
                    return true;
                }
                return false;
            }
            const Type::Field& field = fVaryings->type().fields()[fa.fieldIndex()];
            if (!field.fType->matches(*fContext.fTypes.fFloat2)) {
                fFieldIndex = kFailed;
                return true;
            }
            fFieldIndex = fa.fieldIndex();
            return false;
        }

    private:
        const Context&  fContext;
        const Variable* fVaryings   = nullptr;
        int             fFieldIndex = -1;
    };

    Visitor v(*fsProgram.fContext);
    v.visit(fsProgram);
    return v.fieldIndex();
}

SkMeshSpecification::Result SkMeshSpecification::Make(SkSpan<const Attribute> attributes,
                                                      size_t vertexStride,
                                                      SkSpan<const Varying> varyings,
                                                      const SkString& vs,
                                                      const SkString& fs) {
    return Make(attributes,
                vertexStride,
                varyings,
                vs,
                fs,
                SkColorSpace::MakeSRGB(),
                kPremul_SkAlphaType);
}

SkMeshSpecification::Result SkMeshSpecification::Make(SkSpan<const Attribute> attributes,
                                                      size_t vertexStride,
                                                      SkSpan<const Varying> varyings,
                                                      const SkString& vs,
                                                      const SkString& fs,
                                                      sk_sp<SkColorSpace> cs) {
    return Make(attributes, vertexStride, varyings, vs, fs, std::move(cs), kPremul_SkAlphaType);
}

SkMeshSpecification::Result SkMeshSpecification::Make(SkSpan<const Attribute> attributes,
                                                      size_t vertexStride,
                                                      SkSpan<const Varying> varyings,
                                                      const SkString& vs,
                                                      const SkString& fs,
                                                      sk_sp<SkColorSpace> cs,
                                                      SkAlphaType at) {
    SkString attributesStruct("struct Attributes {\n");
    for (const auto& a : attributes) {
        attributesStruct.appendf("  %s %s;\n", attribute_type_string(a.type), a.name.c_str());
    }
    attributesStruct.append("};\n");

    bool userProvidedPositionVarying = false;
    for (const auto& v : varyings) {
        if (v.name.equals("position")) {
            if (v.type != Varying::Type::kFloat2) {
                return {nullptr, SkString("Varying \"position\" must have type float2.")};
            }
            userProvidedPositionVarying = true;
        }
    }

    SkSTArray<kMaxVaryings, Varying> tempVaryings;
    if (!userProvidedPositionVarying) {
        // Even though we check the # of varyings in MakeFromSourceWithStructs we check here, too,
        // to avoid overflow with + 1.
        if (varyings.size() > kMaxVaryings - 1) {
            RETURN_FAILURE("A maximum of %zu varyings is allowed.", kMaxVaryings);
        }
        for (const auto& v : varyings) {
            tempVaryings.push_back(v);
        }
        tempVaryings.push_back(Varying{Varying::Type::kFloat2, SkString("position")});
        varyings = tempVaryings;
    }

    SkString varyingStruct("struct Varyings {\n");
    for (const auto& v : varyings) {
        varyingStruct.appendf("  %s %s;\n", varying_type_string(v.type), v.name.c_str());
    }
    varyingStruct.append("};\n");

    SkString fullVS;
    fullVS.append(varyingStruct.c_str());
    fullVS.append(attributesStruct.c_str());
    fullVS.append(vs.c_str());

    SkString fullFS;
    fullFS.append(varyingStruct.c_str());
    fullFS.append(fs.c_str());

    return MakeFromSourceWithStructs(attributes,
                                     vertexStride,
                                     varyings,
                                     fullVS,
                                     fullFS,
                                     std::move(cs),
                                     at);
}

SkMeshSpecification::Result SkMeshSpecification::MakeFromSourceWithStructs(
        SkSpan<const Attribute> attributes,
        size_t                  stride,
        SkSpan<const Varying>   varyings,
        const SkString&         vs,
        const SkString&         fs,
        sk_sp<SkColorSpace>     cs,
        SkAlphaType             at) {
    if (auto [ok, error] = check_vertex_offsets_and_stride(attributes, stride); !ok) {
        return {nullptr, error};
    }

    for (const auto& a : attributes) {
        if (!check_name(a.name)) {
            RETURN_FAILURE("\"%s\" is not a valid attribute name.", a.name.c_str());
        }
    }

    if (varyings.size() > kMaxVaryings) {
        RETURN_FAILURE("A maximum of %zu varyings is allowed.", kMaxVaryings);
    }

    for (const auto& v : varyings) {
        if (!check_name(v.name)) {
            return {nullptr, SkStringPrintf("\"%s\" is not a valid varying name.", v.name.c_str())};
        }
    }

    std::vector<Uniform> uniforms;
    size_t offset = 0;

    SkSL::Compiler compiler(SkSL::ShaderCapsFactory::Standalone());

    // Disable memory pooling; this might slow down compilation slightly, but it will ensure that a
    // long-lived mesh specification doesn't waste memory.
    SkSL::ProgramSettings settings;
    settings.fUseMemoryPool = false;

    // TODO(skia:11209): Add SkCapabilities to the API, check against required version.
    std::unique_ptr<SkSL::Program> vsProgram = compiler.convertProgram(
            SkSL::ProgramKind::kMeshVertex,
            std::string(vs.c_str()),
            settings);
    if (!vsProgram) {
        RETURN_FAILURE("VS: %s", compiler.errorText().c_str());
    }

    if (auto [result, error] = gather_uniforms_and_check_for_main(
                *vsProgram,
                &uniforms,
                SkMeshSpecification::Uniform::Flags::kVertex_Flag,
                &offset);
        !result) {
        return {nullptr, std::move(error)};
    }

    if (SkSL::Analysis::CallsColorTransformIntrinsics(*vsProgram)) {
        RETURN_FAILURE("Color transform intrinsics are not permitted in custom mesh shaders");
    }

    std::unique_ptr<SkSL::Program> fsProgram = compiler.convertProgram(
            SkSL::ProgramKind::kMeshFragment,
            std::string(fs.c_str()),
            settings);

    if (!fsProgram) {
        RETURN_FAILURE("FS: %s", compiler.errorText().c_str());
    }

    if (auto [result, error] = gather_uniforms_and_check_for_main(
                *fsProgram,
                &uniforms,
                SkMeshSpecification::Uniform::Flags::kFragment_Flag,
                &offset);
        !result) {
        return {nullptr, std::move(error)};
    }

    if (SkSL::Analysis::CallsColorTransformIntrinsics(*fsProgram)) {
        RETURN_FAILURE("Color transform intrinsics are not permitted in custom mesh shaders");
    }

    ColorType ct = get_fs_color_type(*fsProgram);

    if (ct == ColorType::kNone) {
        cs = nullptr;
        at = kPremul_SkAlphaType;
    } else {
        if (!cs) {
            return {nullptr, SkString{"Must provide a color space if FS returns a color."}};
        }
        if (at == kUnknown_SkAlphaType) {
            return {nullptr, SkString{"Must provide a valid alpha type if FS returns a color."}};
        }
    }

    int passthroughLocalCoordsVaryingIndex = check_for_passthrough_local_coords(*fsProgram);
    if (passthroughLocalCoordsVaryingIndex >= 0) {
        SkASSERT(varyings[passthroughLocalCoordsVaryingIndex].type == Varying::Type::kFloat2);
    }

    return {sk_sp<SkMeshSpecification>(new SkMeshSpecification(attributes,
                                                               stride,
                                                               varyings,
                                                               passthroughLocalCoordsVaryingIndex,
                                                               std::move(uniforms),
                                                               std::move(vsProgram),
                                                               std::move(fsProgram),
                                                               ct,
                                                               std::move(cs),
                                                               at)),
            /*error=*/{}};
}

SkMeshSpecification::~SkMeshSpecification() = default;

SkMeshSpecification::SkMeshSpecification(
        SkSpan<const Attribute>              attributes,
        size_t                               stride,
        SkSpan<const Varying>                varyings,
        int                                  passthroughLocalCoordsVaryingIndex,
        std::vector<Uniform>                 uniforms,
        std::unique_ptr<const SkSL::Program> vs,
        std::unique_ptr<const SkSL::Program> fs,
        ColorType                            ct,
        sk_sp<SkColorSpace>                  cs,
        SkAlphaType                          at)
        : fAttributes(attributes.begin(), attributes.end())
        , fVaryings(varyings.begin(), varyings.end())
        , fUniforms(std::move(uniforms))
        , fVS(std::move(vs))
        , fFS(std::move(fs))
        , fStride(stride)
        , fPassthroughLocalCoordsVaryingIndex(passthroughLocalCoordsVaryingIndex)
        , fColorType(ct)
        , fColorSpace(std::move(cs))
        , fAlphaType(at) {
    fHash = SkOpts::hash_fn(fVS->fSource->c_str(), fVS->fSource->size(), 0);
    fHash = SkOpts::hash_fn(fFS->fSource->c_str(), fFS->fSource->size(), fHash);

    // The attributes and varyings SkSL struct declarations are included in the program source.
    // However, the attribute offsets and types need to be included, the latter because the SkSL
    // struct definition has the GPU type but not the CPU data format.
    for (const auto& a : fAttributes) {
        fHash = SkOpts::hash_fn(&a.offset, sizeof(a.offset), fHash);
        fHash = SkOpts::hash_fn(&a.type,   sizeof(a.type),   fHash);
    }

    fHash = SkOpts::hash_fn(&stride, sizeof(stride), fHash);

    uint64_t csHash = fColorSpace ? fColorSpace->hash() : 0;
    fHash = SkOpts::hash_fn(&csHash, sizeof(csHash), fHash);

    auto atInt = static_cast<uint32_t>(fAlphaType);
    fHash = SkOpts::hash_fn(&atInt, sizeof(atInt), fHash);
}

size_t SkMeshSpecification::uniformSize() const {
    return fUniforms.empty() ? 0
                             : SkAlign4(fUniforms.back().offset + fUniforms.back().sizeInBytes());
}

const Uniform* SkMeshSpecification::findUniform(std::string_view name) const {
    auto iter = std::find_if(fUniforms.begin(), fUniforms.end(), [name] (const Uniform& u) {
        return u.name == name;
    });
    return iter == fUniforms.end() ? nullptr : &(*iter);
}

const Attribute* SkMeshSpecification::findAttribute(std::string_view name) const {
    auto iter = std::find_if(fAttributes.begin(), fAttributes.end(), [name](const Attribute& a) {
        return name.compare(a.name.c_str()) == 0;
    });
    return iter == fAttributes.end() ? nullptr : &(*iter);
}

const Varying* SkMeshSpecification::findVarying(std::string_view name) const {
    auto iter = std::find_if(fVaryings.begin(), fVaryings.end(), [name](const Varying& v) {
        return name.compare(v.name.c_str()) == 0;
    });
    return iter == fVaryings.end() ? nullptr : &(*iter);
}

//////////////////////////////////////////////////////////////////////////////

SkMesh::SkMesh()  = default;
SkMesh::~SkMesh() = default;

SkMesh::SkMesh(const SkMesh&) = default;
SkMesh::SkMesh(SkMesh&&)      = default;

SkMesh& SkMesh::operator=(const SkMesh&) = default;
SkMesh& SkMesh::operator=(SkMesh&&)      = default;

sk_sp<IndexBuffer> SkMesh::MakeIndexBuffer(GrDirectContext* dc, const void* data, size_t size) {
    if (!dc) {
        return SkMeshPriv::CpuIndexBuffer::Make(data, size);
    }
#if SK_SUPPORT_GPU
    return SkMeshPriv::GpuIndexBuffer::Make(dc, data, size);
#endif
    return nullptr;
}

sk_sp<IndexBuffer> SkMesh::CopyIndexBuffer(GrDirectContext* dc, sk_sp<IndexBuffer> src) {
    if (!src) {
        return nullptr;
    }
    auto* ib = static_cast<SkMeshPriv::IB*>(src.get());
    const void* data = ib->peek();
    if (!data) {
        return nullptr;
    }
    return MakeIndexBuffer(dc, data, ib->size());
}

sk_sp<VertexBuffer> SkMesh::MakeVertexBuffer(GrDirectContext* dc, const void* data, size_t size) {
    if (!dc) {
        return SkMeshPriv::CpuVertexBuffer::Make(data, size);
    }
#if SK_SUPPORT_GPU
    return SkMeshPriv::GpuVertexBuffer::Make(dc, data, size);
#endif
    return nullptr;
}

sk_sp<VertexBuffer> SkMesh::CopyVertexBuffer(GrDirectContext* dc, sk_sp<VertexBuffer> src) {
    if (!src) {
        return nullptr;
    }
    auto* vb = static_cast<SkMeshPriv::VB*>(src.get());
    const void* data = vb->peek();
    if (!data) {
        return nullptr;
    }
    return MakeVertexBuffer(dc, data, vb->size());
}

SkMesh SkMesh::Make(sk_sp<SkMeshSpecification> spec,
                    Mode mode,
                    sk_sp<VertexBuffer> vb,
                    size_t vertexCount,
                    size_t vertexOffset,
                    sk_sp<const SkData> uniforms,
                    const SkRect& bounds) {
    SkMesh cm;
    cm.fSpec     = std::move(spec);
    cm.fMode     = mode;
    cm.fVB       = std::move(vb);
    cm.fUniforms = std::move(uniforms);
    cm.fVCount   = vertexCount;
    cm.fVOffset  = vertexOffset;
    cm.fBounds   = bounds;
    return cm.validate() ? cm : SkMesh{};
}

SkMesh SkMesh::MakeIndexed(sk_sp<SkMeshSpecification> spec,
                           Mode mode,
                           sk_sp<VertexBuffer> vb,
                           size_t vertexCount,
                           size_t vertexOffset,
                           sk_sp<IndexBuffer> ib,
                           size_t indexCount,
                           size_t indexOffset,
                           sk_sp<const SkData> uniforms,
                           const SkRect& bounds) {
    SkMesh cm;
    cm.fSpec     = std::move(spec);
    cm.fMode     = mode;
    cm.fVB       = std::move(vb);
    cm.fVCount   = vertexCount;
    cm.fVOffset  = vertexOffset;
    cm.fIB       = std::move(ib);
    cm.fUniforms = std::move(uniforms);
    cm.fICount   = indexCount;
    cm.fIOffset  = indexOffset;
    cm.fBounds   = bounds;
    return cm.validate() ? cm : SkMesh{};
}

bool SkMesh::isValid() const {
    bool valid = SkToBool(fSpec);
    SkASSERT(valid == this->validate());
    return valid;
}

static size_t min_vcount_for_mode(SkMesh::Mode mode) {
    switch (mode) {
        case SkMesh::Mode::kTriangles:     return 3;
        case SkMesh::Mode::kTriangleStrip: return 3;
    }
    SkUNREACHABLE;
}

bool SkMesh::validate() const {
    if (!fSpec) {
        return false;
    }

    if (!fVB) {
        return false;
    }

    if (!fVCount) {
        return false;
    }

    auto vb = static_cast<SkMeshPriv::VB*>(fVB.get());
    auto ib = static_cast<SkMeshPriv::IB*>(fIB.get());

    SkSafeMath sm;
    size_t vsize = sm.mul(fSpec->stride(), fVCount);
    if (sm.add(vsize, fVOffset) > vb->size()) {
        return false;
    }

    if (fVOffset%fSpec->stride() != 0) {
        return false;
    }

    if (size_t uniformSize = fSpec->uniformSize()) {
        if (!fUniforms || fUniforms->size() < uniformSize) {
            return false;
        }
    }

    if (ib) {
        if (fICount < min_vcount_for_mode(fMode)) {
            return false;
        }
        size_t isize = sm.mul(sizeof(uint16_t), fICount);
        if (sm.add(isize, fIOffset) > ib->size()) {
            return false;
        }
        // If we allow 32 bit indices then this should enforce 4 byte alignment in that case.
        if (!SkIsAlign2(fIOffset)) {
            return false;
        }
    } else {
        if (fVCount < min_vcount_for_mode(fMode)) {
            return false;
        }
        if (fICount || fIOffset) {
            return false;
        }
    }

    return sm.ok();
}

//////////////////////////////////////////////////////////////////////////////

static inline bool check_update(const void* data, size_t offset, size_t size, size_t bufferSize) {
    SkSafeMath sm;
    return data                                &&
           size                                &&
           SkIsAlign4(offset)                  &&
           SkIsAlign4(size)                    &&
           sm.add(offset, size) <= bufferSize  &&
           sm.ok();
}

bool SkMesh::IndexBuffer::update(GrDirectContext* dc,
                                 const void* data,
                                 size_t offset,
                                 size_t size) {
    return check_update(data, offset, size, this->size()) && this->onUpdate(dc, data, offset, size);
}

bool SkMesh::VertexBuffer::update(GrDirectContext* dc,
                                  const void* data,
                                  size_t offset,
                                  size_t size) {
    return check_update(data, offset, size, this->size()) && this->onUpdate(dc, data, offset, size);
}

#if SK_SUPPORT_GPU
bool SkMeshPriv::UpdateGpuBuffer(GrDirectContext* dc,
                                 sk_sp<GrGpuBuffer> buffer,
                                 const void* data,
                                 size_t offset,
                                 size_t size) {
    if (!dc || dc != buffer->getContext()) {
        return false;
    }
    SkASSERT(!dc->abandoned()); // If dc is abandoned then buffer->getContext() should be null.

    if (!dc->priv().caps()->transferFromBufferToBufferSupport()) {
        auto ownedData = SkData::MakeWithCopy(data, size);
        dc->priv().drawingManager()->newBufferUpdateTask(std::move(ownedData),
                                                         std::move(buffer),
                                                         offset);
        return true;
    }

    sk_sp<GrGpuBuffer> tempBuffer;
    size_t tempOffset = 0;
    if (auto* sbm = dc->priv().getGpu()->stagingBufferManager()) {
        auto alignment = dc->priv().caps()->transferFromBufferToBufferAlignment();
        auto [sliceBuffer, sliceOffset, ptr] = sbm->allocateStagingBufferSlice(size, alignment);
        if (sliceBuffer) {
            std::memcpy(ptr, data, size);
            tempBuffer.reset(SkRef(sliceBuffer));
            tempOffset = sliceOffset;
        }
    }

    if (!tempBuffer) {
        tempBuffer = dc->priv().resourceProvider()->createBuffer(size,
                                                                 GrGpuBufferType::kXferCpuToGpu,
                                                                 kDynamic_GrAccessPattern,
                                                                 GrResourceProvider::ZeroInit::kNo);
        if (!tempBuffer) {
            return false;
        }
        if (!tempBuffer->updateData(data, 0, size, /*preserve=*/false)) {
            return false;
        }
    }

    dc->priv().drawingManager()->newBufferTransferTask(std::move(tempBuffer),
                                                       tempOffset,
                                                       std::move(buffer),
                                                       offset,
                                                       size);

    return true;
}
#endif  // SK_SUPPORT_GPU

#endif  // SK_ENABLE_SKSL
