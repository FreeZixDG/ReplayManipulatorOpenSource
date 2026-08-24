#pragma once
#include <optional>
#include <string>

#include "bakkesmod/core/loadout_structs.h"

/// Camera settings the way a preset stores them. `enabled` mirrors CameraOverride::enabled,
/// so a preset can carry a camera setup with the override left switched off.
struct CameraPreset
{
    bool enabled = false;
    ProfileCameraSettings settings{};
};

/// One saved "player config": everything the plugin can change about a single player in a
/// replay. Every section is optional, so a preset can hold a full look or just a camera.
struct PlayerPreset
{
    std::string name;

    std::optional<pluginsdk::Loadout> loadout;
    /// An empty string means "no custom decal", which is not the same as carrying no decal
    /// section at all: the first clears a decal the player has, the second leaves it alone.
    std::optional<std::string> custom_decal_name;
    std::optional<CameraPreset> camera;
    /// An empty string means "put the original name back".
    std::optional<std::string> player_name;
    /// An empty string means "remove the title".
    std::optional<std::string> title_id;

    [[nodiscard]] bool IsEmpty() const;
};

/// Which sections of a preset to write to a player, or to store when saving one.
struct PlayerPresetSelection
{
    bool loadout = true;
    bool custom_decal = true;
    bool camera = true;
    bool player_name = true;
    bool title = true;

    /// Ticks exactly the sections the preset actually carries.
    [[nodiscard]] static PlayerPresetSelection ForPreset(const PlayerPreset& preset);
};

/// Drops every section not ticked in `selection`, so `preset` only holds what the user asked
/// to store.
[[nodiscard]] PlayerPreset FilterPreset(const PlayerPreset& preset, const PlayerPresetSelection& selection);

[[nodiscard]] json PlayerPresetToJson(const PlayerPreset& preset);
/// Throws if the json is not a preset. `fallback_name` is used when the file carries no name.
[[nodiscard]] PlayerPreset PlayerPresetFromJson(const json& j, const std::string& fallback_name);
