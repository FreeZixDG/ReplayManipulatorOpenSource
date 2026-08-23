#include "pch.h"
#include "PlayerTitleChanger.h"

#include <cstdint>

#include "Framework/StateLock.h"

namespace
{
// TAGame.PRI_TA.Title -- the FName the game resolves against TitleConfig_X to get the text
// and colours the scoreboard shows. The BakkesMod SDK exposes no accessor for it, so we read
// and write the field directly.
//
// Offset taken from https://github.com/ObscuritySRL/RocketLeagueSDK ("Refresh SDK definitions
// for v1.0.3", 2026-08-06); the Steam and Epic builds agree on it. It WILL move when Psyonix
// changes the class layout, which is what PlayerTitleChanger::ValidateLayout guards against.
constexpr std::uintptr_t kPriTitleOffset = 0x0710;

// The next fields after Title that the SDK does expose. Reading these raw and comparing them
// against the wrapper getters is how we confirm the class layout still matches these offsets.
// They all have non-zero defaults (0.5 / 1.0 / 1.0), which is what makes the check meaningful.
constexpr std::uintptr_t kPriDodgeInputThresholdOffset = 0x0728;
constexpr std::uintptr_t kPriSteeringSensitivityOffset = 0x072c;
constexpr std::uintptr_t kPriAirControlSensitivityOffset = 0x0730;

// UE3 FName: an index into the global name table plus an instance number. Index 0 is None,
// which is what a player without a title carries.
struct RlFName
{
    std::int32_t index = 0;
    std::int32_t number = 0;
};

static_assert(sizeof(RlFName) == 8, "PRI_TA.Title is an 8 byte FName");

RlFName* TitleField(const PriWrapper& pri)
{
    return reinterpret_cast<RlFName*>(pri.memory_address + kPriTitleOffset);
}

float ReadFloat(const PriWrapper& pri, const std::uintptr_t offset)
{
    return *reinterpret_cast<const float*>(pri.memory_address + offset);
}

struct FlagGuard
{
    explicit FlagGuard(bool& flag)
        : flag_(flag)
    {
        flag_ = true;
    }

    FlagGuard(const FlagGuard& other) = delete;
    FlagGuard(FlagGuard&& other) noexcept = delete;
    FlagGuard& operator=(const FlagGuard& other) = delete;
    FlagGuard& operator=(FlagGuard&& other) noexcept = delete;

    ~FlagGuard()
    {
        flag_ = false;
    }

private:
    bool& flag_;
};
}


PlayerTitleChanger::PlayerTitleChanger(std::shared_ptr<GameWrapper> gw)
    : gw_(std::move(gw)) {}


void PlayerTitleChanger::SetTitle(PriWrapper pri, const std::string& title_id)
{
    StateLock const lock;
    if (!CanChangeTitle())
    {
        LOG("player title changer is disabled");
        return;
    }
    if (!pri)
    {
        LOG("Pri is null when changing the title to '{}'", title_id);
        return;
    }

    const auto id = PriUid(pri);
    const auto p = title_cache_.find(id);
    // Keep the title the player started with, not whatever we last wrote onto them.
    const auto original = p != title_cache_.end() ? p->second.original_title_id : ReadTitleId(pri);

    if (DoSetTitle(pri, title_id))
    {
        title_cache_[id] = {original, ReadTitleId(pri)};
    }
}


void PlayerTitleChanger::Restore(PriWrapper pri)
{
    StateLock const lock;
    if (!CanChangeTitle())
    {
        LOG("player title changer is disabled");
        return;
    }
    if (!pri)
    {
        return;
    }
    const auto id = PriUid(pri);
    if (const auto p = title_cache_.find(id); p != title_cache_.end())
    {
        const auto original_title = p->second.original_title_id;
        title_cache_.erase(p);
        DoSetTitle(pri, original_title);
    }
}


void PlayerTitleChanger::ResetTitleOverrides()
{
    StateLock const lock;
    title_cache_.clear();
    observed_titles_.clear();
    // known_title_ids_ is deliberately kept: ids collected from earlier replays are the only
    // library of real title ids the user has.
}


void PlayerTitleChanger::ObserveAndReapply(PriWrapper& pri)
{
    // Fires in bursts from the loadout hooks during a seek, and Remember below writes the
    // caches the UI reads on the render thread.
    StateLock const lock;
    if (applying_ || !enabled_ || !pri)
    {
        return;
    }
    if (!ValidateLayout(pri))
    {
        return;
    }

    const auto id = PriUid(pri);
    const auto current_title = ReadTitleId(pri);
    Remember(id, current_title);

    const auto p = title_cache_.find(id);
    if (p == title_cache_.end() || p->second.new_title_id == current_title)
    {
        return;
    }
    if (!CanChangeTitle())
    {
        return;
    }

    DEBUGLOG("Re-applying title '{}' for {}", p->second.new_title_id, id.pri_id_string);
    DoSetTitle(pri, p->second.new_title_id);
}


