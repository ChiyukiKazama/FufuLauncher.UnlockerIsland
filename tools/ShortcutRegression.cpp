// Copyright (c) FufuLauncher Dev Team. AGPL-3.0.
// Native/offline regression only: no game process or real requests.
#include "../FufuLauncher.UnlockerIsland/Automation/ClaimLogic.h"
#include "../FufuLauncher.UnlockerIsland/Config/Config.h"
#include "../FufuLauncher.UnlockerIsland/Config/ShortcutSettings.h"
#include <iostream>
#include <vector>

static int checks = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { std::cerr << "Failed at line " << __LINE__ << ": " #condition "\n"; return 1; } } while (0)

static uintptr_t allocatedType = 0, sentClient = 0, sentRequest = 0;
static bool abiValid = true, allocationFails = false;
static uintptr_t __fastcall Allocate(uintptr_t type, uintptr_t a2, uintptr_t a3, uintptr_t a4)
{
    allocatedType = type;
    abiValid = abiValid && a2 == 0 && a3 != 0 && a4 == 0;
    return allocationFails ? 0 : 0x12345;
}
static void __fastcall Send(uintptr_t client, uintptr_t request, unsigned a3, uintptr_t a4)
{
    sentClient = client; sentRequest = request;
    abiValid = abiValid && a3 == 0 && a4 == 0;
}
static std::string Read(const std::string& path)
{
    std::ifstream input(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(input)), {});
}
static void Write(const std::string& path, const std::string& bytes)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), bytes.size());
}
static std::string Utf16(const std::wstring& text)
{
    return std::string("\xFF\xFE", 2) + std::string(reinterpret_cast<const char*>(text.data()), text.size() * sizeof(wchar_t));
}

