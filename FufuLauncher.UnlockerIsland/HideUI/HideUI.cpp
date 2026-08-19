/*
Copyright (c) FufuLauncher Dev Team. All rights reserved.
Licensed under the AGPL-3.0 License.
*/
#include "HideUI.h"
#include "../Patterns/Patterns.h"
#include "../Config/Config.h"
#include "../Core/Utils.h"
#include <iostream>
#include <ctime>

static HWND g_hGameWindow = NULL;
static std::atomic<bool> g_profile_privacy_config_reload_pending{ false };
static std::atomic<bool> g_profile_privacy_ui_active{ false };
static bool g_profile_uid_retry_pending = false;
static ULONGLONG g_profile_uid_retry_started = 0;
static ULONGLONG g_profile_uid_last_retry = 0;
static int g_profile_uid_retry_attempts = 0;
static const char* g_profile_birthday_resolved_target = nullptr;
static bool g_profile_uid_last_enabled = false;
static bool g_profile_birthday_last_enabled = false;

static constexpr ULONGLONG PROFILE_UID_RETRY_WINDOW_MS = 1500;
static constexpr ULONGLONG PROFILE_UID_RETRY_INTERVAL_MS = 8;
static constexpr int PROFILE_UID_MAX_RETRY_ATTEMPTS = 12;

bool CheckWindowFocused(HWND window) {
    if (!window) return false;
    DWORD foregroundProcessId = 0;
    GetWindowThreadProcessId(window, &foregroundProcessId);
    return foregroundProcessId == GetCurrentProcessId();
}

void UpdateHideUID() {
    auto& config = Config::Get();
    if (!config.hide_uid) return;

    static float last_check_time = 0.0f;
    float current_time = (float)clock() / CLOCKS_PER_SEC;

    auto SetActive = (tSetActive)o_SetActive.load();
    if (!SetActive) return;

    if (current_time - last_check_time > 2.0f) {
        last_check_time = current_time;

        auto FindString = (tFindString)p_FindString.load();
        auto FindGameObject = (tFindGameObject)p_FindGameObject.load();

        if (FindString && FindGameObject) {
            auto str_obj = FindString(GameStrings::UIDPathWatermark);
            if (str_obj) {
                void* foundObj = FindGameObject(str_obj);
                if (foundObj) {
                    SetActive(foundObj, false);
                }
            }
        }
    }
}

void UpdateHideMainUI() {
    auto& config = Config::Get();
    if (!config.hide_main_ui) return;

    static float last_check_time = 0.0f;

    auto SetActive = (tSetActive)o_SetActive.load();
    if (!SetActive) return;

    float current_time = (float)clock() / CLOCKS_PER_SEC;
    if (current_time - last_check_time > 2.0f) {
        last_check_time = current_time;

        auto FindString = (tFindString)p_FindString.load();
        auto FindGameObject = (tFindGameObject)p_FindGameObject.load();

        if (FindString && FindGameObject) {
            auto str_obj = FindString(GameStrings::UIDPathMain);
            if (str_obj) {
                void* foundObj = FindGameObject(str_obj);
                if (foundObj) {
                    SetActive(foundObj, false);
                }
            }
        }
    }
}

static bool SetProfileUIDActive(bool active) {
    auto SetActive = (tSetActive)o_SetActive.load();
    if (!SetActive) return false;

    auto FindString = (tFindString)p_FindString.load();
    auto FindGameObject = (tFindGameObject)p_FindGameObject.load();
    if (!FindString || !FindGameObject) return false;

    bool updated = false;
    SafeInvoke([&] {
        auto str_obj = FindString(GameStrings::ProfileUIDPath);
        if (str_obj) {
            void* foundObj = FindGameObject(str_obj);
            if (foundObj) {
                SetActive(foundObj, active);
                updated = true;
            }
        }
    });
    return updated;
}

bool UpdateHideProfileUID() {
    auto& config = Config::Get();
    if (!config.hide_profile_uid) return true;
    return SetProfileUIDActive(false);
}

