/*
Copyright (c) FufuLauncher Dev Team. All rights reserved.
Licensed under the AGPL-3.0 License.
*/
#include "SkillCooldown.h"
#include "../Config/Config.h"
#include "../Core/SharedState.h"
#include "../MinHook/MinHook.h"
#include "../Patterns/Patterns.h"
#include "../Scanner/Scanner.h"
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <unordered_map>

namespace SkillCooldown
{
    namespace
    {
        namespace Native = Patterns::SkillCooldownCN71;
        struct Vec2 { float x, y; };
        struct Vec3 { float x, y, z; };
        struct Rect { float x, y, width, height; };
        struct alignas(16) Encoded { uint64_t words[2]; };
        static_assert(sizeof(Vec2) == 8 && sizeof(Vec3) == 12 && sizeof(Encoded) == 16);

        using Unary = void(__fastcall*)(void*, void*);
        using Tick = void(__fastcall*)(void*, float, void*);
        using Setup = void(__fastcall*)(void*, uint64_t, void*);
        using Bind = void(__fastcall*)(void*, void*, int, uint64_t, bool, bool, void*);
        using Resolve = void*(__fastcall*)(void*, int, void*);
        // Explicit hidden return buffers: Win64 IL2CPP passes 16-byte values
        // through memory, not XMM0 or two integer return registers.
        using GetEncoded = Encoded*(__fastcall*)(Encoded*, void*, void*);
        using GetCount = int(__fastcall*)(void*, void*);
        using FloatDecode = float(__fastcall*)(const Encoded*, void*);
        using IntDecode = int(__fastcall*)(const Encoded*, void*);
        using Reset = void(__fastcall*)(void*, void*, float, void*);
        using Notify = void(__fastcall*)(void*, float, bool, void*);
        using GetObject = void*(__fastcall*)(void*, void*);
        using GetBool = bool(__fastcall*)(void*, void*);
        using SetBool = void(__fastcall*)(void*, bool, void*);
        using Clone = void*(__fastcall*)(void*, void*, bool, void*);
        using GetRect = Rect*(__fastcall*)(Rect*, void*, void*);
        using TransformPoint = Vec3*(__fastcall*)(Vec3*, void*, const Vec3*, void*);
        using SetVec3 = void(__fastcall*)(void*, const Vec3*, void*);
        using SetVec2 = void(__fastcall*)(void*, Vec2, void*);
        using SetText = void(__fastcall*)(void*, void*, void*);
        using SetAlignment = void(__fastcall*)(void*, int, void*);

        Bind g_Bind = nullptr;
        Setup g_Setup = nullptr;
        Unary g_Update = nullptr, g_Disable = nullptr, g_Destroy = nullptr;
        Tick g_SkillTick = nullptr, g_SprintTick = nullptr;
        Unary g_SprintRecord = nullptr;
        Resolve g_Resolve = nullptr;
        FloatDecode g_DecodeFloat = nullptr;
        IntDecode g_DecodeInt = nullptr;
        Reset g_Reset = nullptr;
        Notify g_Notify = nullptr;
        Clone g_Clone = nullptr;
        Unary g_DestroyObject = nullptr;
        GetObject g_Transform = nullptr, g_GameObject = nullptr;
        GetBool g_GetEnabled = nullptr;
        SetBool g_SetEnabled = nullptr, g_SetActive = nullptr, g_SetRaycast = nullptr;
        GetRect g_GetRect = nullptr;
        TransformPoint g_TransformPoint = nullptr, g_InversePoint = nullptr;
        SetVec3 g_SetPosition = nullptr, g_SetScale = nullptr;
        SetVec2 g_SetSize = nullptr, g_AnchorMin = nullptr, g_AnchorMax = nullptr, g_Pivot = nullptr;
        SetText g_SetText = nullptr;
        SetAlignment g_Alignment = nullptr;
        uintptr_t g_Base = 0, g_End = 0;
        std::atomic<bool> g_UIFault{false}, g_SkillFault{false}, g_SprintFault{false};
        std::atomic<bool> g_UnknownSkillReported{false};
        std::string g_TracePath;

