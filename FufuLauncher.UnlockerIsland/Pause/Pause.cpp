/*
Copyright (c) FufuLauncher Dev Team. All rights reserved.
Licensed under the AGPL-3.0 License.
*/
#include "Pause.h"
#include "../Config/Config.h"
#include "../Core/SharedState.h"
#include "../Core/Utils.h"
#pragma pack(pop) // Balance Il2CppObject.h's legacy pack(push, 4).
#include "../Patterns/Patterns.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <string>

#pragma comment(lib, "gdi32.lib")

namespace {
    constexpr wchar_t kOverlayClassName[] = L"FufuPauseOverlay";
    constexpr ULONGLONG kPageAppearTimeoutMs = 2500;
    constexpr ULONGLONG kPageCheckIntervalMs = 50;
    constexpr ULONGLONG kUnknownPageCloseTimeoutMs = 1500;
    constexpr ULONGLONG kExitReplayDelayMs = 150;

    constexpr size_t kMaximumPauseExitKeys = 32;

    // PAUSE label appearance. Smaller divisors move/enlarge the label.
    constexpr int kPauseVerticalDivisor = 6;
    constexpr int kPauseMinimumTop = 40;
    constexpr int kPauseFontDivisor = 12;
    constexpr int kPauseMinimumFontSize = 32;
    constexpr int kPauseMaximumFontSize = 64;
    constexpr COLORREF kPauseTextColor = RGB(255, 255, 255);
    constexpr COLORREF kPauseShadowColor = RGB(35, 35, 35);
    std::atomic<bool> g_active{ false };
    std::atomic<bool> g_exitKeyWhitelistReloadPending{ true };
    std::array<int, kMaximumPauseExitKeys> g_pauseExitKeyWhitelist{};
    size_t g_pauseExitKeyCount = 0;
    HWND g_gameWindow = nullptr;
    HWND g_overlayWindow = nullptr;
    HBITMAP g_snapshot = nullptr;
    HFONT g_pauseFont = nullptr;
    int g_snapshotWidth = 0;
    int g_snapshotHeight = 0;
    POINT g_clientOrigin{};
    bool g_prepared = false;
    bool g_pageObserved = false;
    unsigned int g_exitKeysDownMask = 0;
    bool g_altWasDown = false;
    bool g_overlayVisible = false;
    bool g_gameFontLoaded = false;
    ULONGLONG g_activatedAt = 0;
    ULONGLONG g_lastPageCheck = 0;
    ULONGLONG g_closeRequestedAt = 0;
    bool g_closeSignalSent = false;

