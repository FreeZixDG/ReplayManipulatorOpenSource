#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "PlayerPreset.h"

class Items;

/// Stores and restores complete player configs (loadout, custom decal, camera, name, title)
/// as files on disk, so a look you built once can be dropped onto a player in any replay in
/// a single click instead of being rebuilt by hand every time.
///
/// The manager owns the files and the widgets; it never touches the game. Applying is left to
/// the caller, which gets an ApplyRequest back from DrawForPlayer and writes it to the player.
class PlayerPresetManager
{
public:
    PlayerPresetManager(std::filesystem::path folder, std::shared_ptr<Items> items);

    /// Re-reads the folder. Unreadable files are logged and skipped, never thrown.
    void Refresh();

    [[nodiscard]] const std::vector<PlayerPreset>& GetPresets() const { return presets_; }
    [[nodiscard]] const PlayerPreset* Find(const std::string& preset_name) const;

    bool Save(const PlayerPreset& preset);
    bool Delete(const std::string& preset_name);

    struct ApplyRequest
    {
        PlayerPreset preset;
        PlayerPresetSelection selection;
    };

    /// Draws the preset widgets for one player. `current` is that player as they look right
    /// now, which is what the Save button stores. Returns a request on the frame the user
    /// asks for a preset to be put on the player, and nothing on every other frame.
    [[nodiscard]] std::optional<ApplyRequest> DrawForPlayer(const PlayerPreset& current);

private:
    /// Draws the config list. Returns a request when a row's one-click Apply was pressed.
    [[nodiscard]] std::optional<ApplyRequest> DrawPresetList();
    /// Tall enough for every saved config, up to the point where the list starts scrolling.
    [[nodiscard]] float ListHeight() const;
    void DrawSaveRow(const PlayerPreset& current);
    void DrawPresetSummary(const PlayerPreset& preset) const;
    /// Checkbox that ties itself off and greys out when the preset has nothing for it.
    static void DrawSectionCheckbox(const char* label, bool preset_has_section, bool* selected);

    [[nodiscard]] std::filesystem::path PathFor(const std::string& preset_name) const;
    static std::string SanitizeFileName(const std::string& preset_name);

    std::filesystem::path folder_;
    std::shared_ptr<Items> items_;
    std::vector<PlayerPreset> presets_;

    // Shared across the player tabs on purpose: you pick a config once, then click through
    // the players you want it on.
    std::string selected_preset_;
    std::string new_preset_name_;
    PlayerPresetSelection save_selection_{};
    PlayerPresetSelection apply_selection_{};
    std::string status_;
};