        struct TextView
        {
            void* text = nullptr;
            char last[64] = {};
        };
        struct Row
        {
            uint64_t guid = 0;
            int slot = -1;
            TextView e, q;
            void* hiddenImage = nullptr;
            bool imageWasEnabled = false;
            unsigned traced = 0;
        };
        struct Skill
        {
            void* state = nullptr;
            float remaining = 0;
            int available = 0, maximum = 0;
        };
        // UI state is used only from native HUD callbacks. The GUID whitelist
        // is separately protected because logic ticks may use another thread.
        std::unordered_map<void*, Row> g_Rows;
        std::unordered_map<void*, uint64_t> g_Party;
        std::mutex g_PartyMutex;
        std::atomic<void*> g_Controller{nullptr}; // Active list, not the unused-row queue.

        template<typename T> T Read(const void* object, size_t offset)
        {
            return *reinterpret_cast<const T*>(static_cast<const uint8_t*>(object) + offset);
        }

        bool Alive(void* object)
        {
            return object && Read<void*>(object, 0x10); // Unity Object.m_CachedPtr
        }

        template<typename T> T Virtual(void* object, unsigned slot)
        {
            if (!object) return nullptr;
            auto* klass = Read<void*>(object, 0);
            if (!klass) return nullptr;
            // This game's IL2CPP vtable has 8-byte code entries at class+0xD0.
            auto address = Read<uintptr_t>(klass, 0xD0 + slot * sizeof(void*));
            if (address < g_Base || address >= g_End) return nullptr;
            return reinterpret_cast<T>(address);
        }

        void RememberParty(void* row, uint64_t guid)
        {
            std::lock_guard<std::mutex> lock(g_PartyMutex);
            if (guid) g_Party[row] = guid;
            else g_Party.erase(row);
            if (g_Party.empty()) g_Controller.store(nullptr);
        }

        bool PartyAvatar(void* avatar)
        {
            if (!avatar) return false;
            // Only inspect a proven AvatarEntity: the GUID offset does not
            // apply to monsters, gadgets or the base Entity class.
            if (Read<void*>(avatar, 0) != Read<void*>(reinterpret_cast<void*>(g_Base), 0x57B1570))
                return false;
            const auto guid = Read<uint64_t>(avatar, 0x558);
            void* controller = g_Controller.load();
            if (controller)
            {
                void* list = Read<void*>(controller, 0x4F0);
                void* array = list ? Read<void*>(list, 0x10) : nullptr;
                const int count = list ? Read<int>(list, 0x18) : 0;
                if (!array || count <= 0 || count > 32 ||
                    static_cast<uintptr_t>(count) > Read<uintptr_t>(array, 0x18))
                    return false;
                for (int i = 0; i < count; ++i)
                {
                    void* row = Read<void*>(array, 0x20 + i * sizeof(void*));
                    std::lock_guard<std::mutex> lock(g_PartyMutex);
                    const auto it = g_Party.find(row);
                    if (it != g_Party.end() && guid && it->second == guid) return true;
                }
                return false;
            }
            std::lock_guard<std::mutex> lock(g_PartyMutex);
            for (const auto& entry : g_Party)
                if (guid && entry.second == guid) return true;
            return false;
        }

        bool CurrentAvatar(void* avatar)
        {
            if (!PartyAvatar(avatar)) return false;
            void* managers = Read<void*>(reinterpret_cast<void*>(g_Base), Native::Managers);
            if (!managers) return false;
            void* scene = Read<void*>(managers, 0x9D50);
            return scene && Read<uint32_t>(scene, 0x250) != 0 &&
                Read<uint32_t>(scene, 0x250) == Read<uint32_t>(avatar, 0x3A8);
        }

        void* ResolveSkill(void* component, unsigned slot)
        {
            if (!component || !g_Resolve) return nullptr;
            // Same resolved-slot array and fallback as native MonoBattleBtn.
            // Re-resolve every tick so tap/hold variants and replacements are
            // not cached against a stale skill ID.
            void* array = Read<void*>(component, 0x2B0);
            if (array && Read<uintptr_t>(array, 0x18) > slot)
            {
                return Read<void*>(array, 0x20 + slot * sizeof(void*));
            }
            return g_Resolve(component, static_cast<int>(slot), nullptr);
        }

        bool ReadCooldown(void* component, unsigned slot, Skill& result)
        {
            result = {};
            result.state = ResolveSkill(component, slot);
            auto remaining = Virtual<GetEncoded>(result.state, 5);
            if (!remaining) return false;
            Encoded value{};
            remaining(&value, result.state, nullptr);
            result.remaining = g_DecodeFloat(&value, nullptr);
            return std::isfinite(result.remaining);
        }