    bool IsAltDown() {
        return (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    }

    HCURSOR GetSystemNormalCursor() {
        static HCURSOR cursor = static_cast<HCURSOR>(LoadImageW(
            nullptr, IDC_ARROW, IMAGE_CURSOR, 0, 0,
            LR_SHARED | LR_DEFAULTSIZE));
        return cursor;
    }

    void ApplyPauseCursor(bool altDown) {
        SetCursor(altDown ? GetSystemNormalCursor() : nullptr);
    }

    bool RefreshPauseExitKeyWhitelist() {
        if (!g_exitKeyWhitelistReloadPending.exchange(
                false, std::memory_order_acq_rel)) {
            return false;
        }

        g_pauseExitKeyWhitelist.fill(0);
        g_pauseExitKeyCount = 0;

        char configuredKeysBuffer[256]{};
        const std::string configPath = Config::GetConfigPath();
        GetPrivateProfileStringA(
            "PauseExitKeys", "Value", "27,192", configuredKeysBuffer,
            static_cast<DWORD>(sizeof(configuredKeysBuffer)),
            configPath.c_str());
        std::string configuredKeys(configuredKeysBuffer);
        std::replace(configuredKeys.begin(), configuredKeys.end(), ';', ',');
        std::stringstream stream(configuredKeys);
        std::string token;

        while (std::getline(stream, token, ',') &&
               g_pauseExitKeyCount < kMaximumPauseExitKeys) {
            char* end = nullptr;
            const long parsed = std::strtol(token.c_str(), &end, 0);
            while (end && *end != '\0' &&
                   std::isspace(static_cast<unsigned char>(*end))) {
                ++end;
            }
            if (end == token.c_str() || (end && *end != '\0') ||
                parsed <= 0 || parsed > 255) {
                continue;
            }

            const int key = static_cast<int>(parsed);
            bool duplicate = false;
            for (size_t index = 0; index < g_pauseExitKeyCount; ++index) {
                if (g_pauseExitKeyWhitelist[index] == key) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) {
                g_pauseExitKeyWhitelist[g_pauseExitKeyCount++] = key;
            }
        }

        // Never allow a malformed/empty hot reload to create an inescapable
        // pause screen.
        if (g_pauseExitKeyCount == 0) {
            g_pauseExitKeyWhitelist[g_pauseExitKeyCount++] = VK_ESCAPE;
        }
        return true;
    }

    unsigned int GetPauseExitKeyMask() {
        unsigned int mask = 0;
        for (size_t index = 0; index < g_pauseExitKeyCount; ++index) {
            const int key = g_pauseExitKeyWhitelist[index];
            if (key > 0 && (GetAsyncKeyState(key) & 0x8000) != 0) {
                mask |= 1u << static_cast<unsigned int>(index);
            }
        }
        return mask;
    }

    void ReplayEscapeToGame() {
        if (!g_gameWindow) return;

        const UINT scanCode = MapVirtualKeyW(VK_ESCAPE, MAPVK_VK_TO_VSC);
        const LPARAM keyDown = 1 | (static_cast<LPARAM>(scanCode) << 16);
        const LPARAM keyUp = keyDown | (static_cast<LPARAM>(1) << 30) |
                             (static_cast<LPARAM>(1) << 31);
        PostMessageW(g_gameWindow, WM_KEYDOWN, VK_ESCAPE, keyDown);
        PostMessageW(g_gameWindow, WM_KEYUP, VK_ESCAPE, keyUp);
    }

    void DeleteRenderResources() {
        if (g_pauseFont) {
            DeleteObject(g_pauseFont);
            g_pauseFont = nullptr;
        }
        if (g_snapshot) {
            DeleteObject(g_snapshot);
            g_snapshot = nullptr;
        }
        g_snapshotWidth = 0;
        g_snapshotHeight = 0;
        g_prepared = false;
    }

    void LoadGameFontResource() {
        if (g_gameFontLoaded) return;

        wchar_t executablePath[MAX_PATH]{};
        if (!GetModuleFileNameW(nullptr, executablePath, MAX_PATH)) return;

        std::wstring executable(executablePath);
        const size_t slash = executable.find_last_of(L"\\/");
        if (slash == std::wstring::npos) return;

        const size_t dot = executable.find_last_of(L'.');
        if (dot == std::wstring::npos || dot <= slash) return;

        std::wstring fontPath = executable.substr(0, slash + 1);
        fontPath += executable.substr(slash + 1, dot - slash - 1);
        fontPath += L"_Data\\StreamingAssets\\MiHoYoSDKRes\\"
                    L"HttpServerResources\\font\\zh-cn.ttf";
        g_gameFontLoaded = AddFontResourceExW(
            fontPath.c_str(), FR_PRIVATE, nullptr) > 0;
    }

    void CreatePauseFont() {
        if (g_pauseFont) DeleteObject(g_pauseFont);
        LoadGameFontResource();

        const int height = std::clamp(
            g_snapshotHeight / kPauseFontDivisor,
            kPauseMinimumFontSize,
            kPauseMaximumFontSize);
        g_pauseFont = CreateFontW(
            -height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            g_gameFontLoaded ? L"SDK_SC_Web" : L"Microsoft YaHei UI");
    }

    LRESULT CALLBACK OverlayWndProc(
        HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_NCHITTEST:
            return HTCLIENT;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_SETCURSOR:
            ApplyPauseCursor(IsAltDown());
            return TRUE;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_MBUTTONDBLCLK:
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
        case WM_XBUTTONDBLCLK:
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC target = BeginPaint(window, &paint);
            if (target) {
                if (g_snapshot) {
                    HDC source = CreateCompatibleDC(target);
                    if (source) {
                        HGDIOBJ oldBitmap = SelectObject(source, g_snapshot);
                        BitBlt(target, 0, 0, g_snapshotWidth, g_snapshotHeight,
                               source, 0, 0, SRCCOPY);
                        SelectObject(source, oldBitmap);
                        DeleteDC(source);
                    }
                }

                if (g_pauseFont) {
                    HGDIOBJ oldFont = SelectObject(target, g_pauseFont);
                    SetBkMode(target, TRANSPARENT);

                    const int calculatedTop =
                        g_snapshotHeight / kPauseVerticalDivisor;
                    const int top = calculatedTop > kPauseMinimumTop
                        ? calculatedTop
                        : kPauseMinimumTop;
                    RECT shadow{ 2, top + 2, g_snapshotWidth + 2, top + 100 };
                    SetTextColor(target, kPauseShadowColor);
                    DrawTextW(target, L"PAUSE", -1, &shadow,
                              DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);

                    RECT text{ 0, top, g_snapshotWidth, top + 100 };
                    SetTextColor(target, kPauseTextColor);
                    DrawTextW(target, L"PAUSE", -1, &text,
                              DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);
                    SelectObject(target, oldFont);
                }
            }
            EndPaint(window, &paint);
            return 0;
        }
        default:
            return DefWindowProcW(window, message, wParam, lParam);
        }
    }

    bool RegisterOverlayClass() {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = OverlayWndProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.lpszClassName = kOverlayClassName;

        if (RegisterClassExW(&windowClass)) return true;
        return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }

    bool CaptureGameClient(HWND gameWindow) {
        RECT client{};
        if (!GetClientRect(gameWindow, &client)) return false;

        const int width = client.right - client.left;
        const int height = client.bottom - client.top;
        if (width <= 0 || height <= 0) return false;

        POINT origin{ 0, 0 };
        if (!ClientToScreen(gameWindow, &origin)) return false;

        HDC screen = GetDC(nullptr);
        if (!screen) return false;

        HDC memory = CreateCompatibleDC(screen);
        HBITMAP snapshot = memory
            ? CreateCompatibleBitmap(screen, width, height)
            : nullptr;
        bool captured = false;

        if (memory && snapshot) {
            HGDIOBJ oldBitmap = SelectObject(memory, snapshot);
            captured = BitBlt(memory, 0, 0, width, height, screen,
                              origin.x, origin.y, SRCCOPY | CAPTUREBLT) != FALSE;
            SelectObject(memory, oldBitmap);
        }

        if (memory) DeleteDC(memory);
        ReleaseDC(nullptr, screen);

        if (!captured) {
            if (snapshot) DeleteObject(snapshot);
            return false;
        }

        g_snapshot = snapshot;
        g_snapshotWidth = width;
        g_snapshotHeight = height;
        g_clientOrigin = origin;
        return true;
    }

    bool GetSynthesisPageState(bool& exists, bool& active) {
        exists = false;
        active = false;

        auto findString = (tFindString)p_FindString.load();
        auto findGameObject = (tFindGameObject)p_FindGameObject.load();
        auto getActive = (tGetActive)p_GetActive.load();
        if (!IsValid(findString) || !IsValid(findGameObject)) return false;

        const char* candidates[] = {
            GameStrings::SynthesisLayerPath,
            GameStrings::SynthesisPage
        };

        SafeInvoke([&] {
            for (const char* candidate : candidates) {
                Il2CppString* name = findString(candidate);
                if (!name) continue;

                void* object = findGameObject(name);
                if (!object) continue;

                exists = true;
                active = !IsValid(getActive) || getActive(object);
                return;
            }
        });
        return true;
    }

    bool IsGameForeground() {
        HWND foreground = GetForegroundWindow();
        if (!foreground || !g_gameWindow) return false;

        DWORD foregroundProcess = 0;
        DWORD gameProcess = 0;
        GetWindowThreadProcessId(foreground, &foregroundProcess);
        GetWindowThreadProcessId(g_gameWindow, &gameProcess);
        return foregroundProcess != 0 && foregroundProcess == gameProcess;
    }
}

