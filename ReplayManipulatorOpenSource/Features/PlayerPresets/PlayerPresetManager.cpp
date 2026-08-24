#include "pch.h"
#include "PlayerPresetManager.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <ranges>

#include "ImguiUtils.h"
#include "Features/Items/Items.h"

namespace
{
constexpr auto kPresetExtension = ".rmpreset";

/// How many configs the list shows before it starts scrolling. Keeps the block from pushing
/// the loadout editor off screen once a lot of them are saved.
constexpr int kMaxVisibleRows = 8;
}

PlayerPresetManager::PlayerPresetManager(std::filesystem::path folder, std::shared_ptr<Items> items)
    : folder_(std::move(folder)),
      items_(std::move(items))
{
    Refresh();
}

void PlayerPresetManager::Refresh()
{
    presets_.clear();

    if (!exists(folder_))
    {
        LOG("Player preset folder does not exist yet: {}", folder_.string());
        return;
    }

    for (const auto& entry : std::filesystem::directory_iterator(folder_))
    {
        const auto& path = entry.path();
        if (!is_regular_file(entry) || path.extension() != kPresetExtension)
        {
            continue;
        }

        try
        {
            std::ifstream file(path);
            if (!file.good())
            {
                throw std::runtime_error("the file could not be opened");
            }
            json j;
            file >> j;
            presets_.push_back(PlayerPresetFromJson(j, path.stem().string()));
        }
        catch (const std::exception& e)
        {
            LOG("Skipping the player preset {}: {}", path.filename().string(), e.what());
        }
    }

    std::ranges::sort(presets_, [](const PlayerPreset& a, const PlayerPreset& b) {
        return a.name < b.name;
    });

    LOG("Loaded {} player presets from {}", presets_.size(), folder_.string());
}

const PlayerPreset* PlayerPresetManager::Find(const std::string& preset_name) const
{
    const auto it = std::ranges::find_if(presets_, [&preset_name](const PlayerPreset& preset) {
        return preset.name == preset_name;
    });
    return it == presets_.end() ? nullptr : &(*it);
}

bool PlayerPresetManager::Save(const PlayerPreset& preset)
{
    if (preset.name.empty() || preset.IsEmpty())
    {
        return false;
    }

    try
    {
        create_directories(folder_);
        const auto path = PathFor(preset.name);
        std::ofstream file(path, std::ios::trunc);
        if (!file.good())
        {
            throw std::runtime_error("the file could not be opened for writing");
        }
        file << PlayerPresetToJson(preset).dump(4);
    }
    catch (const std::exception& e)
    {
        LOG("Failed to save the player preset {}: {}", preset.name, e.what());
        return false;
    }

    Refresh();
    return true;
}

bool PlayerPresetManager::Delete(const std::string& preset_name)
{
    try
    {
        if (!std::filesystem::remove(PathFor(preset_name)))
        {
            return false;
        }
    }
    catch (const std::exception& e)
    {
        LOG("Failed to delete the player preset {}: {}", preset_name, e.what());
        return false;
    }

    Refresh();
    return true;
}

