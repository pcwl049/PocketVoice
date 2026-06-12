#include "text_postprocess.h"

#include <cassert>
#include <cstdio>
#include <string>

static void assertText(const std::string& input, const std::string& expected) {
    auto result = stt::postprocessRecognizedText(input);
    assert(result.text == expected);
}

static bool hasRule(const stt::PostprocessResult& result, const std::string& rule) {
    for (const auto& item : result.appliedRules) {
        if (item == rule) return true;
    }
    return false;
}

int main() {
    assertText("这个版本先跑 Q N N H T P，后面再看端侧推理。", "这个版本先跑 QNN HTP，后面再看端侧推理。");
    assertText("爷青回，元神牛逼。A W S L。", "爷青回，原神牛逼。AWSL。");
    assertText("y y d s这个词现在还有人说吗？", "yyds这个词现在还有人说吗？");
    assertText("Quest单击进VR Chat Booth上买的模型，还没改。", "Quest单机进VRChat Booth上买的模型，还没改。");
    assertText("我单击按钮打开设置。", "我单击按钮打开设置。");
    assertText("这个世界加载有点慢，我换个皮再进来。", "这个世界加载有点慢，我换皮再进来。");
    assertText("这是第一种，第二种叫呃与O S O S，什么意思啊？", "这是第一种，第二种叫呃与SOS，什么意思啊？");

    auto traced = stt::postprocessRecognizedText("Quest单击进VR Chat Booth上买的模型，还没改。");
    assert(hasRule(traced, "vrchat.compact"));
    assert(hasRule(traced, "vrchat_context.single_player"));

    puts("text_postprocess tests passed");
    return 0;
}
