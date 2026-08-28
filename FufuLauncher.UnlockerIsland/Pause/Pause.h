/*
Copyright (c) FufuLauncher Dev Team. All rights reserved.
Licensed under the AGPL-3.0 License.
*/
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace Pause {
    bool Prepare(HWND gameWindow);
    bool Activate();
    void Cancel();
    void End();
    void Update();
    void NotifyConfigReload();
    bool IsActive();
}