std::optional<PlayerPresetManager::ApplyRequest> PlayerPresetManager::DrawForPlayer(const PlayerPreset& current)
{
    std::optional<ApplyRequest> request;

    DrawSaveRow(current);
    ImGui::Separator();

    if (presets_.empty())
    {
        ImGui::TextDisabled("No saved config yet. Set a player up the way you like it, give it a name above "
                            "and hit Save.");
        if (ImGui::Button("Reload from disk"))
        {
            Refresh();
        }
        return request;
    }

    // Resolved before the list is drawn, so a click that changes the selection reshapes the
    // block on the next frame rather than mid-layout.
    const auto* selected = Find(selected_preset_);

    if (selected == nullptr)
    {
        // Nothing to describe, so the details pane does not exist at all and the list gets the
        // full width. Clicking the open config again brings you back here.
        request = DrawPresetList();
    }
    else
    {
        ImGui::Columns(2, "player_preset_columns", false);
        ImGui::SetColumnWidth(0, 220);

        request = DrawPresetList();

        ImGui::NextColumn();

        // Drawn straight into the column instead of into a child window: the description needs
        // however many lines it needs, and a child would have to be given a height up front.
        // The columns block ends up as tall as whichever side is taller.
        DrawPresetSummary(*selected);

        ImGui::Columns(1);
    }

    std::string to_delete;
    if (selected != nullptr)
    {
        ImGui::TextUnformatted("Apply only:");
        ImGui::SameLine();
        DrawSectionCheckbox("Loadout##apply", selected->loadout.has_value(), &apply_selection_.loadout);
        ImGui::SameLine();
        DrawSectionCheckbox("Decal##apply", selected->custom_decal_name.has_value(), &apply_selection_.custom_decal);
        ImGui::SameLine();
        DrawSectionCheckbox("Camera##apply", selected->camera.has_value(), &apply_selection_.camera);
        ImGui::SameLine();
        DrawSectionCheckbox("Name##apply", selected->player_name.has_value(), &apply_selection_.player_name);
        ImGui::SameLine();
        DrawSectionCheckbox("Title##apply", selected->title_id.has_value(), &apply_selection_.title);

        if (ImGui::Button("Apply the ticked parts"))
        {
            request = ApplyRequest{*selected, apply_selection_};
            status_ = std::format("Applied \"{}\"", selected->name);
        }
        ImGui::SameLine();
        if (ImGui::Button("Delete this config"))
        {
            to_delete = selected->name;
        }
        ImGui::SameLine();
    }

    if (ImGui::Button("Reload from disk"))
    {
        Refresh();
    }

    if (!to_delete.empty())
    {
        // Refresh() inside Delete() invalidates `selected`, so this runs after the last use.
        status_ = Delete(to_delete)
                      ? std::format("Deleted \"{}\"", to_delete)
                      : std::format("Could not delete \"{}\"", to_delete);
        selected_preset_.clear();
    }

    if (!status_.empty())
    {
        ImGui::TextDisabled("%s", status_.c_str());
    }

    return request;
}

std::optional<PlayerPresetManager::ApplyRequest> PlayerPresetManager::DrawPresetList()
{
    std::optional<ApplyRequest> request;

    ImGui::BeginChild("PresetList", {-1, ListHeight()});
    for (const auto& preset : presets_)
    {
        ImGui::ScopeId const preset_scope{preset.name};
        // One click, everything the config holds. The checkboxes below are there for the times
        // you only want a part of it.
        if (ImGui::SmallButton("Apply"))
        {
            request = ApplyRequest{preset, PlayerPresetSelection::ForPreset(preset)};
            status_ = std::format("Applied \"{}\"", preset.name);
        }
        ImGui::SameLine();
        const auto is_open = preset.name == selected_preset_;
        if (ImGui::Selectable(preset.name.c_str(), is_open))
        {
            // Clicking the open config again folds its details away.
            selected_preset_ = is_open ? "" : preset.name;
            apply_selection_ = PlayerPresetSelection::ForPreset(preset);
        }
    }
    ImGui::EndChild();

    return request;
}

float PlayerPresetManager::ListHeight() const
{
    const auto rows = std::min(static_cast<int>(presets_.size()), kMaxVisibleRows);
    return static_cast<float>(rows) * ImGui::GetFrameHeightWithSpacing();
}

void PlayerPresetManager::DrawSaveRow(const PlayerPreset& current)
{
    ImGui::SetNextItemWidth(200);
    ImGui::InputText("Config name", &new_preset_name_);
    ImGui::SameLine();
    HelpMarker("Saves this player exactly as they look right now under that name. Reusing the name of an "
               "existing config overwrites it.");

    ImGui::TextUnformatted("Store:");
    ImGui::SameLine();
    DrawSectionCheckbox("Loadout##save", current.loadout.has_value(), &save_selection_.loadout);
    ImGui::SameLine();
    DrawSectionCheckbox("Decal##save", current.custom_decal_name.has_value(), &save_selection_.custom_decal);
    ImGui::SameLine();
    DrawSectionCheckbox("Camera##save", current.camera.has_value(), &save_selection_.camera);
    ImGui::SameLine();
    DrawSectionCheckbox("Name##save", current.player_name.has_value(), &save_selection_.player_name);
    ImGui::SameLine();
    DrawSectionCheckbox("Title##save", current.title_id.has_value(), &save_selection_.title);

    auto to_save = FilterPreset(current, save_selection_);
    to_save.name = new_preset_name_;

    const auto overwrites = Find(new_preset_name_) != nullptr;
    {
        ImGui::Disable const disable_if_nothing_to_save{new_preset_name_.empty() || to_save.IsEmpty()};
        if (ImGui::Button(overwrites ? "Overwrite" : "Save"))
        {
            status_ = Save(to_save)
                          ? std::format("Saved \"{}\"", to_save.name)
                          : std::format("Could not save \"{}\", see the BakkesMod console (F6)", to_save.name);
            selected_preset_ = to_save.name;
        }
    }
}

