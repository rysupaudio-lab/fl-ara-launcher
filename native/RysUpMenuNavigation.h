#ifndef RYSUP_MENU_NAVIGATION_H
#define RYSUP_MENU_NAVIGATION_H

#include <stdbool.h>

enum {
  RysMenuEnabled = 1 << 0,
  RysMenuSeparator = 1 << 1,
  RysMenuHidden = 1 << 2
};

static inline bool RysMenuSelectable(unsigned char flags) {
  return (flags & RysMenuEnabled) && !(flags & (RysMenuSeparator | RysMenuHidden));
}

// Signed arrow count: positive is Down, negative is Up. NSMenu skips entries
// that cannot be selected. A missing highlight (-1) starts before the first
// entry; an existing, now-unselectable highlight still supplies its position.
// Reject invalid/unselectable destinations without producing a key sequence.
static inline bool RysMenuNavigationSteps(const unsigned char *items, long count,
                                         long current, long target, long *steps) {
  if (!items || !steps || count <= 0 || current < -1 || current >= count ||
      target < 0 || target >= count || !RysMenuSelectable(items[target]))
    return false;
  *steps = 0;
  if (current == target) return true;
  const long direction = target > current ? 1 : -1;
  for (long i = current + direction;; i += direction) {
    if (RysMenuSelectable(items[i])) *steps += direction;
    if (i == target) return true;
  }
}

typedef enum {
  RysMenuInvalid, RysMenuWait, RysMenuDown, RysMenuUp, RysMenuReturn, RysMenuDone
} RysMenuAction;

typedef struct {
  bool arrow_pending;
  long arrow_from;
  bool return_sent;
} RysMenuNavigationState;

// Each call consumes a fresh observation, not the position a posted key was
// expected to reach. Never queue another arrow until the highlight changes.
// If an event is lost, the recipe times out instead of guessing and committing.
static inline RysMenuAction RysMenuNavigationNext(RysMenuNavigationState *state,
                                                bool tracking, const unsigned char *items,
                                                long count, long current, long target) {
  if (!state) return RysMenuInvalid;
  if (!tracking) return state->return_sent ? RysMenuDone : RysMenuInvalid;
  long steps = 0;
  if (!RysMenuNavigationSteps(items, count, current, target, &steps)) return RysMenuInvalid;
  if (state->return_sent) return RysMenuWait;
  if (state->arrow_pending && current == state->arrow_from) return RysMenuWait;
  state->arrow_pending = false;
  if (steps == 0) {
    state->return_sent = true;
    return RysMenuReturn;
  }
  state->arrow_pending = true;
  state->arrow_from = current;
  return steps > 0 ? RysMenuDown : RysMenuUp;
}

#endif
