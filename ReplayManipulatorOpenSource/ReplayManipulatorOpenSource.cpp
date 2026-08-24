#include "pch.h"
#include "ReplayManipulatorOpenSource.h"

#include "ImguiUtils.h"
#include "Data/PriData.h"
#include "Features/TextureCache.h"
#include "bakkesmod/utilities/LoadoutUtilities.h"
#include "bakkesmod/wrappers/GameObject/MeshComponents/CarMeshComponentBaseWrapper.h"
#include "Features/BallHide/BallHiderAndDecals.h"
#include "Features/CamPathsManager/CamPathsManager.h"
#include "Features/Camera/CameraFocus.h"
#include "Features/Camera/CameraSettingsOverride.h"
#include "Features/CarRotator/CarRotator.h"
#include "Features/Credits/Credits.h"
#include "Features/CustomTextures/CustomTextures.h"
#include "Features/Items/ItemPaints.h"
#include "Features/Items/Items.h"
#include "Features/Items/LoadoutEditor.h"
#include "Features/Items/PaintFinishColors.h"
#include "Features/MapChange/ReplayMapChanger.h"
#include "Features/Names/PlayerRenamer.h"
#include "Features/Names/PlayerTitleChanger.h"
#include "Features/PlayerPresets/PlayerPresetManager.h"
#include "Features/ReplayManager/ReplayManager.h"
#include "Features/SlowMotionTransitionFixer/Stfu.h"
#include "Features/StadiumColors/StadiumManager.h"
#include "Framework/GuiFeatureBase.h"

BAKKESMOD_PLUGIN(ReplayManipulatorOpenSource, "write a plugin description here", plugin_version, PLUGINTYPE_FREEPLAY)

// ReSharper disable once CppInconsistentNaming
std::shared_ptr<CVarManagerWrapper> _globalCvarManager;

void ReplayManipulatorOpenSource::onLoad()
{
    _globalCvarManager = cvarManager;

    InitUtilityModules();

    stadium_manager_ = CreateModule<StadiumManager>(gameWrapper, cvarManager);
    replay_manager_ = CreateModule<ReplayManager>(gameWrapper, cvarManager);
    camera_focus_ = std::make_shared<CameraFocus>(gameWrapper);
    slowmotion_fixer_ = CreateModule<SlowmotionTransitionFixerUtility>(gameWrapper);
    camera_settings_ = std::make_shared<CameraSettingsOverride>(gameWrapper);
    map_changer_ = CreateModule<ReplayMapChanger>(gameWrapper);

    auto custom_ball_decals_folder = gameWrapper->GetDataFolder() / "acplugin" / "BallTextures";
    ball_hider_ = CreateModule<BallHiderAndDecals>(gameWrapper, texture_cache_, custom_ball_decals_folder);
    player_rename_ = std::make_shared<PlayerRenamer>(gameWrapper);
    player_title_ = std::make_shared<PlayerTitleChanger>(gameWrapper);
    dollycam_manager_ = CreateModule<CamPathsManager>(gameWrapper, cvarManager,
                                                      gameWrapper->GetDataFolder() / "campaths");
    credits_ = CreateModule<CreditsInSettings>(gameWrapper);

    items_ = std::make_shared<Items>(gameWrapper);
    paint_finish_colors_ = std::make_shared<PaintFinishColors>();
    item_paints_ = std::make_shared<ItemPaints>(gameWrapper);

    loadout_editor_ = std::make_shared<LoadoutEditor>(items_, paint_finish_colors_, item_paints_);

    loadout_editor_->LoadProductIcons(gameWrapper->GetDataFolder() / "ReplayManipulatorOS" / "slots");

    player_presets_ = std::make_shared<PlayerPresetManager>(
        gameWrapper->GetDataFolder() / "ReplayManipulatorOS" / "player_configs", items_);

    gameWrapper->HookEventPost("Function TAGame.GameInfo_Replay_TA.HandleReplayImported", [this](...) {
        gameWrapper->HookEventPost("Function TAGame.GameInfo_Replay_TA.EventGameEventSet", [this](...) {
            OnReplayOpen();
            gameWrapper->UnhookEventPost("Function TAGame.GameInfo_Replay_TA.EventGameEventSet");
        });
    });

    gameWrapper->HookEvent("Function TAGame.GFxHUD_Replay_TA.Destroyed", [this](...) {
        OnReplayClose();
    });

    if (gameWrapper->IsInReplay())
    {
        OnReplayOpen();
    }

    ReadJsons();
}

