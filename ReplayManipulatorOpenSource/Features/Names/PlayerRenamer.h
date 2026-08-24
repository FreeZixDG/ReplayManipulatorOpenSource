#pragma once
#include "bakkesmod/wrappers/gamewrapper.h"
#include "Data/PriUid.h"


struct PlayerNameOverride
{
    std::string original;
    std::string new_name;
};

class PlayerRenamer
{
public:
    explicit PlayerRenamer(std::shared_ptr<GameWrapper> gw);

    void Rename(PriWrapper pri, const std::string& new_name);
    void Restore(PriWrapper pri);
    void ResetNameOverrides();
    [[nodiscard]] bool IsInRenameCache(const PriUid& pri_id) const;
    /// The name we renamed this player to, or empty if we never renamed them. Only reads
    /// cached state, so it is safe to call from the render thread.
    [[nodiscard]] std::string GetOverriddenName(const PriUid& pri_id) const;
    [[nodiscard]] bool CanRename() const;

private:
    void OnNameChange(PriWrapper& pri);
    static void DoRename(PriWrapper pri, const std::string& new_name);
    /// True while the replay is jumping through the timeline, and for a short settling
    /// period afterwards. Renaming during that window crashes the game -- see the .cpp.
    [[nodiscard]] bool IsSeekingTimeline(ReplayServerWrapper& replay);

    std::map<PriUid, PlayerNameOverride> name_cache_;
    /// Replay frame seen on the previous nameplate update, -1 when we have no baseline yet.
    int last_replay_frame_ = -1;
    int seek_settle_frames_ = 0;
    bool enabled_ = true;
    std::shared_ptr<GameWrapper> gw_;
};
