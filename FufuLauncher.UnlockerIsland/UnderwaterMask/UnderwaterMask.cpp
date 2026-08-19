/*
Copyright (c) FufuLauncher Dev Team. All rights reserved.
Licensed under the AGPL-3.0 License.
*/
#include "UnderwaterMask.h"

#include "../Config/Config.h"
#include "../Core/Utils.h"
#include "../MinHook/MinHook.h"
#include "../Patterns/Patterns.h"
#include "../Scanner/Scanner.h"

#include <atomic>
#include <cstdint>
#include <iostream>

namespace UnderwaterMask {
    namespace {
        using MaskFunction = std::int64_t(__fastcall*)(void*, double);
        using ClearFunction = void(__fastcall*)(void*);

        constexpr uintptr_t ClearSearchWindow = 0x8000;
        constexpr int MaxExceptionCount = 3;

        MaskFunction g_origPre = nullptr;
        MaskFunction g_origMain = nullptr;
        MaskFunction g_origPost = nullptr;
        ClearFunction g_clearMask = nullptr;

        std::atomic<int> g_exceptionCount{ 0 };
        std::atomic<bool> g_forceFallback{ false };

        bool IsDisabled() {
            if (g_forceFallback.load(std::memory_order_relaxed)) {
                return false;
            }
            return Config::Get().disable_underwater_mask;
        }

        void OnException(const char* stage) {
            int count = g_exceptionCount.fetch_add(1, std::memory_order_relaxed) + 1;
            std::cout << "[ERR] UnderwaterMask exception in " << stage
                      << " (count: " << count << "/" << MaxExceptionCount << ")" << std::endl;
            if (count >= MaxExceptionCount) {
                g_forceFallback.store(true, std::memory_order_relaxed);
                std::cout << "[WARN] UnderwaterMask disabled due to repeated exceptions, falling back to original." << std::endl;
            }
        }

        void InvokeClearMask(void* thisPtr) {
            if (!thisPtr || !IsValid(g_clearMask)) {
                return;
            }

            __try {
                g_clearMask(thisPtr);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                OnException("ClearMask");
            }
        }

        std::int64_t InvokeOriginal(MaskFunction original, void* thisPtr, double deltaTime) {
            if (!original) {
                return 0;
            }
            __try {
                return original(thisPtr, deltaTime);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                return 0;
            }
        }

        std::int64_t InvokeOrClear(MaskFunction original, void* thisPtr, double deltaTime, const char* stage) {
            if (!IsDisabled()) {
                return InvokeOriginal(original, thisPtr, deltaTime);
            }

            __try {
                InvokeClearMask(thisPtr);
                return 0;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                OnException(stage);
                return InvokeOriginal(original, thisPtr, deltaTime);
            }
        }

        std::int64_t __fastcall HookPre(void* thisPtr, double deltaTime) {
            return InvokeOrClear(g_origPre, thisPtr, deltaTime, "Pre");
        }

        std::int64_t __fastcall HookMain(void* thisPtr, double deltaTime) {
            return InvokeOrClear(g_origMain, thisPtr, deltaTime, "Main");
        }

        std::int64_t __fastcall HookPost(void* thisPtr, double deltaTime) {
            return InvokeOrClear(g_origPost, thisPtr, deltaTime, "Post");
        }

        void* ScanDirect(const char* name, const char* pattern) {
            std::cout << "[SCAN] " << name << "..." << std::endl;

            void* address = Scanner::ScanMainMod(pattern);
            if (!address) {
                std::cout << "   -> [ERR] Pattern Not Found." << std::endl;
                return nullptr;
            }

            const auto moduleBase = reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr));
            std::cout << "   -> Found at: 0x"
                      << std::hex
                      << (reinterpret_cast<uintptr_t>(address) - moduleBase)
                      << std::dec
                      << std::endl;
            return address;
        }

        void* FindClearFunction(void* anchorFunction) {
            if (!anchorFunction) {
                return Scanner::ScanMainMod(Patterns::UnderwaterMaskClear);
            }

            const uintptr_t anchor = reinterpret_cast<uintptr_t>(anchorFunction);
            if (void* local = Scanner::ScanRange(
                    anchorFunction,
                    ClearSearchWindow,
                    Patterns::UnderwaterMaskClear)) {
                return local;
            }

            const uintptr_t start = anchor > ClearSearchWindow
                ? anchor - ClearSearchWindow
                : 0;
            if (start != 0 && start < anchor) {
                if (void* local = Scanner::ScanRange(
                        reinterpret_cast<void*>(start),
                        static_cast<size_t>(anchor - start),
                        Patterns::UnderwaterMaskClear)) {
                    return local;
                }
            }

            return Scanner::ScanMainMod(Patterns::UnderwaterMaskClear);
        }

        bool InstallOne(void* target, void* hook, MaskFunction* original, const char* name) {
            if (!target) {
                return true;
            }

            if (MH_CreateHook(target, hook, reinterpret_cast<void**>(original)) == MH_OK) {
                std::cout << "[SCAN] UnderwaterMask " << name << " Hook Ready." << std::endl;
                return true;
            }

            std::cout << "[ERR] UnderwaterMask " << name << " Hook Failed." << std::endl;
            return false;
        }
    }

    void Init() {
        if (!Config::Get().disable_underwater_mask) {
            return;
        }

        void* addrPre = ScanDirect("UnderwaterMaskPreMain", Patterns::UnderwaterMaskPreMain);
        void* addrMain = ScanDirect("UnderwaterMaskMain", Patterns::UnderwaterMaskMain);
        void* addrPost = ScanDirect("UnderwaterMaskPostMain", Patterns::UnderwaterMaskPostMain);

        if (!addrPre && !addrMain && !addrPost) {
            std::cout << "[ERR] UnderwaterMask patterns not found." << std::endl;
            return;
        }

        void* clearAnchor = addrMain ? addrMain : (addrPre ? addrPre : addrPost);
        g_clearMask = reinterpret_cast<ClearFunction>(FindClearFunction(clearAnchor));
        if (g_clearMask) {
            const auto moduleBase = reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr));
            std::cout << "[SCAN] UnderwaterMask clear function found at: 0x"
                      << std::hex
                      << (reinterpret_cast<uintptr_t>(g_clearMask) - moduleBase)
                      << std::dec
                      << std::endl;
        } else {
            std::cout << "[WARN] UnderwaterMask clear function not found; active cleanup unavailable." << std::endl;
        }

        bool ok = true;
        ok &= InstallOne(addrPre, reinterpret_cast<void*>(HookPre), &g_origPre, "Pre");
        ok &= InstallOne(addrMain, reinterpret_cast<void*>(HookMain), &g_origMain, "Main");
        ok &= InstallOne(addrPost, reinterpret_cast<void*>(HookPost), &g_origPost, "Post");

        if (!ok) {
            std::cout << "[ERR] UnderwaterMask hook initialization failed." << std::endl;
        }
    }
}