void ReplayManipulatorOpenSource::InitUtilityModules()
{
    texture_cache_ = std::make_shared<TextureCache>(gameWrapper);
    //event_dispatcher_ = std::make_shared<BakkesModEventDispatcher>(this);
}


void ReplayManipulatorOpenSource::OnReplayOpen()
{
    auto game_event = gameWrapper->GetGameEventAsReplay();
    if (!game_event)
    {
        return;
    }
    map_changer_->UpdateCurrentMap();
    auto replay = game_event.GetReplay();
    if (!replay)
    {
        return;
    }
    auto replay_id = replay.GetId().ToString();
    if (replay_id == current_replay_id_)
    {
        LOG("Same replay reloaded");
    }
    else
    {
        LOG("Loading replay with ID: {}", replay_id);
        replay_players_.clear();
        replay_players_originals_.clear();
        player_rename_->ResetNameOverrides();
        player_title_->ResetTitleOverrides();
    }
    current_replay_id_ = replay_id;
    current_replay_name_ = replay.GetReplayName().ToString();

    auto lodout_set_cb = [this](PriWrapper&& pri, void*, const std::string& name) {
        OnPriLoadoutSet(pri);
    };
    gameWrapper->HookEventWithCaller<PriWrapper>("Function TAGame.PRI_TA.OnLoadoutsSetInternal", lodout_set_cb);
    gameWrapper->HookEventWithCaller<PriWrapper>("Function TAGame.PRI_TA.OnLoadoutsSet", lodout_set_cb);
    gameWrapper->HookEventWithCaller<PriWrapper>("Function TAGame.PRI_TA.HandleLoadoutLoaded", lodout_set_cb);
    gameWrapper->HookEventWithCaller<PriWrapper>("Function TAGame.PRI_TA.UpdateFromLoadout", lodout_set_cb);

    gameWrapper->HookEventWithCallerPost<ActorWrapper>("Function TAGame.CarMeshComponentBase_TA.InitMaterials",
                                                       [this](const ActorWrapper& caller, ...) {
                                                           OnMaterialInit(
                                                               CarMeshComponentBaseWrapper{caller.memory_address});
                                                       });

    gameWrapper->HookEventWithCaller<CarWrapper>("Function TAGame.Car_TA.UpdateTeamLoadout",
                                                 [this](CarWrapper&& car, ...) {
                                                     if (auto pri = car.GetPRI())
                                                     {
                                                         OnPriLoadoutSet(pri);
                                                     }
                                                 });

    gameWrapper->HookEventWithCaller<ActorWrapper>("Function TAGame.CarMeshComponentBase_TA.SetMeshMaterialColors",
                                                   [this](ActorWrapper&& car_mesh, ...) {
                                                       if (auto car = CarMeshComponentBaseWrapper(car_mesh.memory_address).GetCar())
                                                       {
                                                           OnSetMeshMaterialColors(car);
                                                       }
                                                   });

    gameWrapper->HookEvent("Function TAGame.PlayerInput_TA.PlayerInput", [this](...) {
        CameraLock();
    });

    gameWrapper->SetTimeout([this](...) {
        RefreshPriData();
    }, 2);
}

void ReplayManipulatorOpenSource::OnReplayClose() const
{
    gameWrapper->UnhookEvent("Function TAGame.PRI_TA.OnLoadoutsSetInternal");
    gameWrapper->UnhookEvent("Function TAGame.PRI_TA.OnLoadoutsSet");
    gameWrapper->UnhookEvent("Function TAGame.PRI_TA.HandleLoadoutLoaded");
    gameWrapper->UnhookEvent("Function TAGame.PRI_TA.UpdateFromLoadout");
    gameWrapper->UnhookEvent("Function TAGame.Car_TA.UpdateTeamLoadout");
    gameWrapper->UnhookEvent("Function TAGame.PlayerInput_TA.PlayerInput");
    gameWrapper->UnhookEventPost("Function TAGame.CarMeshComponentBase_TA.InitMaterials");
    gameWrapper->UnhookEvent("Function TAGame.CarMeshComponentBase_TA.SetMeshMaterialColors");
}