        bool ReadCharges(Skill& result)
        {
            auto available = Virtual<GetEncoded>(result.state, 7);
            auto maximum = Virtual<GetCount>(result.state, 16);
            if (!available || !maximum) return false;
            Encoded value{};
            available(&value, result.state, nullptr);
            result.available = g_DecodeInt(&value, nullptr);
            result.maximum = maximum(result.state, nullptr);
            return result.maximum > 0 &&
                result.available >= 0 && result.available <= result.maximum;
        }

        bool ReadSkill(void* component, unsigned slot, Skill& result)
        {
            return ReadCooldown(component, slot, result) && ReadCharges(result);
        }

        bool Format(const Skill& skill, bool charges, char (&text)[64])
        {
            text[0] = 0;
            if (!std::isfinite(skill.remaining) || skill.remaining <= 0.0f) return false;
            // Round upward to a tenth; a live cooldown never displays 0.0.
            const double rounded = std::ceil(static_cast<double>(skill.remaining) * 10.0 - 1e-5) / 10.0;
            const double seconds = rounded > 0 ? rounded : 0.1;
            const int written = charges && skill.maximum > 1 ?
                _snprintf_s(text, sizeof(text), _TRUNCATE, "%.1f %d/%d", seconds, skill.available, skill.maximum) :
                _snprintf_s(text, sizeof(text), _TRUNCATE, "%.1f", seconds);
            if (written < 0) { text[0] = 0; return false; }
            return true;
        }

        void RestoreIcon(Row& row)
        {
            if (Alive(row.hiddenImage))
                g_SetEnabled(row.hiddenImage, row.imageWasEnabled, nullptr);
            row.hiddenImage = nullptr;
        }

        void RemoveText(TextView& view)
        {
            if (Alive(view.text))
            {
                void* object = g_GameObject(view.text, nullptr);
                if (Alive(object))
                {
                    g_SetActive(object, false, nullptr);
                    g_DestroyObject(object, nullptr);
                }
            }
            view = {};
        }

        void ClearView(Row& row)
        {
            RestoreIcon(row);
            RemoveText(row.e);
            RemoveText(row.q);
        }

        bool CreateText(TextView& view, void* source, void* parent)
        {
            if (Alive(view.text)) return true;
            view = {};
            if (!Alive(source) || !Alive(parent)) return false;
            view.text = g_Clone(source, parent, false, nullptr);
            if (!Alive(view.text)) return false;
            g_SetRaycast(view.text, false, nullptr);
            g_Alignment(view.text, 4, nullptr); // MiddleCenter, inherited native font/material.
            g_SetEnabled(view.text, true, nullptr);
            void* transform = g_Transform(view.text, nullptr);
            if (!Alive(transform)) { RemoveText(view); return false; }
            const Vec3 scale{1, 1, 1};
            g_SetScale(transform, &scale, nullptr);
            g_AnchorMin(transform, {0.5f, 0.5f}, nullptr);
            g_AnchorMax(transform, {0.5f, 0.5f}, nullptr);
            g_Pivot(transform, {0.5f, 0.5f}, nullptr);
            return true;
        }

        bool ShowText(TextView& view, const char* text)
        {
            if (!Alive(view.text)) return false;
            void* object = g_GameObject(view.text, nullptr);
            if (!Alive(object)) return false;
            if (!text || !*text)
            {
                g_SetActive(object, false, nullptr);
                return false;
            }
            if (std::strcmp(text, view.last) != 0)
            {
                auto stringNew = reinterpret_cast<FnStringNew>(p_StringNew.load());
                if (!stringNew) { g_SetActive(object, false, nullptr); return false; }
                void* string = reinterpret_cast<void*>(stringNew(text));
                if (!string) { g_SetActive(object, false, nullptr); return false; }
                g_SetText(view.text, string, nullptr);
                strcpy_s(view.last, text);
            }
            g_SetActive(object, true, nullptr);
            return true;
        }