namespace Pause {
    bool Prepare(HWND gameWindow) {
        End();
        if (!gameWindow || !IsWindow(gameWindow)) return false;

        DWORD processId = 0;
        GetWindowThreadProcessId(gameWindow, &processId);
        if (processId != GetCurrentProcessId()) return false;

        g_gameWindow = gameWindow;
        if (!CaptureGameClient(gameWindow)) {
            g_gameWindow = nullptr;
            return false;
        }

        g_prepared = true;
        return true;
    }

    bool Activate() {
        if (!g_prepared || !g_snapshot || !g_gameWindow) return false;
        RefreshPauseExitKeyWhitelist();
        if (!RegisterOverlayClass()) {
            Cancel();
            return false;
        }

        CreatePauseFont();
        g_overlayWindow = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            kOverlayClassName, L"", WS_POPUP,
            g_clientOrigin.x, g_clientOrigin.y,
            g_snapshotWidth, g_snapshotHeight,
            g_gameWindow, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!g_overlayWindow) {
            Cancel();
            return false;
        }

        SetWindowPos(g_overlayWindow, HWND_TOP,
                     g_clientOrigin.x, g_clientOrigin.y,
                     g_snapshotWidth, g_snapshotHeight,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        InvalidateRect(g_overlayWindow, nullptr, FALSE);
        UpdateWindow(g_overlayWindow);

        g_pageObserved = false;
        g_exitKeysDownMask = GetPauseExitKeyMask();
        g_altWasDown = IsAltDown();
        g_overlayVisible = true;
        g_activatedAt = GetTickCount64();
        g_lastPageCheck = 0;
        g_closeRequestedAt = 0;
        g_closeSignalSent = false;
        g_active.store(true, std::memory_order_release);
        ApplyPauseCursor(g_altWasDown);
        return true;
    }

