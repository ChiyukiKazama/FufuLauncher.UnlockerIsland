// Copyright (c) FufuLauncher Dev Team. AGPL-3.0.
#pragma once

namespace Shortcuts
{
    void Initialize(bool isOS);
    // Called by the existing game-thread FOV callback, like cooking/expedition.
    void Tick(bool focused);
}
