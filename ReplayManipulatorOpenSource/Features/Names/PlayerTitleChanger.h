#pragma once
#include <set>
#include <string>

#include "bakkesmod/wrappers/gamewrapper.h"
#include "Data/PriUid.h"


struct PlayerTitleOverride
{
    std::string original_title_id;
    std::string new_title_id;
};

/// <summary>
/// Overrides the player title the game shows for a player in a replay.
///
/// Rocket League stores the title as an FName in PRI_TA.Title and resolves it against
/// TitleConfig_X to get the text and colours the scoreboard displays. The BakkesMod SDK
/// wraps none of that, so this writes the field through a raw offset. Every write is
/// validated, and the whole feature disables itself if the offset stops looking sane.
/// </summary>
class PlayerTitleChanger
{
public:
    explicit PlayerTitleChanger(std::shared_ptr<GameWrapper> gw);

    /// An empty title_id writes FName None, which the game shows as no title at all.
    void SetTitle(PriWrapper pri, const std::string& title_id);
    void Restore(PriWrapper pri);
    void ResetTitleOverrides();

    /// Game thread only. Records the title the game currently has for this player and
    /// re-applies our override if the game replaced it.
    void ObserveAndReapply(PriWrapper& pri);

    [[nodiscard]] bool IsInTitleCache(const PriUid& pri_id) const;
    [[nodiscard]] bool CanChangeTitle() const;
    /// False once the raw offset has been found to no longer match this Rocket League build.
    [[nodiscard]] bool IsUsable() const;

    // The three below are the render thread's view of this feature. They read cached state
    // that ObserveAndReapply writes from the game thread, so they take a StateLock.
    [[nodiscard]] std::string GetDisplayedTitleId(const PriUid& pri_id) const;
    /// Every distinct title id seen on a player so far. There is no way to enumerate the
    /// game's full title list, so this is how the user discovers ids that actually exist.
    /// Returned by value: a reference would let the caller walk the set after dropping the
    /// lock, which is exactly the crash this is meant to prevent.
    [[nodiscard]] std::set<std::string> GetKnownTitleIds() const;

private:
    [[nodiscard]] bool DoSetTitle(PriWrapper pri, const std::string& title_id);
    [[nodiscard]] std::string ReadTitleId(const PriWrapper& pri) const;
    /// Checks once per session that the raw offset still points at a plausible FName.
    [[nodiscard]] bool ValidateLayout(PriWrapper pri);
    void Remember(const PriUid& pri_id, const std::string& title_id);

    std::map<PriUid, PlayerTitleOverride> title_cache_;
    std::map<PriUid, std::string> observed_titles_;
    std::set<std::string> known_title_ids_;
    // Writing the title fires the events we react to. Don't recurse.
    bool applying_ = false;
    bool enabled_ = true;
    bool layout_checked_ = false;
    std::shared_ptr<GameWrapper> gw_;
};
