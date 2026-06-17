#pragma once

#include <string>
#include <vector>

namespace stt {

struct PostprocessResult {
    std::string text;
    std::vector<std::string> appliedRules;
};

PostprocessResult postprocessRecognizedText(const std::string& text);

}  // namespace stt
