/*
 * Copyright 2014 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "GrGLShaderBuilder.h"
#include "gl/builders/GrGLProgramBuilder.h"
#include "glsl/GrGLSLCaps.h"
#include "glsl/GrGLSLShaderVar.h"
#include "glsl/GrGLSLTextureSampler.h"

static void map_swizzle(const char* swizzleMap, const char* swizzle, char* mangledSwizzle) {
    int i;
    for (i = 0; '\0' != swizzle[i]; ++i) {
        switch (swizzle[i]) {
            case 'r':
                mangledSwizzle[i] = swizzleMap[0];
                break;
            case 'g':
                mangledSwizzle[i] = swizzleMap[1];
                break;
            case 'b':
                mangledSwizzle[i] = swizzleMap[2];
                break;
            case 'a':
                mangledSwizzle[i] = swizzleMap[3];
                break;
            default:
                SkFAIL("Unsupported swizzle");
        }
    }
    mangledSwizzle[i] ='\0';
}

static void append_texture_lookup(SkString* out,
                                  const GrGLSLCaps* glslCaps,
                                  const char* samplerName,
                                  const char* coordName,
                                  GrPixelConfig config,
                                  const char* swizzle,
                                  GrSLType varyingType = kVec2f_GrSLType) {
    SkASSERT(coordName);

    out->appendf("%s(%s, %s)",
                 GrGLSLTexture2DFunctionName(varyingType, glslCaps->generation()),
                 samplerName,
                 coordName);

    char mangledSwizzle[5];

    // This refers to any swizzling we may need to get from some backend internal format to the
    // format used in GrPixelConfig. Some backends will automatically do the sizzling for us.
    if (glslCaps->mustSwizzleInShader()) {
        const char* swizzleMap = glslCaps->getSwizzleMap(config);
        // if the map is simply 'rgba' then we don't need to do any manual swizzling to get us to
        // a GrPixelConfig format.
        if (memcmp(swizzleMap, "rgba", 4)) {
            // Manually 'swizzle' the swizzle using our mapping
            map_swizzle(swizzleMap, swizzle, mangledSwizzle);
            swizzle = mangledSwizzle;
        }
    }

    // For shader prettiness we omit the swizzle rather than appending ".rgba".
    if (memcmp(swizzle, "rgba", 4)) {
        out->appendf(".%s", swizzle);
    }
}

GrGLShaderBuilder::GrGLShaderBuilder(GrGLProgramBuilder* program)
    : fProgramBuilder(program)
    , fInputs(GrGLProgramBuilder::kVarsPerBlock)
    , fOutputs(GrGLProgramBuilder::kVarsPerBlock)
    , fFeaturesAddedMask(0)
    , fCodeIndex(kCode)
    , fFinalized(false) {
    // We push back some dummy pointers which will later become our header
    for (int i = 0; i <= kCode; i++) {
        fShaderStrings.push_back();
        fCompilerStrings.push_back(nullptr);
        fCompilerStringLengths.push_back(0);
    }

    this->main() = "void main() {";
}

void GrGLShaderBuilder::declAppend(const GrGLSLShaderVar& var) {
    SkString tempDecl;
    var.appendDecl(fProgramBuilder->glslCaps(), &tempDecl);
    this->codeAppendf("%s;", tempDecl.c_str());
}

void GrGLShaderBuilder::emitFunction(GrSLType returnType,
                                     const char* name,
                                     int argCnt,
                                     const GrGLSLShaderVar* args,
                                     const char* body,
                                     SkString* outName) {
    this->functions().append(GrGLSLTypeString(returnType));
    fProgramBuilder->nameVariable(outName, '\0', name);
    this->functions().appendf(" %s", outName->c_str());
    this->functions().append("(");
    for (int i = 0; i < argCnt; ++i) {
        args[i].appendDecl(fProgramBuilder->glslCaps(), &this->functions());
        if (i < argCnt - 1) {
            this->functions().append(", ");
        }
    }
    this->functions().append(") {\n");
    this->functions().append(body);
    this->functions().append("}\n\n");
}

void GrGLShaderBuilder::appendTextureLookup(SkString* out,
                                            const GrGLSLTextureSampler& sampler,
                                            const char* coordName,
                                            GrSLType varyingType) const {
    append_texture_lookup(out,
                          fProgramBuilder->glslCaps(),
                          fProgramBuilder->getUniformCStr(sampler.fSamplerUniform),
                          coordName,
                          sampler.config(),
                          sampler.swizzle(),
                          varyingType);
}

void GrGLShaderBuilder::appendTextureLookup(const GrGLSLTextureSampler& sampler,
                                            const char* coordName,
                                            GrSLType varyingType) {
    this->appendTextureLookup(&this->code(), sampler, coordName, varyingType);
}

void GrGLShaderBuilder::appendTextureLookupAndModulate(const char* modulation,
                                                       const GrGLSLTextureSampler& sampler,
                                                       const char* coordName,
                                                       GrSLType varyingType) {
    SkString lookup;
    this->appendTextureLookup(&lookup, sampler, coordName, varyingType);
    this->codeAppend((GrGLSLExpr4(modulation) * GrGLSLExpr4(lookup)).c_str());
}

void GrGLShaderBuilder::addFeature(uint32_t featureBit, const char* extensionName) {
    if (!(featureBit & fFeaturesAddedMask)) {
        this->extensions().appendf("#extension %s: require\n", extensionName);
        fFeaturesAddedMask |= featureBit;
    }
}

void GrGLShaderBuilder::appendDecls(const VarArray& vars, SkString* out) const {
    for (int i = 0; i < vars.count(); ++i) {
        vars[i].appendDecl(fProgramBuilder->glslCaps(), out);
        out->append(";\n");
    }
}

void GrGLShaderBuilder::addLayoutQualifier(const char* param, InterfaceQualifier interface) {
    SkASSERT(fProgramBuilder->glslCaps()->generation() >= k330_GrGLSLGeneration ||
             fProgramBuilder->glslCaps()->mustEnableAdvBlendEqs());
    fLayoutParams[interface].push_back() = param;
}

void GrGLShaderBuilder::compileAndAppendLayoutQualifiers() {
    static const char* interfaceQualifierNames[] = {
        "out"
    };

    for (int interface = 0; interface <= kLastInterfaceQualifier; ++interface) {
        const SkTArray<SkString>& params = fLayoutParams[interface];
        if (params.empty()) {
            continue;
        }
        this->layoutQualifiers().appendf("layout(%s", params[0].c_str());
        for (int i = 1; i < params.count(); ++i) {
            this->layoutQualifiers().appendf(", %s", params[i].c_str());
        }
        this->layoutQualifiers().appendf(") %s;\n", interfaceQualifierNames[interface]);
    }

    GR_STATIC_ASSERT(0 == GrGLShaderBuilder::kOut_InterfaceQualifier);
    GR_STATIC_ASSERT(SK_ARRAY_COUNT(interfaceQualifierNames) == kLastInterfaceQualifier + 1);
}

void GrGLShaderBuilder::finalize(uint32_t visibility) {
    SkASSERT(!fFinalized);
    this->versionDecl() = fProgramBuilder->glslCaps()->versionDeclString();
    this->compileAndAppendLayoutQualifiers();
    SkASSERT(visibility);
    fProgramBuilder->appendUniformDecls((GrGLProgramBuilder::ShaderVisibility) visibility,
                                        &this->uniforms());
    this->appendDecls(fInputs, &this->inputs());
    // We should not have any outputs in the fragment shader when using version 1.10
    SkASSERT(GrGLProgramBuilder::kFragment_Visibility != visibility ||
             k110_GrGLSLGeneration != fProgramBuilder->glslCaps()->generation() ||
             fOutputs.empty());
    this->appendDecls(fOutputs, &this->outputs());
    this->onFinalize();
    // append the 'footer' to code
    this->code().append("}");

    for (int i = 0; i <= fCodeIndex; i++) {
        fCompilerStrings[i] = fShaderStrings[i].c_str();
        fCompilerStringLengths[i] = (int)fShaderStrings[i].size();
    }

    fFinalized = true;
}

