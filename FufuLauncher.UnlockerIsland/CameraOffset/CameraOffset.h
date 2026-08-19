/*
Copyright (c) FufuLauncher Dev Team. All rights reserved.
Licensed under the AGPL-3.0 License.
*/
#pragma once

namespace CameraOffset {
    void Init();
    void SuspendImmediately();
    void Tick(bool allowGameplayCameraOffset, bool cameraOwnedByAnotherFeature);
}