    void Cancel() {
        End();
    }

    void End() {
        g_active.store(false, std::memory_order_release);
        if (g_overlayWindow) {
            DestroyWindow(g_overlayWindow);
            g_overlayWindow = nullptr;
        }
        DeleteRenderResources();
        g_gameWindow = nullptr;
        g_pageObserved = false;
        g_exitKeysDownMask = 0;
        g_altWasDown = false;
        g_overlayVisible = false;
        g_activatedAt = 0;
        g_lastPageCheck = 0;
        g_closeRequestedAt = 0;
        g_closeSignalSent = false;
    }

    void Update() {
        const bool whitelistChanged = RefreshPauseExitKeyWhitelist();
        if (!g_active.load(std::memory_order_acquire)) return;

        if (whitelistChanged) {
            g_exitKeysDownMask = GetPauseExitKeyMask();
        }

        const bool foreground = IsGameForeground();
        if (g_overlayWindow && foreground != g_overlayVisible) {
            ShowWindow(g_overlayWindow,
                       foreground ? SW_SHOWNOACTIVATE : SW_HIDE);
            g_overlayVisible = foreground;
        }

        const ULONGLONG now = GetTickCount64();
        const bool altDown = IsAltDown();
        if (altDown != g_altWasDown) {
            g_altWasDown = altDown;
            ApplyPauseCursor(altDown);
        }

        const unsigned int exitKeysDownMask = GetPauseExitKeyMask();
        if ((exitKeysDownMask & ~g_exitKeysDownMask) != 0) {
            g_closeRequestedAt = now;
            g_closeSignalSent = false;
        }
        g_exitKeysDownMask = exitKeysDownMask;

        if (g_lastPageCheck != 0 &&
            now - g_lastPageCheck < kPageCheckIntervalMs) {
            return;
        }
        g_lastPageCheck = now;

        bool exists = false;
        bool pageActive = false;
        const bool stateAvailable =
            GetSynthesisPageState(exists, pageActive);
        if (exists && pageActive) g_pageObserved = true;

        if (g_pageObserved && stateAvailable && (!exists || !pageActive)) {
            End();
            return;
        }

        if (!g_pageObserved && now - g_activatedAt > kPageAppearTimeoutMs) {
            End();
            return;
        }

        if (g_closeRequestedAt != 0) {
            // Keep the cover and the active state until the real page has
            // finished closing. This prevents the next hotkey request from
            // reopening the still-live SynthesisPage as a visible page.
            if (g_pageObserved && stateAvailable) {
                if (!g_closeSignalSent &&
                    now - g_closeRequestedAt >= kExitReplayDelayMs) {
                    ReplayEscapeToGame();
                    g_closeSignalSent = true;
                }
                return;
            }

            if (now - g_closeRequestedAt > kUnknownPageCloseTimeoutMs) {
                End();
            }
        }
    }

    bool IsActive() {
        return g_active.load(std::memory_order_acquire);
    }

    void NotifyConfigReload() {
        g_exitKeyWhitelistReloadPending.store(true, std::memory_order_release);
    }
}
