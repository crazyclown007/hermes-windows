/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/**
 * This is a driver of the Hermes Compiler intended to be compiled to
 * WebAssembly with Emscripten and invoked from JavaScript.
 *
 * When configuring CMake, don't specify CMAKE_EXE_LINKER_FLAGS, because the
 * correct flags are already set for this target.
 *
 * HermesCompiler.js is a module exposing the compiler interface to JS.
 */

#include "hermes/AST/SemValidate.h"
#include "hermes/BCGen/HBC/HBC.h"
#include "hermes/SourceMap/SourceMapParser.h"
#include "hermes/Support/Algorithms.h"
#include "hermes/Support/SimpleDiagHandler.h"

#include "llvh/Support/SHA1.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

using namespace hermes;

// Forward declaration.
class CompileResult;

extern "C" {
/// Compile the supplied source and return a CompileResult, which is an opaque
/// structure containing the generated bytecode or an error message. The
/// result must be freed with \c hermesCompileResult_free().
///
/// \param source utf-8 encoded input string. It must be zero terminated.
/// \param sourceSize the length of \c source in bytes, including the
///     terminating zero.
/// \param sourceURL optional string containing the source URL.
/// \param sourceMapData optional string containing a source map.
/// \param sourceMapSize the length if \c sourceMapData, including nul
/// \return a new instance of CompileResult.
CompileResult *hermesCompileToBytecode(
    const char *source,
    size_t sourceSize,
    const char *sourceURL,
    const char *sourceMapData,
    size_t sourceMapSize);

/// Free the CompileResult allocated by \c hermesCompileToBytecode().
void hermesCompileResult_free(CompileResult *res);

/// \return nullptr if compilation was successfull, the error string otherwise.
const char *hermesCompileResult_getError(const CompileResult *res);

/// \return a pointer to the generated bytecode, or nullptr if there was an
///     error.
const char *hermesCompileResult_getBytecodeAddr(const CompileResult *res);

/// \return the size of the generated bytecode, or 0 if there was an error.
size_t hermesCompileResult_getBytecodeSize(const CompileResult *res);

/// \return a JSON string encoding constant Hermes properties.
EMSCRIPTEN_KEEPALIVE
extern "C" const char *hermesGetProperties();

} // extern "C"

/// An opaque object containing the result of a compilation.
class CompileResult {
 public:
  std::string error_;
  llvh::SmallVector<char, 0> bytecode_;
};

EMSCRIPTEN_KEEPALIVE
extern "C" void hermesCompileResult_free(CompileResult *res) {
  delete res;
}

EMSCRIPTEN_KEEPALIVE
extern "C" const char *hermesCompileResult_getError(const CompileResult *res) {
  if (!res || res->error_.empty())
    return nullptr;
  return res->error_.c_str();
}

EMSCRIPTEN_KEEPALIVE
extern "C" const char *hermesCompileResult_getBytecodeAddr(
    const CompileResult *res) {
  if (!res || res->bytecode_.empty())
    return nullptr;
  return res->bytecode_.data();
}

EMSCRIPTEN_KEEPALIVE
extern "C" size_t hermesCompileResult_getBytecodeSize(
    const CompileResult *res) {
  if (!res || res->bytecode_.empty())
    return 0;
  return res->bytecode_.size();
}

