// The files each backend writes, by their path under the repo root.
#pragma once

#include "model.hpp"

#include <string>
#include <vector>

namespace codegen {

struct Output {
    std::string path;
    std::string text;
};

// The C structs with extras pasted at their end, the roster enum and the compiled table.
std::vector<Output> write_c(const Model &model, const std::string &extras);
// The C++ structs inside extras at its marker, and the reflection rant.hpp splices in.
std::vector<Output> write_cpp(const Model &model, const std::string &extras);
// The C# structs and enums with extras pasted above them.
std::vector<Output> write_csharp(const Model &model, const std::string &extras);
// The Python classes and their stubs, each with its extras pasted after the generated part.
std::vector<Output> write_python(const Model &model, const std::string &extras, const std::string &stub_extras);
// The Rust types with extras pasted after them, and the middleware's Message impls of them.
std::vector<Output> write_rust(const Model &model, const std::string &extras);
// The whole model for tools that read no schema text, such as the docs site.
std::vector<Output> write_json(const Model &model);

}  // namespace codegen