bool PlayerTitleChanger::IsInTitleCache(const PriUid& pri_id) const
{
    StateLock const lock;
    return title_cache_.contains(pri_id);
}


bool PlayerTitleChanger::IsUsable() const
{
    StateLock const lock;
    return enabled_;
}


bool PlayerTitleChanger::CanChangeTitle() const
{
    return enabled_ && gw_->IsInReplay();
}


std::string PlayerTitleChanger::GetDisplayedTitleId(const PriUid& pri_id) const
{
    StateLock const lock;
    if (const auto p = title_cache_.find(pri_id); p != title_cache_.end())
    {
        return p->second.new_title_id;
    }
    if (const auto p = observed_titles_.find(pri_id); p != observed_titles_.end())
    {
        return p->second;
    }
    return {};
}


std::set<std::string> PlayerTitleChanger::GetKnownTitleIds() const
{
    StateLock const lock;
    return known_title_ids_;
}


bool PlayerTitleChanger::DoSetTitle(PriWrapper pri, const std::string& title_id)
{
    if (!ValidateLayout(pri))
    {
        return false;
    }

    // FName index 0 is None, which the game renders as no title.
    std::int32_t index = 0;
    if (!title_id.empty())
    {
        index = gw_->GetFNameIndexByString(title_id);
        if (index <= 0)
        {
            LOG("Rocket League has no name entry for the title id '{}'", title_id);
            return false;
        }
    }

    const auto expected = index == 0 ? std::string{} : gw_->GetFNameByIndex(index);
    if (index != 0 && expected.empty())
    {
        LOG("The title id '{}' resolved to an empty name, refusing to write it", title_id);
        return false;
    }

    {
        FlagGuard const guard{applying_};
        *TitleField(pri) = {index, 0};
        pri.OnTitleChanged();
        pri.SyncPlayerTitle();
    }

    const auto written = ReadTitleId(pri);
    if (written != expected)
    {
        LOG("Failed to write the title '{}': the field holds '{}' afterwards", title_id, written);
        return false;
    }

    Remember(PriUid(pri), written);
    return true;
}


std::string PlayerTitleChanger::ReadTitleId(const PriWrapper& pri) const
{
    if (!pri || !enabled_)
    {
        return {};
    }
    const auto current = *TitleField(pri);
    if (current.index <= 0)
    {
        return {};
    }
    return gw_->GetFNameByIndex(current.index);
}


bool PlayerTitleChanger::ValidateLayout(PriWrapper pri)
{
    if (layout_checked_)
    {
        return enabled_;
    }
    if (!pri)
    {
        // Can't tell anything from a null PRI, so don't record a verdict yet.
        return false;
    }

    // Note we deliberately do NOT check the title field itself: a player with no title carries
    // FName None (index 0), which is perfectly valid and indistinguishable from a bad read.
    // Instead, cross-check the neighbouring fields the SDK does expose. If our raw reads agree
    // with the wrapper, this build lays PRI_TA out the way the offsets say and the title offset
    // is trustworthy too.
    const auto dodge = ReadFloat(pri, kPriDodgeInputThresholdOffset);
    const auto steering = ReadFloat(pri, kPriSteeringSensitivityOffset);
    const auto air_control = ReadFloat(pri, kPriAirControlSensitivityOffset);

    const auto expected_dodge = pri.GetDodgeInputThreshold();
    const auto expected_steering = pri.GetSteeringSensitivity();
    const auto expected_air_control = pri.GetAirControlSensitivity();

    if (dodge != expected_dodge || steering != expected_steering || air_control != expected_air_control)
    {
        layout_checked_ = true;
        enabled_ = false;
        LOG("Title editing disabled: this Rocket League build does not lay PRI_TA out the way "
            "the plugin expects. Read {} / {} / {} at the sensitivity offsets, but the game "
            "reports {} / {} / {}. The offsets in PlayerTitleChanger.cpp need updating.",
            dodge, steering, air_control, expected_dodge, expected_steering, expected_air_control);
        return false;
    }

    if (dodge == 0.0f && steering == 0.0f && air_control == 0.0f)
    {
        // All zeroes match trivially and prove nothing. Stay usable, keep looking for a real
        // confirmation on a later PRI. A bad layout would already have failed the check above.
        return true;
    }

    layout_checked_ = true;
    const auto current_title = ReadTitleId(pri);
    LOG("Title editing enabled: PRI_TA layout confirmed. Title of {} is '{}'",
        pri.GetPlayerName().ToString(), current_title.empty() ? "(none)" : current_title);
    return true;
}


void PlayerTitleChanger::Remember(const PriUid& pri_id, const std::string& title_id)
{
    observed_titles_[pri_id] = title_id;
    if (!title_id.empty())
    {
        known_title_ids_.insert(title_id);
    }
}