static bool SetProfileBirthdayActive(bool active) {
    auto& config = Config::Get();

    auto SetActive = (tSetActive)o_SetActive.load();
    if (!SetActive) return false;

    auto FindString = (tFindString)p_FindString.load();
    auto FindGameObject = (tFindGameObject)p_FindGameObject.load();
    if (!FindString || !FindGameObject) return false;

    bool updated = false;
    SafeInvoke([&] {
        if (g_profile_birthday_resolved_target) {
            auto str_obj = FindString(g_profile_birthday_resolved_target);
            if (str_obj) {
                void* foundObj = FindGameObject(str_obj);
                if (foundObj) {
                    SetActive(foundObj, active);
                    updated = true;
                }
            }
            return;
        }

        for (const char* target : GameStrings::ProfileBirthdayTargets) {
            auto str_obj = FindString(target);
            if (!str_obj) continue;

            void* foundObj = FindGameObject(str_obj);
            if (foundObj) {
                SetActive(foundObj, active);
                g_profile_birthday_resolved_target = target;
                updated = true;
                if (!active && config.debug_console) {
                    std::cout << "[HideUI] Profile birthday hidden via: "
                              << target << std::endl;
                }
                break;
            }
        }
    });
    return updated;
}

void UpdateHideProfileBirthday() {
    auto& config = Config::Get();
    if (!config.hide_profile_birthday) return;
    SetProfileBirthdayActive(false);
}

void UpdateProfilePrivacyUI() {
    auto& config = Config::Get();
    if (!config.hide_profile_uid && !config.hide_profile_birthday) return;

    if (UpdateHideProfileUID()) g_profile_uid_retry_pending = false;
    UpdateHideProfileBirthday();
}

bool IsProfilePrivacyUIActive() {
    return g_profile_privacy_ui_active.load(std::memory_order_relaxed);
}

static bool FindActiveProfilePage() {
    auto FindString = (tFindString)p_FindString.load();
    auto FindGameObject = (tFindGameObject)p_FindGameObject.load();
    auto GetActive = (tGetActive)p_GetActive.load();
    if (!FindString || !FindGameObject) return false;

    bool active = false;
    SafeInvoke([&] {
        auto str_obj = FindString(GameStrings::ProfileLayerPath);
        if (!str_obj) return;

        void* foundObj = FindGameObject(str_obj);
        if (!foundObj) return;

        active = !GetActive || GetActive(foundObj);
    });
    return active;
}

static void ResetProfileUIDRetry() {
    g_profile_uid_retry_pending = false;
    g_profile_uid_retry_started = 0;
    g_profile_uid_last_retry = 0;
    g_profile_uid_retry_attempts = 0;
}

void BeginProfilePrivacyUI() {
    auto& config = Config::Get();

    g_profile_privacy_ui_active.store(true, std::memory_order_relaxed);
    ResetProfileUIDRetry();
    g_profile_uid_retry_started = GetTickCount64();

    if (config.hide_profile_uid) {
        g_profile_uid_retry_pending = !UpdateHideProfileUID();
    }
    UpdateHideProfileBirthday();
    g_profile_uid_last_enabled = config.hide_profile_uid;
    g_profile_birthday_last_enabled = config.hide_profile_birthday;
}

void EndProfilePrivacyUI() {
    g_profile_privacy_ui_active.store(false, std::memory_order_relaxed);
    ResetProfileUIDRetry();
}

void NotifyProfileUIDBlocked() {
    g_profile_uid_retry_pending = false;
}

void NotifyProfilePrivacyConfigReload() {
    g_profile_privacy_config_reload_pending.store(true, std::memory_order_release);
}

static void ApplyPendingProfilePrivacyConfigReload() {
    if (!g_profile_privacy_config_reload_pending.exchange(
            false, std::memory_order_acq_rel)) {
        return;
    }

    ResetProfileUIDRetry();
    bool profilePageActive = IsProfilePrivacyUIActive();
    if (!profilePageActive) {
        profilePageActive = FindActiveProfilePage();
        g_profile_privacy_ui_active.store(profilePageActive, std::memory_order_relaxed);
    }

    auto& config = Config::Get();
    if (!profilePageActive) {
        g_profile_uid_last_enabled = config.hide_profile_uid;
        g_profile_birthday_last_enabled = config.hide_profile_birthday;
        return;
    }

    if (config.hide_profile_uid || config.hide_profile_birthday) {
        g_ProfilePrivacyRuntimeReady.store(true, std::memory_order_relaxed);
    }

    if (config.hide_profile_uid) {
        g_profile_uid_retry_started = GetTickCount64();
        g_profile_uid_retry_pending = !UpdateHideProfileUID();
    } else if (g_profile_uid_last_enabled) {
        SetProfileUIDActive(true);
    }

    if (config.hide_profile_birthday) {
        SetProfileBirthdayActive(false);
    } else if (g_profile_birthday_last_enabled) {
        SetProfileBirthdayActive(true);
    }

    g_profile_uid_last_enabled = config.hide_profile_uid;
    g_profile_birthday_last_enabled = config.hide_profile_birthday;
}