EMSCRIPTEN_KEEPALIVE
extern "C" CompileResult *hermesCompileToBytecode(
    const char *source,
    size_t sourceSize,
    const char *sourceURL,
    const char *sourceMapData,
    size_t sourceMapSize) {
  auto compileRes = std::make_unique<CompileResult>();
  std::unique_ptr<SourceMap> sourceMap;

  if (source[sourceSize - 1] != 0) {
    compileRes->error_ = "Input source must be zero-terminated";
    return compileRes.release();
  }

  if (sourceMapData != nullptr && sourceMapData[0] != '\0') {
    if (sourceMapData[sourceMapSize - 1] != 0) {
      compileRes->error_ = "Input sourcemap must be zero-terminated";
      return compileRes.release();
    }

    SourceErrorManager sm;
    SimpleDiagHandlerRAII diagHandler(sm);
    sourceMap = SourceMapParser::parse({sourceMapData, sourceMapSize - 1}, sm);
    if (!sourceMap) {
      compileRes->error_ =
          "Failed to parse source map:" + diagHandler.getErrorString();
      return compileRes.release();
    }
  }

  hbc::CompileFlags flags{};
  flags.debug = true;

  // Note that we are relying the zero termination provided by str.data(),
  // because the parser requires it.
  auto res = hbc::BCProviderFromSrc::createBCProviderFromSrc(
      std::make_unique<hermes::Buffer>((const uint8_t *)source, sourceSize - 1),
      sourceURL ? sourceURL : "",
      std::move(sourceMap),
      flags);
  if (!res.first) {
    if (!res.second.empty())
      compileRes->error_ = res.second;
    else
      compileRes->error_ = "Unknown compilation error";
    return compileRes.release();
  }

  llvh::raw_svector_ostream bcstream{compileRes->bytecode_};

  BytecodeGenerationOptions opts(::hermes::EmitBundle);
  opts.optimizationEnabled = false;

  hbc::BytecodeSerializer BS{bcstream, opts};
  BS.serialize(
      *res.first->getBytecodeModule(),
      llvh::SHA1::hash(llvh::makeArrayRef(
          reinterpret_cast<const uint8_t *>(source), sourceSize - 1)));

  return compileRes.release();
}

static std::string getPropertiesHelper() {
  std::string json{};
  llvh::raw_string_ostream OS{json};
  OS << "{ \"BYTECODE_ALIGNMENT\":" << hbc::BYTECODE_ALIGNMENT
     << ", \"HEADER_SIZE\":" << sizeof(hbc::BytecodeFileHeader)
     << ", \"VERSION\":" << hbc::BYTECODE_VERSION << ", \"MAGIC\": ["
     << (uint32_t)hbc::MAGIC << ", " << (uint32_t)(hbc::MAGIC >> 32) << "]"
     << ", \"LENGTH_OFFSET\":" << offsetof(hbc::BytecodeFileHeader, fileLength)
     << "}";

  return OS.str();
}

EMSCRIPTEN_KEEPALIVE
extern "C" const char *hermesGetProperties() {
  static std::string props = getPropertiesHelper();
  return props.c_str();
}

// This is just a dummy main routine to exercise the code. It won't actually
// be called by JS.
int main() {
  const char map[] = R"(
      {
        "version": 3,
        "file": "x.js",
        "sourceRoot": "",
        "sources": [
          "test.js"
        ],
        "names": [],
        "mappings": "AAKA,SAAS,OAAO,CAAC,MAAc;IAC3B,OAAO,SAAS,GAAG,MAAM,CAAC,SAAS,GAAG,GAAG,GAAG,MAAM,CAAC,QAAQ,CAAC;AAChE,CAAC;AAED,IAAI,IAAI,GAAG,EAAE,SAAS,EAAE,MAAM,EAAE,QAAQ,EAAE,MAAM,EAAE,CAAC;AACnD,OAAO,CAAC,GAAG,CAAC,OAAO,CAAC,IAAI,CAAC,CAAC,CAAC"
      }
    )";
  static const char src1[] = "var x = 1; print(x);";
  auto *res1 =
      hermesCompileToBytecode(src1, sizeof(src1), "x.js", map, sizeof(map));
  assert(!hermesCompileResult_getError(res1) && "success expected");
  llvh::outs() << "Generated " << hermesCompileResult_getBytecodeSize(res1)
               << " bytecode bytes\n";
  hermesCompileResult_free(res1);

  static const char src2[] = "var x = 1 + ;";
  auto *res2 =
      hermesCompileToBytecode(src2, sizeof(src2), "x.js", map, sizeof(map));
  assert(hermesCompileResult_getError(res2) && "error expected");
  llvh::outs() << "Error " << hermesCompileResult_getError(res2) << "\n";
  hermesCompileResult_free(res2);

  return 0;
}