int main(int argc, char** argv)
{
    CHECK(argc == 2); // Existing isolated workspace supplied by the caller.
    using namespace Shortcuts;
    ModConfig defaults;
    CHECK(!defaults.enable_quick_claim && !defaults.enable_auto_forge);
    CHECK(defaults.claim_all_key == VK_F8 && defaults.auto_forge_key == VK_F7);

    HotkeyEdge key;
    CHECK(!key.Sample(VK_F8, false, true, true));
    CHECK(key.Sample(VK_F8, true, true, true));
    CHECK(!key.Sample(VK_F8, true, true, true));
    CHECK(!key.Sample(VK_F8, false, true, true));
    CHECK(!key.Sample(VK_F8, true, false, true));
    CHECK(!key.Sample(VK_F8, true, true, true)); // Focus return while held.
    CHECK(!key.Sample(VK_F8, false, true, false));
    CHECK(!key.Sample(VK_F8, true, true, false));
    CHECK(!key.Sample(VK_F8, true, true, true)); // Enable while held.
    CHECK(!key.Sample(VK_F7, true, true, true)); // Rebind while held.
    CHECK(!key.Sample(VK_F7, false, true, true));
    CHECK(key.Sample(VK_F7, true, true, true));
    for (int invalid : {0, -1, 255, 10000})
    {
        CHECK(!key.Sample(invalid, false, true, true));
        CHECK(!key.Sample(invalid, true, true, true));
    }

    ClaimScheduler scheduler;
    std::vector<unsigned> stages;
    auto call = [&](unsigned stage) { stages.push_back(stage); return true; };
    CHECK(scheduler.Tick(false, 0, true, call) == ClaimResult::Idle);
    CHECK(stages.empty());
    CHECK(scheduler.Tick(true, 100, true, call) == ClaimResult::FirstCalled);
    CHECK(scheduler.Pending());
    CHECK(scheduler.Tick(true, 699, true, call) == ClaimResult::Idle);
    CHECK(stages == std::vector<unsigned>{0});
    CHECK(scheduler.Tick(true, 700, false, call) == ClaimResult::SecondCalled);
    CHECK((stages == std::vector<unsigned>{0, 1}));
    CHECK(!scheduler.Pending());
    CHECK(scheduler.Tick(true, 701, false, call) == ClaimResult::Idle);
    CHECK(scheduler.Tick(true, 800, true, call) == ClaimResult::FirstCalled);
    CHECK(scheduler.Tick(false, 1400, false, call) == ClaimResult::Idle);
    CHECK(!scheduler.Pending());
    const auto before = stages.size();
    CHECK(scheduler.Tick(true, 1500, false, call) == ClaimResult::Idle && stages.size() == before);
    auto fail = [](unsigned) { return false; };
    CHECK(scheduler.Tick(true, 1600, true, fail) == ClaimResult::FirstFailed);
    CHECK(!scheduler.Pending());
    CHECK(scheduler.Tick(true, 1700, true, call) == ClaimResult::FirstCalled);
    CHECK(scheduler.Tick(true, 2300, true, fail) == ClaimResult::SecondFailed);
    CHECK(!scheduler.Pending());
    CHECK(scheduler.Tick(true, UINT32_MAX - 299, true, call) == ClaimResult::FirstCalled);
    CHECK(scheduler.Tick(true, 299, false, call) == ClaimResult::Idle);
    CHECK(scheduler.Tick(true, 300, false, call) == ClaimResult::SecondCalled);

    uintptr_t value = 0;
    CHECK(!ReadMemory(0, &value, sizeof(value)));
    CHECK(!ReadMemory(UINTPTR_MAX, &value, sizeof(value)));
    CHECK(!ReadMemory(reinterpret_cast<uintptr_t>(&value), nullptr, sizeof(value)));
    CHECK(!ReadMemory(reinterpret_cast<uintptr_t>(&value), &value, 0));
    CHECK(!ReadField(UINTPTR_MAX, 1, value));
    auto* memory = static_cast<unsigned char*>(VirtualAlloc(nullptr, 8192, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    CHECK(memory != nullptr);
    DWORD old = 0;
    CHECK(VirtualProtect(memory + 4096, 4096, PAGE_NOACCESS, &old));
    CHECK(!ReadMemory(reinterpret_cast<uintptr_t>(memory + 4092), &value, sizeof(value)));
    CHECK(ReadMemory(reinterpret_cast<uintptr_t>(memory + 4096 - sizeof(value)), &value, sizeof(value)));
    CHECK(VirtualProtect(memory + 4096, 4096, old, &old));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(memory);
    dos->e_magic = IMAGE_DOS_SIGNATURE; dos->e_lfanew = 128;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(memory + 128);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
    nt->FileHeader.TimeDateStamp = 123;
    nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    nt->OptionalHeader.SizeOfImage = 8192;
    CHECK(MatchGameHeader(reinterpret_cast<uintptr_t>(memory), 123, 8192));
    CHECK(!MatchGameHeader(reinterpret_cast<uintptr_t>(memory), 124, 8192));
    CHECK(!MatchGameHeader(reinterpret_cast<uintptr_t>(memory), 123, 8193));
    nt->FileHeader.Machine = IMAGE_FILE_MACHINE_I386;
    CHECK(!MatchGameHeader(reinterpret_cast<uintptr_t>(memory), 123, 8192));
    CHECK(VirtualFree(memory, 0, MEM_RELEASE));

    uintptr_t clientData[32] = {};
    uintptr_t root[4] = {0, reinterpret_cast<uintptr_t>(clientData)};
    uintptr_t image[4] = {0, reinterpret_cast<uintptr_t>(root), 0x20000, 0x30000};
    clientData[1] = 0x40000;
    reinterpret_cast<uint8_t*>(clientData)[24] = 1;
    ClaimBackend backend{reinterpret_cast<uintptr_t>(image), sizeof(uintptr_t), sizeof(uintptr_t),
        sizeof(uintptr_t), 24, sizeof(uintptr_t) * 2, sizeof(uintptr_t) * 3, Allocate, Send};
    const auto client = reinterpret_cast<uintptr_t>(clientData);
    CHECK(backend.Client() == client);
    CHECK(backend.Submit(client, 0));
    CHECK(allocatedType == 0x20000 && sentClient == client && sentRequest == 0x12345 && abiValid);
    CHECK(backend.Submit(client, 1) && allocatedType == 0x30000 && abiValid);
    CHECK(!backend.Submit(client, 2));
    CHECK(!backend.Submit(client + 8, 0));
    allocationFails = true; sentRequest = 0;
    CHECK(!backend.Submit(client, 0) && sentRequest == 0);
    allocationFails = false;
    image[2] = 0;
    CHECK(!backend.Submit(client, 0));
    clientData[1] = 0;
    CHECK(backend.Client() == 0);
    clientData[1] = 0x40000;
    reinterpret_cast<uint8_t*>(clientData)[24] = 0;
    CHECK(backend.Client() == 0 && !backend.Submit(client, 1));

    const auto directory = std::string(argv[1]) + "\\";
    const std::string existing = "\xEF\xBB\xBF; keep this comment\n[ cLaImAlLkEy ]\nValue = 254\nCustom = untouched\n[QuickClaim]\nValue = 1\n";
    const auto utf8 = directory + "utf8.ini";
    Write(utf8, existing);
    CHECK(Config::EnsureShortcutSettings(utf8.c_str()));
    const auto upgraded = Read(utf8);
    CHECK(upgraded.compare(0, existing.size(), existing) == 0);
    CHECK(upgraded.find("Value = 119") == std::string::npos);
    CHECK(upgraded.find("[AutoForge]") != std::string::npos);
    CHECK(upgraded.find("[AutoForgeKey]") != std::string::npos);
    CHECK(Config::EnsureShortcutSettings(utf8.c_str()) && Read(utf8) == upgraded);
    CHECK(GetFileAttributesA((utf8 + ".shortcuts.tmp").c_str()) == INVALID_FILE_ATTRIBUTES);

    const auto wide = directory + "utf16.ini";
    const auto originalWide = Utf16(L"; retain Chinese: 配置\r\n[ClaimAllKey]\r\nValue = 0\r\n");
    Write(wide, originalWide);
    CHECK(Config::EnsureShortcutSettings(wide.c_str()));
    const auto upgradedWide = Read(wide);
    CHECK(upgradedWide.compare(0, originalWide.size(), originalWide) == 0);
    const auto wideSection = Utf16(L"[AutoForgeKey]").substr(2);
    CHECK(upgradedWide.find(wideSection) != std::string::npos);
    CHECK(GetPrivateProfileIntA("ClaimAllKey", "Value", 119, wide.c_str()) == 0);
    CHECK(GetPrivateProfileIntA("AutoForgeKey", "Value", 0, wide.c_str()) == 118);
    CHECK(Config::EnsureShortcutSettings(wide.c_str()) && Read(wide) == upgradedWide);

    const auto blocked = directory + "blocked.ini";
    Write(blocked, "[Unknown]\nValue = custom\n");
    const auto blockedOriginal = Read(blocked);
    Write(blocked + ".shortcuts.tmp", "occupied");
    CHECK(!Config::EnsureShortcutSettings(blocked.c_str()));
    CHECK(Read(blocked) == blockedOriginal);
    CHECK(Read(blocked + ".shortcuts.tmp") == "occupied");
    CHECK(!Config::EnsureShortcutSettings((directory + "absent.ini").c_str()));
    std::cout << checks << " shortcut regression checks passed (no game attached).\n";
    return 0;
}
