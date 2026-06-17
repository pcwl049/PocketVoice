#include "text_postprocess.h"

namespace stt {
namespace {

bool replaceAll(std::string& text, const std::string& from, const std::string& to) {
    bool changed = false;
    size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::string::npos) {
        text.replace(pos, from.size(), to);
        pos += to.size();
        changed = true;
    }
    return changed;
}

void applyRule(std::string& text, std::vector<std::string>& rules, const std::string& rule, const std::string& from, const std::string& to) {
    if (replaceAll(text, from, to)) {
        rules.push_back(rule);
    }
}

bool containsAny(const std::string& text, const std::vector<std::string>& needles) {
    for (const auto& needle : needles) {
        if (text.find(needle) != std::string::npos) return true;
    }
    return false;
}

bool hasVrchatContext(const std::string& text) {
    return containsAny(text, {
        "VRChat",
        "Quest",
        "Booth",
        "镜子房",
        "全身追踪",
        "半身追踪",
        "动捕",
        "世界加载",
    });
}

}  // namespace

PostprocessResult postprocessRecognizedText(const std::string& text) {
    PostprocessResult result;
    result.text = text;

    applyRule(result.text, result.appliedRules, "acronym.qnn", "Q N N", "QNN");
    applyRule(result.text, result.appliedRules, "acronym.qnn", "q n n", "QNN");
    applyRule(result.text, result.appliedRules, "acronym.htp", "H T P", "HTP");
    applyRule(result.text, result.appliedRules, "acronym.htp", "h t p", "HTP");
    applyRule(result.text, result.appliedRules, "acronym.awsl", "A W S L", "AWSL");
    applyRule(result.text, result.appliedRules, "acronym.awsl", "a w s l", "AWSL");
    applyRule(result.text, result.appliedRules, "acronym.yyds", "y y d s", "yyds");
    applyRule(result.text, result.appliedRules, "acronym.yyds", "Y Y D S", "yyds");
    applyRule(result.text, result.appliedRules, "acronym.sos", "O S O S", "SOS");
    applyRule(result.text, result.appliedRules, "acronym.sos", "OS，OS", "SOS");
    applyRule(result.text, result.appliedRules, "acronym.sos", "OS,OS", "SOS");

    applyRule(result.text, result.appliedRules, "vrchat.compact", "VR Chat", "VRChat");
    applyRule(result.text, result.appliedRules, "vrchat.compact", "V R Chat", "VRChat");

    applyRule(result.text, result.appliedRules, "genshin.domain", "元神牛逼", "原神牛逼");

    if (hasVrchatContext(result.text)) {
        applyRule(result.text, result.appliedRules, "vrchat_context.single_player", "单击进VRChat", "单机进VRChat");
        applyRule(result.text, result.appliedRules, "vrchat_context.single_player", "单击进 VRChat", "单机进VRChat");
        applyRule(result.text, result.appliedRules, "vrchat_context.avatar_change", "换个皮再进来", "换皮再进来");
    }

    return result;
}

}  // namespace stt
