import 'package:flutter_soloud/src/filters/custom_filter.dart';

/// The native filter chain slot a [CustomFilterSlot] addresses.
typedef ClapSlotKey = (int soundHash, int busId, int slotIndex);

/// The [ClapSlotKey] of [slot], resolved the way the native side routes it:
/// the sound and bus are unsigned 32-bit values (an absent one is 0), a
/// nonzero sound wins over the bus, and sound 0 with bus 0 is the global
/// chain. Slots that address the same native slot get equal keys.
ClapSlotKey clapSlotKey(CustomFilterSlot slot) {
  final soundHash = (slot.soundHash?.hash ?? 0) & 0xFFFFFFFF;
  final busId = soundHash != 0 ? 0 : (slot.busId ?? 0) & 0xFFFFFFFF;
  return (soundHash, busId, slot.slotIndex);
}

/// The owner last bound to each native slot, and the engine session it was
/// bound in. An owner only releases a slot while it is still the owner in
/// the current session, so a stale owner never releases a later one's slot.
///
/// Owners are held weakly, and entries of earlier sessions or of collected
/// owners are dropped at every [bind] and [release].
class ClapSlotOwners<T extends Object> {
  final Map<ClapSlotKey, ({WeakReference<T> owner, int session})> _owners =
      {};

  /// The number of entries, for tests.
  int get length => _owners.length;

  /// Makes [owner] the owner of [slot] in [session], replacing any earlier
  /// owner.
  void bind(CustomFilterSlot slot, T owner, int session) {
    _prune(session);
    _owners[clapSlotKey(slot)] = (
      owner: WeakReference<T>(owner),
      session: session,
    );
  }

  /// Removes the binding of [slot] by [owner] and returns whether [owner]
  /// owned it in [session]: only then may it unbind the native slot.
  bool release(CustomFilterSlot slot, T owner, int session) {
    _prune(session);
    final key = clapSlotKey(slot);
    final binding = _owners[key];
    if (binding == null || !identical(binding.owner.target, owner)) {
      return false;
    }
    _owners.remove(key);
    return true;
  }

  void _prune(int session) {
    _owners.removeWhere(
      (_, binding) =>
          binding.session != session || binding.owner.target == null,
    );
  }
}