void PlayerPresetManager::DrawPresetSummary(const PlayerPreset& preset) const
{
    // A section that is present but empty means "clear this on the player". There is no text to
    // show for it, so it gets no line -- the Apply checkboxes below are what tells you it is in
    // there.
    if (preset.player_name && !preset.player_name->empty())
    {
        ImGui::Text("Name: %s", preset.player_name->c_str());
    }
    if (preset.title_id && !preset.title_id->empty())
    {
        ImGui::Text("Title: %s", preset.title_id->c_str());
    }
    if (preset.custom_decal_name && !preset.custom_decal_name->empty())
    {
        ImGui::Text("Custom decal: %s", preset.custom_decal_name->c_str());
    }
    if (preset.camera)
    {
        const auto& settings = preset.camera->settings;
        ImGui::Text("Camera%s: FOV %.0f, distance %.0f, height %.0f, angle %.0f",
                    preset.camera->enabled ? "" : " (override off)",
                    settings.FOV, settings.Distance, settings.Height, settings.Pitch);
    }
    if (preset.loadout && items_)
    {
        // A replay carries every equip slot the game has, cosmetic or not, and a slot the
        // player has nothing in resolves to a nameless default item. On any given player that
        // is most of them, so the lines are collected first and the header is skipped when
        // none of them turned out to be worth showing.
        std::vector<std::string> lines;
        for (const auto& [slot, item] : preset.loadout->items)
        {
            try
            {
                const auto& item_name = items_->GetItemOrDefaultData(item.product_id, slot).name;
                if (item_name.empty())
                {
                    continue;
                }
                lines.push_back(std::format("{}: {}", items_->GetSlot(slot).label, item_name));
            }
            catch (const std::exception&)
            {
                // The ids come off disk, so a slot or product the game no longer knows is a
                // normal thing to hit rather than a bug. Items throws in that case.
                lines.push_back(std::format("slot {}: item {}", static_cast<int>(slot), item.product_id));
            }
        }

        if (!lines.empty())
        {
            ImGui::TextUnformatted("Loadout:");
            ImGui::Indent();
            for (const auto& line : lines)
            {
                ImGui::TextUnformatted(line.c_str());
            }
            ImGui::Unindent();
        }
    }
}

void PlayerPresetManager::DrawSectionCheckbox(const char* label, const bool preset_has_section, bool* selected)
{
    if (!preset_has_section)
    {
        *selected = false;
    }
    ImGui::Disable const disable_if_absent{!preset_has_section};
    ImGui::Checkbox(label, selected);
}

std::filesystem::path PlayerPresetManager::PathFor(const std::string& preset_name) const
{
    return folder_ / (SanitizeFileName(preset_name) + kPresetExtension);
}

std::string PlayerPresetManager::SanitizeFileName(const std::string& preset_name)
{
    std::string sanitized;
    sanitized.reserve(preset_name.size());
    for (const auto c : preset_name)
    {
        const auto uc = static_cast<unsigned char>(c);
        const auto keep = std::isalnum(uc) != 0 || c == ' ' || c == '-' || c == '_';
        sanitized.push_back(keep ? c : '_');
    }

    // Trailing spaces and dots make a file name Windows refuses to open.
    while (!sanitized.empty() && (sanitized.back() == ' ' || sanitized.back() == '.'))
    {
        sanitized.pop_back();
    }

    return sanitized.empty() ? "preset" : sanitized;
}
