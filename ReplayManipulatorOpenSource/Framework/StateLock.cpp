#include "pch.h"
#include "StateLock.h"

#include <mutex>

namespace
{
// One process-wide lock. The plugin is a singleton as far as BakkesMod is concerned, so
// threading a shared_ptr<mutex> through every feature constructor would buy nothing.
std::recursive_mutex g_state_mutex;
}

StateLock::StateLock()
{
    g_state_mutex.lock();
}

StateLock::~StateLock()
{
    g_state_mutex.unlock();
}
