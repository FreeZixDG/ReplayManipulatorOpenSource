#pragma once
#include "Data/PriUid.h"

struct CameraOverride
{
    bool enabled = false;
    ProfileCameraSettings override_settings{};
    ProfileCameraSettings original_settings{};
    PriUid id{""};

    [[nodiscard]] ProfileCameraSettings GetCameraSettings() const;
};


class CameraSettingsOverride
{
public:
    explicit CameraSettingsOverride(std::shared_ptr<GameWrapper> gw);

    [[nodiscard]] CameraOverride GetCameraOverrideSettings(const PriUid& id) const;
    /// False until the game has told us this player's camera. GetCameraOverrideSettings hands
    /// back an all-zero default in that case, which is fine to draw sliders from but must not
    /// be stored as if it were a real camera setup.
    [[nodiscard]] bool HasCameraOverride(const PriUid& id) const;
    void SetCameraOverrideSettings(const PriUid& id, const CameraOverride& camera_override_settings);
    /// Overrides this player's camera and writes it to the game right away, creating the entry
    /// from the player's own camera if we have not seen them yet. Unlike
    /// SetCameraOverrideSettings this keeps original_settings, so Reset still works afterwards.
    /// Game thread only.
    void ApplyCameraOverride(PriWrapper& pri, bool enabled, const ProfileCameraSettings& settings);
    /// Switches the override off and puts the player's own camera back. Does nothing if we
    /// never saw this player's camera, since then we never touched it. Game thread only.
    void ResetCameraOverride(PriWrapper& pri);

    //static ProfileCameraSettings GetPriPersistentCameraSettings(const PriWrapper& pri);
    static CameraOverride ReadOriginalSetting(PriWrapper& pri);
    static void SetPriCameraSetting(const PriWrapper& pri, const ProfileCameraSettings& settings);

    [[nodiscard]] static bool RenderCameraOverride(CameraOverride& camera_override);

private:
    void OnPersistentCameraSet(const CameraSettingsActorWrapper& camera_settings);

    std::unordered_map<PriUid, CameraOverride> camera_overrides_;
    std::shared_ptr<GameWrapper> gw_;
};