        bool BoundsInRow(void* anchor, void* parent, Rect& bounds, float& depth)
        {
            if (!Alive(anchor) || !Alive(parent)) return false;
            Rect rect{};
            g_GetRect(&rect, anchor, nullptr);
            if (!std::isfinite(rect.x) || !std::isfinite(rect.y) ||
                !std::isfinite(rect.width) || !std::isfinite(rect.height) ||
                rect.width <= 0 || rect.height <= 0) return false;
            float right = 0, top = 0;
            for (unsigned i = 0; i < 4; ++i)
            {
                const Vec3 corner{rect.x + ((i & 1) ? rect.width : 0),
                                  rect.y + ((i & 2) ? rect.height : 0), 0};
                Vec3 world{}, local{};
                g_TransformPoint(&world, anchor, &corner, nullptr);
                g_InversePoint(&local, parent, &world, nullptr);
                if (!std::isfinite(local.x) || !std::isfinite(local.y) || !std::isfinite(local.z))
                    return false;
                if (!i) { bounds.x = right = local.x; bounds.y = top = local.y; }
                bounds.x = std::fmin(bounds.x, local.x); right = std::fmax(right, local.x);
                bounds.y = std::fmin(bounds.y, local.y); top = std::fmax(top, local.y);
                depth = local.z;
            }
            bounds.width = right - bounds.x; bounds.height = top - bounds.y;
            return bounds.width > 0 && bounds.height > 0;
        }