void ReplayManipulatorOpenSource::OnGameThread(std::function<void()>&& func) const
{
    gameWrapper->Execute([func = std::move(func)](...) {
        func();
    });
}

void ReplayManipulatorOpenSource::RenderSettings()
{
    if (ImGui::Button("open window"))
    {
        OnGameThread([this] {
            cvarManager->executeCommand(std::format("openmenu {}", GetMenuName()));
        });
    }
    for (const auto& gui_feature_base : gui_features_)
    {
        if (gui_feature_base->ShouldDrawPluginSettings())
        {
            if (ImGui::CollapsingHeader(gui_feature_base->GetName().c_str()))
            {
                ImGui::Indent();
                ImGui::ScopeId const module_scope{gui_feature_base->GetName()};
                gui_feature_base->Render();
                ImGui::Unindent();
            }
        }
    }
}

void ReplayManipulatorOpenSource::DrawPriData(PriData& pri)
{
    if (ImGui::Button("Reset this player"))
    {
        ResetPlayer(pri);
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset all players"))
    {
        ResetAllPlayers();
    }
    ImGui::SameLine();
    HelpMarker("Puts the loadout, name, title, camera and hidden state back the way the replay has them, "
               "and stops applying the custom decal.\n\n"
               "The plugin keeps your edits when you leave and reopen the same replay, so this is how you "
               "start a player over without switching to another replay. It changes nothing on disk: your "
               "saved configs are untouched.\n\n"
               "A custom decal already drawn on a car only goes away once the game rebuilds the car mesh, "
               "the same as picking \"None\" in the decal list below.");
    ImGui::Separator();

    if (player_presets_ && ImGui::CollapsingHeader("Saved player configs"))
    {
        ImGui::Indent();
        // The capture has to happen every frame anyway: it is what the Save button stores, and
        // it also tells the widgets which parts this player currently has something to store.
        if (const auto request = player_presets_->DrawForPlayer(CapturePreset(pri)))
        {
            ApplyPreset(pri, request->preset, request->selection);
        }
        ImGui::Unindent();
        ImGui::Separator();
    }

    if (player_rename_)
    {
        static std::string new_name;
        ImGui::SetNextItemWidth(150);
        ImGui::InputText("New name", &new_name);
        ImGui::SameLine();
        {
            ImGui::Disable const disable_if_no_name{new_name.empty()};
            if (ImGui::Button("Change the name"))
            {
                gameWrapper->Execute([this, pri, name = new_name](...) {
                    auto pri_wrapper = GetPriWrapper(pri);
                    player_rename_->Rename(pri_wrapper, name);
                });
            }
        }
        {
            ImGui::Disable const disable_if_not_changed{!player_rename_->IsInRenameCache(pri.uid)};
            ImGui::SameLine();
            if (ImGui::Button("Restore"))
            {
                gameWrapper->Execute([this, pri](...) {
                    auto pri_wrapper = GetPriWrapper(pri);
                    player_rename_->Restore(pri_wrapper);
                });
            }
        }
    }

    if (player_title_ && !player_title_->IsUsable())
    {
        ImGui::TextUnformatted("Title editing is unavailable on this Rocket League build");
        ImGui::SameLine();
        HelpMarker("The game stores the player title in a field the BakkesMod SDK does not "
                   "expose, so the plugin reads it at a hard-coded offset. That offset no "
                   "longer matches this build of Rocket League, so title editing turned itself "
                   "off rather than write to the wrong place. See the BakkesMod console (F6) "
                   "for the details.");
    }
    else if (player_title_)
    {
        const auto current_title = player_title_->GetDisplayedTitleId(pri.uid);
        ImGui::Text("Current title: %s", current_title.empty() ? "(none)" : current_title.c_str());
        ImGui::SameLine();
        HelpMarker("Rocket League identifies a title by an internal id, not by the text you see "
                   "on the scoreboard. There is no way to list every id the game knows, so the "
                   "dropdown only offers ids seen on players in the replays you have opened. "
                   "Open a replay containing a player who has the title you want, and its id "
                   "shows up here.");

        static std::string new_title_id;
        ImGui::SetNextItemWidth(200);
        ImGui::InputText("Title id", &new_title_id);

        const auto& known_ids = player_title_->GetKnownTitleIds();
        if (!known_ids.empty())
        {
            ImGui::SetNextItemWidth(200);
            if (ImGui::BeginCombo("Title ids seen so far", "Pick one"))
            {
                for (const auto& known_id : known_ids)
                {
                    if (ImGui::Selectable(known_id.c_str(), known_id == new_title_id))
                    {
                        new_title_id = known_id;
                    }
                }
                ImGui::EndCombo();
            }
        }

        {
            ImGui::Disable const disable_if_no_title{new_title_id.empty()};
            if (ImGui::Button("Change the title"))
            {
                gameWrapper->Execute([this, pri, title_id = new_title_id](...) {
                    auto pri_wrapper = GetPriWrapper(pri);
                    player_title_->SetTitle(pri_wrapper, title_id);
                });
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Remove the title"))
        {
            gameWrapper->Execute([this, pri](...) {
                auto pri_wrapper = GetPriWrapper(pri);
                player_title_->SetTitle(pri_wrapper, {});
            });
        }
        {
            ImGui::Disable const disable_if_not_changed{!player_title_->IsInTitleCache(pri.uid)};
            ImGui::SameLine();
            if (ImGui::Button("Restore##title"))
            {
                gameWrapper->Execute([this, pri](...) {
                    auto pri_wrapper = GetPriWrapper(pri);
                    player_title_->Restore(pri_wrapper);
                });
            }
        }
    }

    if (ImGui::Checkbox("Hidden", &pri.hidden))
    {
        OnGameThread([this, pri]() mutable {
            ApplyCarHiddenState(pri);
        });
    }
    ImGui::SameLine();
    if (ImGui::Button("Focus camera"))
    {
        OnGameThread([this, id = pri.uid.pri_id_string] {
            camera_focus_->FocusCameraOnPlayer(id);
        });
    }

    auto loadout_changed = false;
    loadout_changed |= loadout_editor_->DrawLoadoutEditor(pri.loadout, pri.team);

    char input_buffer3[64] = "";
    std::string preview = pri.custom_decal.name;
    if (preview.empty())
    {
        preview = "Select a decal";
    }
    if (ImGui::BeginSearchableCombo("CustomDecal", preview.c_str(), input_buffer3, 64, "search for the decal name"))
    {
        if (ImGui::Selectable("None", false))
        {
            pri.custom_decal = CustomTextures::default_decal_;
            loadout_changed = true;
        }
        for (auto& [name, val] : custom_decal_configs_.decal_config)
        {
            auto name2 = name + (val.has_invalid_paths ? " (has invalid paths)" : "");
            if (ImGui::Selectable(name2.c_str(), false))
            {
                pri.custom_decal = FindCustomDecal(name);
                int& body_id = pri.loadout.items[pluginsdk::Equipslot::BODY].product_id;
                int& skin_id = pri.loadout.items[pluginsdk::Equipslot::DECAL].product_id;
                if (body_id == pri.custom_decal.BodyID && skin_id == pri.custom_decal.SkinID)
                {
                    OnGameThread([this, &pri] {
                        ApplyDecal(pri);
                    });
                }
                else
                {
                    body_id = pri.custom_decal.BodyID;
                    skin_id = pri.custom_decal.SkinID;
                    loadout_changed = true;
                }
            }
        }
        ImGui::EndSearchableCombo();
    }

    if (loadout_changed)
    {
        OnGameThread([this, pri] {
            LOG("loadout changed");
            UpdateLoadout(pri);
        });
    }

    ImGui::Text("Rotate car");
    static std::vector const rotate_offsets = {-90.0f, -30.0f, -10.0f, 10.0f, 30.0f, 90.0f};
    for (auto& rotation : rotate_offsets)
    {
        ImGui::SameLine();
        if (ImGui::Button(std::format("{:+}", rotation).c_str()))
        {
            OnGameThread([this, pri, rotation] {
                if (auto pri_ta = GetPriWrapper(pri))
                {
                    CarRotator::RotateCarOfPri(pri_ta, rotation);
                }
            });
        }
    }

    auto cam_setting = camera_settings_->GetCameraOverrideSettings(pri.uid);

    if (camera_settings_->RenderCameraOverride(cam_setting))
    {
        OnGameThread([this, pri_data = pri, cam_setting] {
            if (const auto pri_wrapper = GetPriWrapper(pri_data))
            {
                camera_settings_->SetCameraOverrideSettings(pri_data.uid, cam_setting);
                camera_settings_->SetPriCameraSetting(pri_wrapper, cam_setting.GetCameraSettings());
            }
        });
    }
}

PlayerPreset ReplayManipulatorOpenSource::CapturePreset(const PriData& pri) const
{
    PlayerPreset preset;

    preset.loadout = pri.loadout;
    preset.custom_decal_name = pri.custom_decal.name;

    // PriData::player_name is a snapshot taken shortly after the replay opened and never
    // refreshed, so it still holds the original name after a rename. The renamer's own cache
    // is the only place that knows what the player is actually called now.
    preset.player_name = pri.player_name;
    if (player_rename_)
    {
        if (auto renamed_to = player_rename_->GetOverriddenName(pri.uid); !renamed_to.empty())
        {
            preset.player_name = renamed_to;
        }
    }

    // Before the game has replicated the camera there is nothing to store but zeroes, which
    // would read back as a 0 FOV camera glued to the car.
    if (camera_settings_ && camera_settings_->HasCameraOverride(pri.uid))
    {
        const auto camera_override = camera_settings_->GetCameraOverrideSettings(pri.uid);
        preset.camera = CameraPreset{camera_override.enabled, camera_override.override_settings};
    }

    if (player_title_ && player_title_->IsUsable())
    {
        preset.title_id = player_title_->GetDisplayedTitleId(pri.uid);
    }

    return preset;
}

void ReplayManipulatorOpenSource::ApplyPreset(PriData& pri, const PlayerPreset& preset,
                                              const PlayerPresetSelection& what)
{
    auto loadout_changed = false;

    if (what.loadout && preset.loadout)
    {
        pri.loadout = *preset.loadout;
        loadout_changed = true;
    }

    if (what.custom_decal && preset.custom_decal_name)
    {
        if (preset.custom_decal_name->empty())
        {
            pri.custom_decal = CustomTextures::default_decal_;
        }
        else
        {
            pri.custom_decal = FindCustomDecal(*preset.custom_decal_name);
            // A custom decal only shows on the body and skin it was authored for. Same fix-up
            // the decal combo below does when you pick one by hand.
            if (pri.custom_decal.BodyID >= 0 && pri.custom_decal.SkinID >= 0)
            {
                auto& body = pri.loadout.items[pluginsdk::Equipslot::BODY];
                body.slot = pluginsdk::Equipslot::BODY;
                body.product_id = pri.custom_decal.BodyID;

                auto& skin = pri.loadout.items[pluginsdk::Equipslot::DECAL];
                skin.slot = pluginsdk::Equipslot::DECAL;
                skin.product_id = pri.custom_decal.SkinID;
            }
        }
        loadout_changed = true;
    }

    // One hop to the game thread for everything, because the order matters: the title lives in
    // the loadout, so writing the loadout wipes it and it has to go last.
    OnGameThread([this, pri_data = pri, preset, what, loadout_changed] {
        auto pri_wrapper = GetPriWrapper(pri_data);
        if (!pri_wrapper)
        {
            return;
        }

        if (loadout_changed)
        {
            UpdateLoadout(pri_data);
        }

        if (what.camera && preset.camera && camera_settings_)
        {
            camera_settings_->ApplyCameraOverride(pri_wrapper, preset.camera->enabled, preset.camera->settings);
        }

        if (what.player_name && preset.player_name && player_rename_)
        {
            if (preset.player_name->empty())
            {
                player_rename_->Restore(pri_wrapper);
            }
            else
            {
                player_rename_->Rename(pri_wrapper, *preset.player_name);
            }
        }

        if (what.title && preset.title_id && player_title_ && player_title_->IsUsable())
        {
            player_title_->SetTitle(pri_wrapper, *preset.title_id);
        }
    });
}

void ReplayManipulatorOpenSource::ResetPlayer(PriData& pri)
{
    // The originals are the snapshot taken when the replay was first read, so they are the
    // only record of what the replay itself holds. Without one there is no loadout to put back.
    if (const auto* original = GetOriginalPriData(pri.uid))
    {
        pri.loadout = original->loadout;
        pri.hidden = original->hidden;
    }
    // The SDK can apply a decal but not take one off, so all we can do is stop re-applying it.
    // The car keeps showing it until the game rebuilds the mesh, exactly like picking "None".
    pri.custom_decal = CustomTextures::default_decal_;

    OnGameThread([this, pri_data = pri] {
        auto pri_wrapper = GetPriWrapper(pri_data);
        if (!pri_wrapper)
        {
            return;
        }

        UpdateLoadout(pri_data);
        ApplyCarHiddenState(pri_data);

        if (camera_settings_)
        {
            camera_settings_->ResetCameraOverride(pri_wrapper);
        }
        if (player_rename_)
        {
            player_rename_->Restore(pri_wrapper);
        }
        // The loadout carries the title, so restoring the loadout above wiped it. Title last.
        if (player_title_)
        {
            player_title_->Restore(pri_wrapper);
        }
    });
}

void ReplayManipulatorOpenSource::ResetAllPlayers()
{
    // Only touches the fields of existing entries, so this is safe to call while RenderWindow
    // is iterating replay_players_.
    for (auto& player : replay_players_)
    {
        ResetPlayer(player);
    }
}

void ReplayManipulatorOpenSource::RenderWindow()
{
    ImGuiTabBarFlags constexpr tab_bar_flags = ImGuiTabBarFlags_None;
    if (ImGui::BeginTabBar("Players", tab_bar_flags))
    {
        for (auto& player : replay_players_)
        {
            ImGui::ScopeId const scoped_player_id{player.uid.pri_id_string.c_str()};
            ImGui::Disable const disable_if_spectating{player.spectating};
            auto tab_lbl = player.player_name + (player.spectating ? " (Spectating)" : "");

            if (player.team == 1)
            {
                auto swap_red_blue = [](ImGuiCol id) {
                    auto c = ImGui::GetStyleColorVec4(id);
                    std::swap(c.x, c.z);
                    return c;
                };

                ImGui::PushStyleColor(ImGuiCol_Tab, swap_red_blue(ImGuiCol_Tab));
                ImGui::PushStyleColor(ImGuiCol_TabHovered, swap_red_blue(ImGuiCol_TabHovered));
                ImGui::PushStyleColor(ImGuiCol_TabActive, swap_red_blue(ImGuiCol_TabActive));
            }

            if (ImGui::BeginTabItem(tab_lbl.c_str()))
            {
                DrawPriData(player);
                ImGui::EndTabItem();
            }

            if (player.team == 1)
            {
                ImGui::PopStyleColor(3);
            }
        }

        for (const auto& gui_feature_base : gui_features_)
        {
            if (gui_feature_base->ShouldDrawPluginWindow())
            {
                if (ImGui::BeginTabItem(gui_feature_base->GetName().c_str()))
                {
                    ImGui::ScopeId const module_scope{gui_feature_base->GetName()};
                    gui_feature_base->Render();
                    ImGui::EndTabItem();
                }
            }
        }

        ImGui::EndTabBar();
    }
}

PriData* ReplayManipulatorOpenSource::GetPriData(PriWrapper& pri)
{
    const auto it = std::ranges::find_if(replay_players_, [pri](const PriData& p)mutable {
        return p == pri;
    });
    if (it != replay_players_.end())
    {
        return &(*it);
    }

    return nullptr;
}

const PriData* ReplayManipulatorOpenSource::GetOriginalPriData(const PriUid& uid) const
{
    const auto it = std::ranges::find_if(replay_players_originals_, [&uid](const PriData& p) {
        return p.uid == uid;
    });
    if (it != replay_players_originals_.end())
    {
        return &(*it);
    }

    return nullptr;
}

void ReplayManipulatorOpenSource::OnPriLoadoutSet(PriWrapper& pri)
{
    if (!pri)
    {
        return;
    }

    ApplyLoadoutOverrides(pri);

    // The title lives in the loadout, so anything that writes a loadout wipes our override:
    // the game when scrubbing the timeline, but also ApplyLoadoutOverrides above. It has to
    // run last, which is why it sits here instead of inside ApplyLoadoutOverrides.
    player_title_->ObserveAndReapply(pri);
}

void ReplayManipulatorOpenSource::ApplyLoadoutOverrides(PriWrapper& pri)
{
    auto car = pri.GetCar();
    //if no car they're spectating. Don't care about those
    if (!car)
    {
        return;
    }
    auto* pri_data = GetPriData(pri);

    if (pri_data == nullptr)
    {
        return;
    }

    auto loadout_maybe = LoadoutUtilities::GetLoadoutFromPri(pri, pri.GetTeamNum2());
    if (!loadout_maybe)
    {
        return;
    }
    auto& [items, paint_finish] = *loadout_maybe;
    if (items != pri_data->loadout.items)
    {
        LoadoutUtilities::SetLoadoutItems(pri, pri_data->loadout.items);
    }
    if (paint_finish != pri_data->loadout.paint_finish)
    {
        LoadoutUtilities::SetLoadoutPaintFinishColors(car, pri_data->loadout.paint_finish);
    }
}

void ReplayManipulatorOpenSource::RefreshPriData()
{
    auto game_event = gameWrapper->GetGameEventAsReplay();
    if (!game_event)
    {
        return;
    }
    auto pris = game_event.GetPRIs();
    if (pris.IsNull())
    {
        return;
    }

    if (replay_players_originals_.empty())
    {
        for (auto pri : pris)
        {
            if (auto loadout = LoadoutUtilities::GetLoadoutFromPri(pri, pri.GetTeamNum2()))
            {
                replay_players_originals_.emplace_back(pri, *loadout);
            }
        }
    }

    for (auto pri : pris)
    {
        player_title_->ObserveAndReapply(pri);
        auto loadout = LoadoutUtilities::GetLoadoutFromPri(pri, pri.GetTeamNum2());
        if (!loadout)
            continue;
        if (auto* pri_data = GetPriData(pri))
        {
            pri_data->Update(pri, *loadout);
        }
        else
        {
            replay_players_.emplace_back(pri, *loadout);
        }
    }
    std::ranges::sort(replay_players_, [](PriData& a, PriData& b) {
        return std::tie(a.team, a.player_name) < std::tie(b.team, b.player_name);
    });
}


PriWrapper ReplayManipulatorOpenSource::GetPriWrapper(const PriData& pri_data) const
{
    auto game_event = gameWrapper->GetGameEventAsReplay();
    if (!game_event)
    {
        return {0};
    }
    auto pris = game_event.GetPRIs();
    if (pris.IsNull())
    {
        return {0};
    }
    for (PriWrapper pri : pris)
    {
        if (pri_data == pri)
        {
            return pri;
        }
    }
    return {0};
}

void ReplayManipulatorOpenSource::UpdateLoadout(const PriData& pri_data) const
{
    auto pri_wrapper = GetPriWrapper(pri_data);
    if (!pri_wrapper)
    {
        return;
    }

    LoadoutUtilities::ForceSetLoadout(pri_wrapper, pri_data.loadout);

    if (pri_data.custom_decal.name.empty())
        return;

    auto car = pri_wrapper.GetCar();
    if (!car)
        return;
    CustomTextures::ApplyDecalToCar(pri_data.custom_decal, car);
}

void ReplayManipulatorOpenSource::ApplyDecal(const PriData& pri_data) const
{
    auto pri_wrapper = GetPriWrapper(pri_data);
    if (!pri_wrapper)
    {
        return;
    }

    if (pri_data.custom_decal.name.empty())
        return;

    auto car = pri_wrapper.GetCar();
    if (!car)
    {
        return;
    }
    CustomTextures::ApplyDecalToCar(pri_data.custom_decal, car);
}

void ReplayManipulatorOpenSource::CameraLock() const
{
    static auto left_alt_index = gameWrapper->GetFNameIndexByString("LeftAlt");
    const auto alt_pressed = gameWrapper->IsKeyPressed(left_alt_index);
    auto should_lock = isWindowOpen_ && m_is_window_hovered;
    if (alt_pressed)
    {
        should_lock = !should_lock;
    }
    if (should_lock)
    {
        auto pc = gameWrapper->GetPlayerController();
        // Lock mouse movement
        pc.SetALookUp(0);
        pc.SetATurn(0);

        //if (const auto* real_pc = UCast<APlayerController>(gw_->GetPlayerController()))
        //{
        //	auto* pi = real_pc->PlayerInput;
        //	pi->ResetInput();
        //}
    }
}

void ReplayManipulatorOpenSource::ApplyCarHiddenState(const PriData& pri_data) const
{
    if (!gameWrapper->IsInReplay())
        return;
    auto pri_wrapper = GetPriWrapper(pri_data);
    if (!pri_wrapper)
    {
        return;
    }
    auto car_wrapper = pri_wrapper.GetCar();
    if (!car_wrapper)
    {
        return;
    }
    car_wrapper.SetHidden2(pri_data.hidden);
}

void ReplayManipulatorOpenSource::OnMaterialInit(const CarMeshComponentBaseWrapper& car_mesh_component)
{
    auto car = car_mesh_component.GetCar();
    if (!car)
    {
        return;
    }

    auto pri = car.GetPRI();
    if (!pri)
    {
        return;
    }

    if (auto* pri_data = GetPriData(pri))
    {
        ApplyDecal(*pri_data);
    }
}

void ReplayManipulatorOpenSource::OnSetMeshMaterialColors(CarWrapper& car_wrapper)
{
    if (!car_wrapper)
        return;

    auto pri = car_wrapper.GetPRI();
    if (!pri)
        return;

    auto* pri_data = GetPriData(pri);
    if (!pri_data)
        return;

    auto current_paint = LoadoutUtilities::GetPaintFinishColors(car_wrapper);
    if (current_paint != pri_data->loadout.paint_finish)
    {
        LoadoutUtilities::SetLoadoutPaintFinishColors(car_wrapper, pri_data->loadout.paint_finish);
    }
}

CustomDecal& ReplayManipulatorOpenSource::FindCustomDecal(const std::string& name)
{
    if (const auto it = loaded_custom_decals_.find(name); it == loaded_custom_decals_.end())
    {
        const auto it2 = custom_decal_configs_.decal_config.find(name);
        if (it2 == custom_decal_configs_.decal_config.end())
        {
            return CustomTextures::default_decal_;
        }
        loaded_custom_decals_[name] = CustomDecal(it2->second, texture_cache_);
    }

    return loaded_custom_decals_[name];
}

void ReplayManipulatorOpenSource::ReadJsons()
{
    custom_decal_configs_.decal_config.clear();
    const auto root_dir = gameWrapper->GetDataFolder() / "acplugin/DecalTextures";
    if (!exists(root_dir))
    {
        return;
    }
    const auto jsons = CustomTextures::FindJsons(root_dir);
    for (auto& p : jsons)
    {
        auto config = CustomTextures::ReadCustomDecalJsons(p, root_dir);
        for (auto& [key, val] : config.decal_config)
        {
            custom_decal_configs_.decal_config[key] = val;
        }
    }
}
