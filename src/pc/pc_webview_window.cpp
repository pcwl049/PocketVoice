#include "pc_webview_window.h"
#include "pc_logger.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wrl.h>
#include <WebView2.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>

namespace stt {

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

struct PcWebViewWindow::Impl {
    std::thread uiThread;
    std::atomic<bool> running{false};
    std::atomic<bool> stopping{false};
    std::mutex hwndMutex;
    std::mutex callbackMutex;
    std::function<void()> closeCallback;
    HWND hwnd = nullptr;
};

namespace {

constexpr wchar_t kWindowClassName[] = L"PocketVoiceWebViewWindow";

struct WindowState {
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webView;
    std::wstring url;
    HWND hwnd = nullptr;
    std::atomic<bool>* stopping = nullptr;
    std::function<void()> closeCallback;
};

static void resizeWebView(HWND hwnd) {
    auto* state = reinterpret_cast<WindowState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!state || !state->controller) return;

    RECT bounds{};
    GetClientRect(hwnd, &bounds);
    state->controller->put_Bounds(bounds);
}

static LRESULT CALLBACK webViewWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_NCCREATE: {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }
        case WM_SIZE:
            resizeWebView(hwnd);
            return 0;
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize.x = 900;
            info->ptMinTrackSize.y = 620;
            return 0;
        }
        case WM_DESTROY:
            if (auto* state = reinterpret_cast<WindowState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA))) {
                const bool stopping = state->stopping && state->stopping->load();
                if (!stopping && state->closeCallback) {
                    state->closeCallback();
                }
            }
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

static bool registerWindowClass(HINSTANCE instance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = webViewWndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kWindowClassName;

    if (RegisterClassExW(&wc)) return true;
    return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

static void initializeWebView(HWND hwnd, WindowState* state) {
    state->hwnd = hwnd;
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr,
        nullptr,
        nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [hwnd, state](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
                if (FAILED(result) || !environment) {
                    pcLogf(PcLogLevel::Error, "UI", "WebView2 environment failed: 0x%08lx", static_cast<unsigned long>(result));
                    return S_OK;
                }

                environment->CreateCoreWebView2Controller(
                    hwnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [hwnd, state](HRESULT controllerResult, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(controllerResult) || !controller) {
                                pcLogf(PcLogLevel::Error, "UI", "WebView2 controller failed: 0x%08lx", static_cast<unsigned long>(controllerResult));
                                return S_OK;
                            }

                            state->controller = controller;
                            state->controller->get_CoreWebView2(&state->webView);
                            resizeWebView(hwnd);
                            if (state->webView) {
                                EventRegistrationToken token{};
                                state->webView->add_WebMessageReceived(
                                    Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                        [state](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                            LPWSTR rawMessage = nullptr;
                                            if (FAILED(args->TryGetWebMessageAsString(&rawMessage)) || !rawMessage) {
                                                return S_OK;
                                            }
                                            std::wstring message(rawMessage);
                                            CoTaskMemFree(rawMessage);

                                            if (message.find(L"window.minimize") != std::wstring::npos) {
                                                ShowWindow(state->hwnd, SW_MINIMIZE);
                                            } else if (message.find(L"window.close") != std::wstring::npos) {
                                                PostMessageW(state->hwnd, WM_CLOSE, 0, 0);
                                            } else if (message.find(L"window.drag") != std::wstring::npos) {
                                                ReleaseCapture();
                                                SendMessageW(state->hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
                                            }
                                            return S_OK;
                                        }).Get(),
                                    &token);
                                const std::wstring& url = state->url;
                                state->webView->Navigate(url.c_str());
                            }
                            return S_OK;
                        }).Get());
                return S_OK;
            }).Get());

    if (FAILED(hr)) {
        pcLogf(PcLogLevel::Error, "UI", "CreateCoreWebView2EnvironmentWithOptions failed: 0x%08lx", static_cast<unsigned long>(hr));
    }
}

} // namespace

PcWebViewWindow::PcWebViewWindow() : m_impl(std::make_unique<Impl>()) {}

PcWebViewWindow::~PcWebViewWindow() {
    stop();
}

bool PcWebViewWindow::startDetached(const std::wstring& url) {
    if (m_impl->running.exchange(true)) return true;
    if (m_impl->uiThread.joinable()) {
        m_impl->uiThread.join();
    }

    try {
        m_impl->uiThread = std::thread(&PcWebViewWindow::runMessageLoop, this, url);
        return true;
    } catch (...) {
        m_impl->running = false;
        return false;
    }
}

void PcWebViewWindow::show(const std::wstring& url) {
    if (m_impl->running.exchange(true)) return;
    runMessageLoop(url);
}

void PcWebViewWindow::stop() {
    bool wasRunning = m_impl->running.exchange(false);
    m_impl->stopping = true;

    HWND hwnd = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_impl->hwndMutex);
        hwnd = m_impl->hwnd;
    }
    if (wasRunning && hwnd) {
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
    }
    if (m_impl->uiThread.joinable()) {
        m_impl->uiThread.join();
    }
    m_impl->stopping = false;
}

bool PcWebViewWindow::isRunning() const {
    return m_impl->running;
}

void PcWebViewWindow::setCloseCallback(std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(m_impl->callbackMutex);
    m_impl->closeCallback = std::move(callback);
}

void PcWebViewWindow::runMessageLoop(const std::wstring& url) {
    HRESULT coInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(coInit)) {
        pcLogf(PcLogLevel::Error, "UI", "CoInitializeEx failed: 0x%08lx", static_cast<unsigned long>(coInit));
        m_impl->running = false;
        return;
    }

    HINSTANCE instance = GetModuleHandleW(nullptr);
    if (!registerWindowClass(instance)) {
        pcLog(PcLogLevel::Error, "UI", "Failed to register WebView window class");
        CoUninitialize();
        m_impl->running = false;
        return;
    }

    WindowState state;
    state.url = url;
    state.stopping = &m_impl->stopping;
    {
        std::lock_guard<std::mutex> lock(m_impl->callbackMutex);
        state.closeCallback = m_impl->closeCallback;
    }

    HWND hwnd = CreateWindowExW(
        WS_EX_APPWINDOW,
        kWindowClassName,
        L"PocketVoice",
        WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        1120,
        760,
        nullptr,
        nullptr,
        instance,
        &state);

    if (!hwnd) {
        pcLog(PcLogLevel::Error, "UI", "Failed to create WebView window");
        CoUninitialize();
        m_impl->running = false;
        return;
    }
    pcLog(PcLogLevel::Info, "UI", "WebView window created");

    {
        std::lock_guard<std::mutex> lock(m_impl->hwndMutex);
        m_impl->hwnd = hwnd;
    }

    ShowWindow(hwnd, SW_SHOWNORMAL);
    SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    UpdateWindow(hwnd);
    initializeWebView(hwnd, &state);

    if (!m_impl->running) {
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (state.controller) {
        state.controller->Close();
        state.controller.Reset();
    }
    state.webView.Reset();

    if (IsWindow(hwnd)) {
        DestroyWindow(hwnd);
    }

    {
        std::lock_guard<std::mutex> lock(m_impl->hwndMutex);
        m_impl->hwnd = nullptr;
    }
    m_impl->running = false;
    pcLog(PcLogLevel::Info, "UI", "WebView window closed");
    CoUninitialize();
}

}
