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
// The whole model for tools that read no schema text, such as the docs site.
std::vector<Output> write_json(const Model &model);

}  // namespace codegen
