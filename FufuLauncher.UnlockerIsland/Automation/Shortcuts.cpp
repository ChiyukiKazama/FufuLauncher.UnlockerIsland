// Copyright (c) FufuLauncher Dev Team. AGPL-3.0.
// Adapted from DetachedPlugins/ConvenienceTools.
#include "Shortcuts.h"
#include "ClaimLogic.h"
#include "../Config/Config.h"
#include "../Core/SharedState.h"
#include "../Core/Utils.h"
#include "../Patterns/Patterns.h"
#include "../Scanner/Scanner.h"

namespace
{
    Shortcuts::ClaimBackend claim;
    Shortcuts::ClaimScheduler scheduler;
    Shortcuts::HotkeyEdge claimKey, forgeKey;
    uintptr_t previousClient = 0;
    bool claimAvailable = false;

    bool Down(int key)
    {
        return key >= 1 && key <= 254 && (GetAsyncKeyState(key) & 0x8000) != 0;
    }

    void* CheckedEntry(uintptr_t base, uintptr_t rva, const char* pattern)
    {
        auto* address = reinterpret_cast<void*>(base + rva);
        return IsValidCodePtr(address) && Scanner::ScanRange(address, 64, pattern) == address ? address : nullptr;
    }
}

namespace Shortcuts
{
    void Initialize(bool isOS)
    {
        claimAvailable = false;
        claim = {};
        scheduler.Reset();
        previousClient = 0;
        const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        using namespace Patterns::CommissionCN71;
        if (isOS || !MatchGameHeader(base, Timestamp, ImageSize))
        {
            std::cout << "[Claim] Unsupported game build; commission shortcut disabled.\n";
            return;
        }
        claim = {base, RootSlot, ClientOffset, RequiredPointer, ReadyByte, FirstType, SecondType,
            reinterpret_cast<AllocateRequest>(CheckedEntry(base, AllocateRva, AllocatePrologue)),
            reinterpret_cast<SendRequest>(CheckedEntry(base, SendRva, SendPrologue))};
        claimAvailable = claim.allocate && claim.send;
        std::cout << (claimAvailable ? "[Claim] Backend validated; game-thread shortcut ready.\n" :
            "[Claim] Entry validation failed; commission shortcut disabled.\n");
    }

    void Tick(bool focused)
    {
        const auto& cfg = Config::Get();
        const bool pressed = claimKey.Sample(cfg.claim_all_key, Down(cfg.claim_all_key), focused, cfg.enable_quick_claim);
        if (!cfg.enable_quick_claim || !claimAvailable)
        {
            scheduler.Reset();
            previousClient = 0;
            if (pressed) std::cout << "[Claim] Backend unavailable; no request sent.\n";
        }
        else
        {
            const auto client = claim.Client();
            if (client != previousClient)
            {
                scheduler.Reset();
                previousClient = client;
            }
            if (pressed && !client) std::cout << "[Claim] Game state not ready; no request sent.\n";
            const auto result = scheduler.Tick(client != 0, GetTickCount(), pressed,
                [&](unsigned stage) { return claim.Submit(client, stage); });
            if (result == ClaimResult::FirstCalled)
                std::cout << "[Claim] First request sent; waiting at least 600 ms.\n";
            if (result == ClaimResult::SecondCalled)
                std::cout << "[Claim] Second request sent; awaiting game feedback.\n";
            if (result == ClaimResult::FirstFailed || result == ClaimResult::SecondFailed)
                std::cout << "[Claim] Request creation or dispatch failed.\n";
        }

        if (!forgeKey.Sample(cfg.auto_forge_key, Down(cfg.auto_forge_key), focused, cfg.enable_auto_forge)) return;
        auto canEnter = reinterpret_cast<tCheckCanEnter>(p_CheckCanEnter.load());
        auto findString = reinterpret_cast<tFindString>(p_FindString.load());
        // DetachedPlugins' OpenForgingPage signature is identical to our
        // CraftPartner page router. Reuse its resolved entry and ABI.
        auto openPage = reinterpret_cast<tCraftPartner>(p_CraftPartner.load());
        if (!IsValidCodePtr(canEnter) || !IsValidCodePtr(findString) || !IsValidCodePtr(openPage))
        {
            std::cout << "[Forge] Page entry unavailable.\n";
            return;
        }
        bool opened = false;
        SafeInvoke([&]
        {
            if (!canEnter()) return;
            auto* name = findString(GameStrings::ForgingPage);
            if (name) opened = openPage(name, nullptr, nullptr, nullptr, nullptr);
        });
        std::cout << (opened ? "[Forge] Page request accepted; awaiting game UI.\n" :
            "[Forge] Current game state did not accept the page request.\n");
    }
}