        bool PlaceText(TextView& view, void* anchor, void* parent, bool isE,
                       void* qAnchor = nullptr, bool charges = false)
        {
            if (!Alive(view.text)) return false;
            void* transform = g_Transform(view.text, nullptr);
            Rect rect{};
            float depth = 0;
            if (!Alive(transform) || !BoundsInRow(anchor, parent, rect, depth)) return false;
            Vec2 size{rect.width * 1.5f, rect.height};
            Vec3 local{rect.x + rect.width * 0.5f, rect.y + rect.height * 0.5f, depth};
            if (isE)
            {
                size = {rect.height * (charges ? 2.5f : 1.5f), rect.height};
                float left = rect.x;
                Rect qBounds{};
                float qDepth = 0;
                if (BoundsInRow(qAnchor, parent, qBounds, qDepth))
                    left = std::fmin(left, qBounds.x - qBounds.width * 0.25f);
                // Reserve the native Q label's complete width. Its glyph can
                // already be left of the portrait; portrait-only placement
                // puts E and Q on top of each other in that layout.
                local.x = left - rect.height * 0.1f - size.x * 0.5f;
            }
            Vec3 position{};
            g_TransformPoint(&position, parent, &local, nullptr);
            if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z))
                return false;
            g_SetSize(transform, size, nullptr);
            g_SetPosition(transform, &position, nullptr);
            return true;
        }

        void TraceDisplay(Row& row, const Skill& e, bool hasE, const Skill& q, bool hasQ,
                          bool visibleE, bool visibleQ)
        {
            if (g_TracePath.empty()) return;
            const unsigned seen = 1u | ((hasE && e.remaining > 0) ? 2u : 0u) |
                                       ((hasQ && q.remaining > 0) ? 4u : 0u) |
                                       (visibleE ? 8u : 0u) | (visibleQ ? 16u : 0u);
            if ((row.traced & seen) == seen) return;
            row.traced |= seen;
            std::ofstream log(g_TracePath, std::ios::app);
            if (log) log << "[DisplaySkillCD] slot=" << row.slot
                         << " E=" << hasE << '/' << e.remaining << " charges=" << e.available << '/' << e.maximum
                         << " Q=" << hasQ << '/' << q.remaining
                         << " visible=" << visibleE << '/' << visibleQ << '\n';
        }

        void UpdateView(void* object, Row& row)
        {
            if (!Config::Get().display_skill_cd || g_UIFault.load())
            {
                ClearView(row);
                return;
            }
            const auto guid = Read<uint64_t>(object, 0x178);
            void* component = Read<void*>(object, 0x188);
            void* avatar = component ? Read<void*>(component, 0x28) : nullptr;
            if (!guid || guid != row.guid || !PartyAvatar(avatar) || Read<uint64_t>(avatar, 0x558) != guid)
            {
                ClearView(row);
                return;
            }
            Skill e, q;
            // Like native IEMBBEOJEEK's cooldown adapter, displaying the timer
            // does not require valid charge metadata. Keep that requirement
            // only for the mutation path and the optional charge-count suffix.
            const bool hasE = ReadCooldown(component, 1, e), hasQ = ReadCooldown(component, 4, q);
            const bool eCharges = hasE && ReadCharges(e);
            char eText[64]{}, qText[64]{};
            const bool showE = hasE && Format(e, eCharges, eText);
            const bool showQ = hasQ && Format(q, false, qText);
            if (!showE && !showQ)
            {
                ShowText(row.e, nullptr);
                ShowText(row.q, nullptr);
                TraceDisplay(row, e, hasE, q, hasQ, false, false);
                return;
            }
            void* source = Read<void*>(object, 0xA0); // Clone; never overwrite swap cooldown.
            void* parent = g_Transform(object, nullptr);
            void* icon = Read<void*>(object, 0x58);
            void* qButton = Read<void*>(object, 0x48);
            void* qGlyph = Read<void*>(object, 0x1B0);
            // Both labels are children of the row, so a natively hidden Q
            // button or swap-CD parent cannot hide our cooldown label.
            void* qAnchor = Alive(qGlyph) ? g_Transform(qGlyph, nullptr) : nullptr;
            const bool visibleE = showE && Alive(icon) && CreateText(row.e, source, parent) &&
                PlaceText(row.e, g_Transform(icon, nullptr), parent, true, qAnchor, eCharges && e.maximum > 1) &&
                ShowText(row.e, eText);
            if (!visibleE) ShowText(row.e, nullptr);
            const bool visibleQ = showQ && Alive(qButton) && Alive(qGlyph) && CreateText(row.q, source, parent) &&
                PlaceText(row.q, qAnchor, parent, false) && ShowText(row.q, qText);
            if (visibleQ)
            {
                // SetupView caches the actual burst sprite Image here. The
                // Button's targetGraphic can be a different background layer.
                void* image = qGlyph;
                if (Alive(image))
                {
                    row.imageWasEnabled = g_GetEnabled(image, nullptr);
                    row.hiddenImage = image;
                    // Only the glyph Graphic: leave Button, energy fills,
                    // colors, interactability and parent active state native.
                    g_SetEnabled(image, false, nullptr);
                }
            }
            else ShowText(row.q, nullptr);
            TraceDisplay(row, e, hasE, q, hasQ, visibleE, visibleQ);
        }

        void GuardRestore(Row* row)
        {
            __try { RestoreIcon(*row); }
            __except (EXCEPTION_EXECUTE_HANDLER) { row->hiddenImage = nullptr; g_UIFault.store(true); }
        }

        void GuardRemove(TextView* view)
        {
            __try { RemoveText(*view); }
            __except (EXCEPTION_EXECUTE_HANDLER) { *view = {}; g_UIFault.store(true); }
        }

        void GuardClear(Row* row)
        {
            // A destroyed glyph must not prevent the other two objects from
            // being cleaned up. Each retained Unity reference is independent.
            GuardRestore(row);
            GuardRemove(&row->e);
            GuardRemove(&row->q);
        }

        void GuardView(void* object, Row* row, bool clear)
        {
            __try
            {
                if (clear) ClearView(*row);
                else UpdateView(object, *row);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                if (!g_UIFault.exchange(true))
                    std::cout << "[WARN] Skill cooldown HUD stopped after an invalid native object.\n";
                GuardClear(row);
            }
        }

        void RefreshRow(void* object, uint64_t guid, int slot)
        {
            Row& row = g_Rows[object];
            if (row.guid != guid) { GuardView(object, &row, true); row.traced = 0; }
            row.guid = guid;
            row.slot = slot;
            RememberParty(object, guid);
        }

        void __fastcall HookSetup(void* object, uint64_t guid, void* method)
        {
            auto it = g_Rows.find(object);
            if (it != g_Rows.end()) GuardView(object, &it->second, true);
            g_Setup(object, guid, method);
            RefreshRow(object, guid, -1);
        }

        void __fastcall HookBind(void* controller, void* object, int slot, uint64_t guid,
                                 bool first, bool second, void* method)
        {
            g_Bind(controller, object, slot, guid, first, second, method);
            {
                std::lock_guard<std::mutex> lock(g_PartyMutex);
                g_Controller.store(controller);
            }
            RefreshRow(object, guid, slot);
        }

        void DiscoverRow(void* object)
        {
            __try
            {
                const auto guid = Read<uint64_t>(object, 0x178);
                if (guid) RefreshRow(object, guid, -1);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { g_UIFault.store(true); }
        }

        void __fastcall HookUpdate(void* object, void* method)
        {
            // Return our temporary glyph override before native refresh. The
            // native button's energy/availability decision runs normally.
            auto it = g_Rows.find(object);
            if (it != g_Rows.end()) GuardRestore(&it->second);
            g_Update(object, method);
            it = g_Rows.find(object);
            if (it == g_Rows.end() && !g_UIFault.load())
            {
                // Also support attaching after the native main page is built.
                DiscoverRow(object);
                it = g_Rows.find(object);
            }
            if (it != g_Rows.end()) GuardView(object, &it->second, false);
        }

        void __fastcall HookDisable(void* object, void* method)
        {
            auto it = g_Rows.find(object);
            if (it != g_Rows.end()) GuardView(object, &it->second, true);
            // Keep identity while HUD is hidden; NoSkillCD is independent of UI.
            g_Disable(object, method);
        }

        void __fastcall HookDestroy(void* object, void* method)
        {
            auto it = g_Rows.find(object);
            if (it != g_Rows.end())
            {
                GuardView(object, &it->second, true);
                g_Rows.erase(it);
            }
            RememberParty(object, 0);
            g_Destroy(object, method);
        }

        void ClearSkills(void* component)
        {
            if (!Config::Get().no_skill_cd || g_SkillFault.load() || !component ||
                !PartyAvatar(Read<void*>(component, 0x28))) return;
            for (unsigned slot : {1u, 4u})
            {
                Skill skill;
                if (!ReadSkill(component, slot, skill) ||
                    (skill.remaining <= 0 && skill.available == skill.maximum)) continue;
                auto reset = Virtual<Reset>(skill.state, 23);
                if (reset != g_Reset)
                {
                    // KFOFIMGPIOK delegates state to a different manager; its
                    // reset virtual is a no-op. Never corrupt its base fields.
                    if (!g_UnknownSkillReported.exchange(true))
                        std::cout << "[WARN] NoSkillCD skipped a delegated skill state.\n";
                    continue;
                }
                // Native EJLIFNFCLBJ(null,0) clears the pending recharge list,
                // restores max charges and writes an encoded zero cooldown.
                reset(skill.state, nullptr, 0.0f, nullptr);
                g_Notify(skill.state, 0.0f, true, nullptr);
            }
        }

        void GuardSkills(void* component)
        {
            __try { ClearSkills(component); }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                if (!g_SkillFault.exchange(true))
                    std::cout << "[WARN] NoSkillCD stopped after an invalid native skill state.\n";
            }
        }

        void __fastcall HookSkillTick(void* component, float dt, void* method)
        {
            GuardSkills(component);
            g_SkillTick(component, dt, method);
            GuardSkills(component);
        }

        void GuardSprint(void* fsm)
        {
            __try
            {
                if (!Config::Get().no_sprint_cd || g_SprintFault.load() || !fsm) return;
                void* move = Read<void*>(fsm, 0x118);
                if (!move || !CurrentAvatar(Read<void*>(move, 0x28))) return;
                // Exactly the two flags native FLJHPOFJFLC clears when its
                // sprintCheckInterval/muteSprintInterval thresholds expire.
                // Stamina, animation states and movement speeds are untouched.
                *(static_cast<uint8_t*>(fsm) + 0x2C3) = 0;
                *(static_cast<uint8_t*>(fsm) + 0x2E5) = 0;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                if (!g_SprintFault.exchange(true))
                    std::cout << "[WARN] NoSprintCD stopped after an invalid native movement state.\n";
            }
        }

        void __fastcall HookSprintRecord(void* fsm, void* method)
        {
            g_SprintRecord(fsm, method);
            GuardSprint(fsm);
        }

        void __fastcall HookSprintTick(void* fsm, float dt, void* method)
        {
            g_SprintTick(fsm, dt, method);
            GuardSprint(fsm);
        }

        void* Address(const Native::Method& entry)
        {
            void* address = reinterpret_cast<void*>(g_Base + entry.rva);
            return Scanner::ScanRange(address, 32, entry.bytes) == address ? address : nullptr;
        }

        template<typename T> bool Load(T& function, const Native::Method& entry)
        {
            function = reinterpret_cast<T>(Address(entry));
            return function != nullptr;
        }

        bool ImageMatches()
        {
            __try
            {
                auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_Base);
                if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x1000)
                    return false;
                auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_Base + dos->e_lfanew);
                if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
                    nt->FileHeader.TimeDateStamp != Native::Timestamp || nt->OptionalHeader.SizeOfImage != Native::ImageSize)
                    return false;
                g_End = g_Base + nt->OptionalHeader.SizeOfImage;
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }
    }

    void Init(bool isOS)
    {
        g_Base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        if (isOS || !g_Base || !ImageMatches())
        {
            std::cout << "[WARN] Skill cooldown functions require the verified CN 7.1 layout.\n";
            return;
        }
        g_TracePath = Config::GetConfigPath();
        g_TracePath.resize(g_TracePath.find_last_of("\\/") + 1);
        g_TracePath += "skill-cooldown.log";
        { std::ofstream log(g_TracePath, std::ios::trunc); if (log) log << "DisplaySkillCD EQ layout revision 2 (CN 7.1)\n"; }
        bool valid = true;
#define LOAD(name, field) valid = Load(field, Native::name) && valid
        LOAD(ResolveSkill, g_Resolve); LOAD(DecodeFloat, g_DecodeFloat); LOAD(DecodeInt, g_DecodeInt);
        LOAD(ResetCharges, g_Reset); LOAD(NotifyCharges, g_Notify);
        LOAD(CloneText, g_Clone); LOAD(DestroyObject, g_DestroyObject);
        LOAD(GetTransform, g_Transform); LOAD(GetGameObject, g_GameObject);
        LOAD(GetEnabled, g_GetEnabled); LOAD(SetEnabled, g_SetEnabled); LOAD(SetActive, g_SetActive);
        LOAD(GetRect, g_GetRect); LOAD(SetPosition, g_SetPosition);
        LOAD(TransformPoint, g_TransformPoint); LOAD(SetScale, g_SetScale); LOAD(SetSize, g_SetSize);
        LOAD(InverseTransformPoint, g_InversePoint);
        LOAD(SetAnchorMin, g_AnchorMin); LOAD(SetAnchorMax, g_AnchorMax); LOAD(SetPivot, g_Pivot);
        LOAD(SetText, g_SetText); LOAD(SetAlignment, g_Alignment); LOAD(SetRaycast, g_SetRaycast);
#undef LOAD
        struct Hook { const Native::Method* entry; void* replacement; void** original; void* address; };
        Hook hooks[] = {
            {&Native::BindTeam, reinterpret_cast<void*>(HookBind), reinterpret_cast<void**>(&g_Bind)},
            {&Native::SetupRow, reinterpret_cast<void*>(HookSetup), reinterpret_cast<void**>(&g_Setup)},
            {&Native::UpdateRow, reinterpret_cast<void*>(HookUpdate), reinterpret_cast<void**>(&g_Update)},
            {&Native::DisableRow, reinterpret_cast<void*>(HookDisable), reinterpret_cast<void**>(&g_Disable)},
            {&Native::DestroyRow, reinterpret_cast<void*>(HookDestroy), reinterpret_cast<void**>(&g_Destroy)},
            {&Native::SkillTick, reinterpret_cast<void*>(HookSkillTick), reinterpret_cast<void**>(&g_SkillTick)},
            {&Native::SprintRecord, reinterpret_cast<void*>(HookSprintRecord), reinterpret_cast<void**>(&g_SprintRecord)},
            {&Native::SprintTick, reinterpret_cast<void*>(HookSprintTick), reinterpret_cast<void**>(&g_SprintTick)}
        };
        for (auto& hook : hooks)
        {
            hook.address = Address(*hook.entry);
            valid = hook.address && valid;
        }
        if (!valid)
        {
            std::cout << "[WARN] Skill cooldown entry verification failed; hooks not installed.\n";
            return;
        }
        size_t installed = 0;
        for (auto& hook : hooks)
        {
            if (MH_CreateHook(hook.address, hook.replacement, hook.original) != MH_OK)
            {
                while (installed) MH_RemoveHook(hooks[--installed].address);
                std::cout << "[WARN] Skill cooldown hook installation failed; partial hooks removed.\n";
                return;
            }
            ++installed;
        }
        // Hooks are also installed with options off for existing config reload.
        // Hooks::Init enables them together with the other project hooks.
        std::cout << "[SCAN] DisplaySkillCD / NoSkillCD / NoSprintCD hooks ready (CN 7.1).\n";
    }
}
