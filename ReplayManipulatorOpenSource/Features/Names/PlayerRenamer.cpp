#include "pch.h"
#include "PlayerRenamer.h"

#include "bakkesmod/wrappers/ReplayServerWrapper.h"

namespace
{
// How many nameplate updates to sit out after a timeline jump, waiting for the game to
// finish rebuilding its nameplate array. Roughly an eighth of a second at 120 fps, which
// is short enough that a name being briefly wrong right after a seek is not noticeable.
constexpr int kFramesToSettleAfterSeek = 15;
}

PlayerRenamer::PlayerRenamer(std::shared_ptr<GameWrapper> gw)
    : gw_(std::move(gw))
{
    gw_->HookEventPost("Function TAGame.GFxNameplatesManager_TA.Update", [this](...) {
        if (name_cache_.empty())
            return;

        auto replay = gw_->GetGameEventAsReplay();
        if (!replay)
            return;

        if (IsSeekingTimeline(replay))
            return;

        for (auto pri : replay.GetPRIs())
        {
            OnNameChange(pri);
        }
    });
}


void PlayerRenamer::DoRename(PriWrapper pri, const std::string& new_name)
{
    pri.ChangeNameForScoreboardAndNameplateInReplay(new_name);
}

void PlayerRenamer::Rename(PriWrapper pri, const std::string& new_name)
{
    if (!CanRename())
    {
        LOG("player renamer is disabled");
        return;
    }
    if (!pri)
    {
        LOG("Pri is null when changing name to {}", new_name);
        return;
    }

    if (const auto id = PriUid(pri); IsInRenameCache(id))
    {
        name_cache_[id].new_name = new_name;
    }
    else
    {
        const auto original = pri.GetPlayerName().ToString();
        name_cache_[id] = {original, new_name};
    }

    DoRename(pri, new_name);
}


void PlayerRenamer::Restore(PriWrapper pri)
{
    if (!CanRename())
    {
        LOG("player renamer is disabled");
        return;
    }
    const auto id = PriUid(pri);
    if (const auto p = name_cache_.find(id); p != name_cache_.end())
    {
        DEBUGLOG("Restoring the name for {}", p->second.original);
        const auto original_name = p->second.original;
        name_cache_.erase(p);
        DoRename(pri, original_name);
    }
}


bool PlayerRenamer::IsInRenameCache(const PriUid& pri_id) const
{
    return name_cache_.contains(pri_id);
}


void PlayerRenamer::ResetNameOverrides()
{
    name_cache_.clear();
    last_replay_frame_ = -1;
    seek_settle_frames_ = 0;
}


bool PlayerRenamer::CanRename() const
{
    return enabled_ && gw_->IsInReplay();
}


// Runs once per player on every single nameplate update, so it has to stay cheap and it must
// not write unless it has to. ChangeNameForScoreboardAndNameplateInReplay walks the game's
// nameplate array and dereferences every entry without null-checking it, so each redundant
// call is another roll of the dice against an entry the game has not filled in yet.
void PlayerRenamer::OnNameChange(PriWrapper& pri)
{
    if (!CanRename() || !pri)
    {
        return;
    }
    const auto p = name_cache_.find(PriUid(pri));
    if (p == name_cache_.end())
    {
        return;
    }
    // Already showing what we want: nothing to do. This is the common case by far, since the
    // game only resets the name when it rebuilds the player, not on every frame.
    if (pri.GetPlayerName().ToString() == p->second.new_name)
    {
        return;
    }

    DoRename(pri, p->second.new_name);
}


// Renaming while the replay is seeking crashes Rocket League: the game tears down and respawns
// its actors, and for a few frames the nameplate array the SDK scans holds a null entry, which
// it dereferences at +0xE8. The null check is missing inside pluginsdk.dll so we cannot fix it
// from here -- all we can do is not call into it while the array is inconsistent.
bool PlayerRenamer::IsSeekingTimeline(ReplayServerWrapper& replay)
{
    const auto frame = replay.GetCurrentReplayFrame();
    const auto previous = last_replay_frame_;
    last_replay_frame_ = frame;

    // Playback advances the replay frame by one or two per nameplate update and never runs it
    // backwards, so a bigger step forwards -- or any step back -- means the user jumped.
    const auto fps = replay.GetReplayFPS();
    const auto forward_limit = fps > 4 ? fps / 2 : 2;
    if (previous >= 0 && (frame - previous < -1 || frame - previous > forward_limit))
    {
        DEBUGLOG("Replay seeked from frame {} to {}, pausing renames", previous, frame);
        seek_settle_frames_ = kFramesToSettleAfterSeek;
    }

    if (seek_settle_frames_ > 0)
    {
        --seek_settle_frames_;
        return true;
    }
    return false;
}
