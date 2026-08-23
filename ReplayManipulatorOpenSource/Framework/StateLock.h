#pragma once

/// <summary>
/// RAII lock over every piece of plugin state that both of our threads touch.
///
/// BakkesMod calls RenderSettings/RenderWindow from its D3D present hook, which runs on
/// Rocket League's render thread, while HookEvent callbacks and gameWrapper->Execute
/// callbacks run on the game thread. Anything reachable from both -- replay_players_ and
/// the loadouts inside it, the camera overrides, the name and title caches -- needs
/// guarding, or a container gets rehashed or reallocated under an iterator on the other
/// thread. A timeline seek is the worst case: it fires the loadout and camera hooks in
/// bursts while the UI keeps drawing.
///
/// One lock for all of it, deliberately, rather than one per container:
///  - A single UI frame walks the players, the camera overrides and the title caches
///    together. Per-container locks would mean several acquisitions per frame, in an order
///    the game thread has no reason to follow, which is how deadlocks get built.
///  - Nothing here ever blocks waiting on the other thread (Execute and SetTimeout only
///    enqueue), so with exactly one lock a deadlock is not expressible.
///
/// It is recursive because our own writes re-enter the game -- SetLoadoutItems inside the
/// UpdateFromLoadout hook re-fires that same hook -- and because nested helpers should not
/// have to know whether a caller already locked.
///
/// Held only for short stretches: building a UI frame, or one hook callback.
/// </summary>
class StateLock
{
public:
    StateLock();
    ~StateLock();

    StateLock(const StateLock& other) = delete;
    StateLock(StateLock&& other) noexcept = delete;
    StateLock& operator=(const StateLock& other) = delete;
    StateLock& operator=(StateLock&& other) noexcept = delete;
};