void UpdatePendingProfilePrivacyUI() {
    ApplyPendingProfilePrivacyConfigReload();
    if (!g_profile_uid_retry_pending) return;

    auto& config = Config::Get();
    if (!config.hide_profile_uid) {
        EndProfilePrivacyUI();
        return;
    }

    ULONGLONG current_time = GetTickCount64();
    if (g_profile_uid_retry_attempts >= PROFILE_UID_MAX_RETRY_ATTEMPTS ||
        current_time - g_profile_uid_retry_started > PROFILE_UID_RETRY_WINDOW_MS) {
        g_profile_uid_retry_pending = false;
        if (config.debug_console) {
            std::cout << "[HideUI] Profile UID retry window expired." << std::endl;
        }
        return;
    }

    if (g_profile_uid_last_retry != 0 &&
        current_time - g_profile_uid_last_retry < PROFILE_UID_RETRY_INTERVAL_MS) {
        return;
    }

    g_profile_uid_last_retry = current_time;
    ++g_profile_uid_retry_attempts;
    if (UpdateHideProfileUID()) {
        g_profile_uid_retry_pending = false;
        if (config.debug_console) {
            std::cout << "[HideUI] Profile UID hidden after "
                      << g_profile_uid_retry_attempts << " bounded retries."
                      << std::endl;
        }
    }
}

void UpdateTitleWatermark() {
    if (!Config::Get().enable_custom_title) return;

    if (!g_hGameWindow || !IsWindow(g_hGameWindow)) {
        HWND hForeground = GetForegroundWindow();
        if (hForeground && CheckWindowFocused(hForeground)) {
            g_hGameWindow = hForeground;
        }
    }

    if (!g_hGameWindow) return;

    static ULONGLONG lastTick = 0;
    ULONGLONG currentTick = GetTickCount64();
    if (currentTick - lastTick < 500) return;
    lastTick = currentTick;

    SetWindowTextA(g_hGameWindow, Config::Get().custom_title_text.c_str());
}

void WINAPI hk_SetupQuestBanner(void* __this) {
    auto& cfg = Config::Get();
    auto findStr = (tFindString)p_FindString.load();
    auto findGO = (tFindGameObject)p_FindGameObject.load();
    auto setActive = (tSetActive)o_SetActive.load();

    if (IsValid(findStr) && IsValid(findGO) && IsValid(setActive)) {
        static bool s_is_hidden = false;

        if (cfg.hide_quest_banner) {
            static ULONGLONG last_check_time = 0;
            ULONGLONG current_time = GetTickCount64();

            if (current_time - last_check_time >= 500) {
                last_check_time = current_time;
                bool found = false;

                SafeInvoke([&]
                {
                    auto s = findStr(GameStrings::QuestBannerPath);
                    if (s) {
                        auto go = findGO(s);
                        if (go) {
                            setActive(go, false);
                            found = true;
                        }
                    }
                });

                s_is_hidden = found;
            }

            if (s_is_hidden) return;
        } else {
            s_is_hidden = false;
        }
    }

    auto orig = (tSetupQuestBanner)o_SetupQuestBanner.load();
    if (orig) orig(__this);
}

void WINAPI hk_ShowDamage(void* a, int b, int c, int d, float e, Il2CppString* f, void* g, void* h, int i) {
    if (Config::Get().disable_show_damage_text) return;
    auto orig = (tShowDamage)o_ShowDamage.load();
    if (orig) orig(a, b, c, d, e, f, g, h, i);
}
