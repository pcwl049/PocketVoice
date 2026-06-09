#pragma once

#include <functional>
#include <memory>
#include <string>

namespace stt {

class PcWebViewWindow {
public:
    PcWebViewWindow();
    ~PcWebViewWindow();

    PcWebViewWindow(const PcWebViewWindow&) = delete;
    PcWebViewWindow& operator=(const PcWebViewWindow&) = delete;

    bool startDetached(const std::wstring& url);
    void show(const std::wstring& url);
    void stop();
    bool isRunning() const;
    void setCloseCallback(std::function<void()> callback);

private:
    void runMessageLoop(const std::wstring& url);

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}
