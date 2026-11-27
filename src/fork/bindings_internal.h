// The narrow part of src/bindings.cpp's engine state that the fork's entry
// points (src/fork/fork_bindings.cpp) may use. Defined at the end of
// bindings.cpp, which owns `player`, `init_deinit_mutex` and
// `player_lifetime_mutex`; nothing here hands out the engine for keeps.
//
// Each helper reproduces exactly what the export used to do inline in
// bindings.cpp, so moving an export does not change its admission or
// lifetime behaviour. Lock ordering is bindings.cpp's:
// `init_deinit_mutex -> loadMutex -> player_lifetime_mutex -> sounds_mutex
// -> audio mutex`.
#pragma once

#ifndef FORK_BINDINGS_INTERNAL_H
#define FORK_BINDINGS_INTERNAL_H

#include <mutex>
#include <shared_mutex>

class Player;

namespace soloud_fork
{
  /// The engine's current Player, read afresh at every use, like the
  /// `player.get()` checks of the unguarded exports in bindings.cpp. It pins
  /// nothing: an export using it keeps the admission rules it had there.
  class EnginePlayer
  {
  public:
    /// True when a Player exists (initialized or not).
    explicit operator bool() const;
    Player *operator->() const;
  };

  /// A nonblocking try-lock on init_deinit_mutex, held for the scope. While it
  /// is held, no init or dispose can start.
  class InitDeinitTryLock
  {
  public:
    InitDeinitTryLock();
    bool ownsLock() const;

  private:
    std::unique_lock<std::mutex> mLock;
  };

  /// An unconditional shared hold on player_lifetime_mutex, for the plugin
  /// audio barrier. Unlike bindings.cpp's PlayerLease it never gives up while
  /// a teardown is pending: it either runs before the teardown or waits for it.
  class PlayerLifetimeSharedLock
  {
  public:
    PlayerLifetimeSharedLock();

  private:
    std::shared_lock<std::shared_mutex> mLock;
  };

  /// Called by dispose while it holds player_lifetime_mutex exclusively, after
  /// Player::dispose() and before the Player is destroyed.
  void onDisposeUnderLifetimeLock();

  /// Called by dispose after it releases player_lifetime_mutex and has shut the
  /// VST3 host down.
  void onDisposePluginShutdown();
} // namespace soloud_fork

#endif // FORK_BINDINGS_INTERNAL_H
