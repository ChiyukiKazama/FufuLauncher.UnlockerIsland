/*
Copyright (c) FufuLauncher Dev Team. All rights reserved.
Licensed under the AGPL-3.0 License.
*/
#include "CameraOffset.h"

#include "../Camera/Camera.h"
#include "../Config/Config.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <Windows.h>

namespace CameraOffset {
    namespace {
        bool g_AppliedStateValid = false;
        void* g_AppliedTransform = nullptr;
        Vector3 g_LastBase = { 0, 0, 0 };
        Vector3 g_LastOutput = { 0, 0, 0 };
        Vector3 g_CurrentOffset = { 0, 0, 0 };
        ULONGLONG g_LastTransitionTick = 0;
        bool g_ShoulderBasisValid = false;
        Vector3 g_LastShoulderRight = { 1, 0, 0 };
        Vector3 g_LastShoulderBack = { 0, 0, -1 };

        bool NearlyEqual(float a, float b) {
            return fabsf(a - b) <= 0.0005f;
        }

        bool NearlyEqual(const Vector3& a, const Vector3& b) {
            return NearlyEqual(a.x, b.x) &&
                NearlyEqual(a.y, b.y) &&
                NearlyEqual(a.z, b.z);
        }

        void ResetAppliedOffset(bool restoreIfUntouched) {
            if (!g_AppliedStateValid) return;

            if (restoreIfUntouched &&
                g_AppliedTransform == Camera::GetTransform()) {
                Vector3 current{};
                if (Camera::GetPosition(current) &&
                    NearlyEqual(current, g_LastOutput)) {
                    Camera::SetPosition(g_LastBase);
                }
            }

            g_AppliedStateValid = false;
            g_AppliedTransform = nullptr;
        }

        Vector3 AdvanceOffset(const Vector3& targetOffset, float transitionSpeed) {
            ULONGLONG now = GetTickCount64();
            float deltaSeconds = 1.0f / 60.0f;
            if (g_LastTransitionTick != 0 && now > g_LastTransitionTick) {
                deltaSeconds = static_cast<float>(now - g_LastTransitionTick) / 1000.0f;
                deltaSeconds = std::clamp(deltaSeconds, 0.0f, 0.1f);
            }
            g_LastTransitionTick = now;

            if (transitionSpeed < 0.1f) transitionSpeed = 0.1f;
            float blend = 1.0f - expf(-transitionSpeed * deltaSeconds);
            g_CurrentOffset.x += (targetOffset.x - g_CurrentOffset.x) * blend;
            g_CurrentOffset.y += (targetOffset.y - g_CurrentOffset.y) * blend;
            g_CurrentOffset.z += (targetOffset.z - g_CurrentOffset.z) * blend;
            if (NearlyEqual(g_CurrentOffset, targetOffset)) {
                g_CurrentOffset = targetOffset;
            }
            return g_CurrentOffset;
        }

        bool UpdateShoulderBasis() {
            Camera::Quaternion rotation{};
            if (!Camera::GetRotation(rotation)) return g_ShoulderBasisValid;

            float normSquared = rotation.x * rotation.x +
                rotation.y * rotation.y + rotation.z * rotation.z +
                rotation.w * rotation.w;
            if (!std::isfinite(normSquared) || normSquared < 0.25f ||
                normSquared > 4.0f) {
                return g_ShoulderBasisValid;
            }

            float inverseNorm = 1.0f / sqrtf(normSquared);
            rotation.x *= inverseNorm;
            rotation.y *= inverseNorm;
            rotation.z *= inverseNorm;
            rotation.w *= inverseNorm;

            Vector3 right = Camera::RotateVector(rotation, { 1, 0, 0 });
            right.y = 0.0f;
            float horizontalLength = sqrtf(right.x * right.x + right.z * right.z);
            if (!std::isfinite(horizontalLength) || horizontalLength < 0.001f) {
                return g_ShoulderBasisValid;
            }
            right.x /= horizontalLength;
            right.z /= horizontalLength;

            g_LastShoulderRight = right;
            g_LastShoulderBack = { right.z, 0.0f, -right.x };
            g_ShoulderBasisValid = true;
            return true;
        }

        Vector3 ResolveWorldOffset(const Vector3& cameraOffset) {
            Vector3 worldOffset{ 0.0f, cameraOffset.y, 0.0f };
            if (UpdateShoulderBasis()) {
                worldOffset.x += g_LastShoulderRight.x * cameraOffset.x +
                    g_LastShoulderBack.x * cameraOffset.z;
                worldOffset.z += g_LastShoulderRight.z * cameraOffset.x +
                    g_LastShoulderBack.z * cameraOffset.z;
            }
            return worldOffset;
        }

        void ApplyOffset(const Vector3& cameraOffset) {
            if (NearlyEqual(cameraOffset, { 0, 0, 0 })) {
                ResetAppliedOffset(true);
                return;
            }

            void* transform = Camera::GetTransform();
            if (!transform) return;

            if (g_AppliedStateValid && g_AppliedTransform != transform) {
                ResetAppliedOffset(false);
            }

            Vector3 current{};
            if (!Camera::GetPosition(current)) return;

            Vector3 base = current;
            if (g_AppliedStateValid && g_AppliedTransform == transform &&
                NearlyEqual(current, g_LastOutput)) {
                // ChangeFOV may run more than once before the game updates the
                // camera. Reuse the untouched base to prevent cumulative drift.
                base = g_LastBase;
            }

            Vector3 worldOffset = ResolveWorldOffset(cameraOffset);
            Vector3 adjusted = base;
            adjusted.x += worldOffset.x;
            adjusted.y += worldOffset.y;
            adjusted.z += worldOffset.z;
            if (Camera::SetPosition(adjusted)) {
                g_AppliedStateValid = true;
                g_AppliedTransform = transform;
                g_LastBase = base;
                g_LastOutput = adjusted;
            }
        }
    }

    void Init() {
        if (Camera::IsReady()) {
            std::cout << "   -> Follow-camera offset ready." << std::endl;
        } else {
            std::cout << "   -> [ERR] Follow-camera offset disabled: shared camera access is unavailable." << std::endl;
        }
    }

    void SuspendImmediately() {
        // ChangeFOV is a state-change event rather than a guaranteed per-frame
        // callback. Restore the known unmodified base on the aiming event so
        // the offset cannot remain mostly applied after a single blend step.
        ResetAppliedOffset(true);
        g_CurrentOffset = { 0.0f, 0.0f, 0.0f };
        g_LastTransitionTick = 0;
    }

    void Tick(bool allowGameplayCameraOffset, bool cameraOwnedByAnotherFeature) {
        auto& config = Config::Get();
        bool allowOffset = config.enable_camera_offset &&
            allowGameplayCameraOffset && !cameraOwnedByAnotherFeature;

        Vector3 targetOffset{ 0.0f, 0.0f, 0.0f };
        if (allowOffset) {
            targetOffset = {
                config.camera_offset_x,
                config.camera_offset_y,
                config.camera_offset_z
            };
        }
        Vector3 currentOffset = AdvanceOffset(
            targetOffset, config.camera_height_transition_speed);

        if (!Camera::IsReady()) {
            g_CurrentOffset = { 0.0f, 0.0f, 0.0f };
            g_LastTransitionTick = 0;
            return;
        }

        if (cameraOwnedByAnotherFeature) {
            // Restore the untouched game camera before the owning feature
            // writes its own transform. The internal offset still eases to zero.
            ResetAppliedOffset(true);
            return;
        }

        ApplyOffset(currentOffset);
    }
}
