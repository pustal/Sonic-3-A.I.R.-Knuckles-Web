/**
 * @file RemoteInput.h
 * @brief Maps LG remote control buttons onto the keyboard keys Sonic 3 A.I.R.
 *        already understands, and recognises the keys that open the menu.
 *
 * A.I.R.'s default keyboard layout for player 1 is:
 *   arrows = D-pad, A = A, S = B, D/Q = X, W = Y, E = L, R = R,
 *   Return = Start, Escape/Backspace = Back.
 * Rather than teaching the game about the remote, each remote button is
 * rewritten into one of those keys before the game sees the event. Game
 * controllers are untouched: the game reads them itself.
 *
 * Layout (the remote held sideways, like a classic pad, as in SonicWebMania).
 * The colour buttons follow the Xbox controller's colours:
 *   2 = Left, 8 = Right, 6 = Up, 4 = Down,
 *   Green = A, Red = B, Blue = X, Yellow = Y, 0 = Start.
 * The remote's own arrow keys and OK button keep working as arrows and Start.
 * Back (and Exit) opens the port's menu (MenuOverlay.h); on a controller, the
 * Guide button or Back + Start together do.
 */
#ifndef S3W_REMOTE_INPUT_H
#define S3W_REMOTE_INPUT_H

#include <SDL.h>

namespace s3w
{

/** What a menu should do in response to an input event. */
enum class MenuAction {
    None,
    Up,
    Down,
    Confirm,
    Cancel,
};

namespace RemoteInput
{

/**
 * If @p event is a key event from a mapped remote button, rewrites its
 * scancode and keycode to the keyboard key the game expects.
 *
 * @return True if the event was rewritten.
 */
bool RewriteForGame(SDL_Event *event);

/** True for a press or release of a key that opens the menu (Back, Exit, or Escape on a keyboard). */
bool IsMenuKey(const SDL_Event &event);

/** True when @p event is a press that should open the menu: a menu key, or the controller's Guide button. */
bool IsMenuOpenRequest(const SDL_Event &event);

/** Translates a key or controller press into a menu action (remote, keyboard or pad). */
MenuAction ToMenuAction(const SDL_Event &event);

/** Applies the SDL hints that let a native webOS app receive Back/Exit and hide the pointer. */
void ApplyPlatformHints();

} // namespace RemoteInput
} // namespace s3w

#endif // S3W_REMOTE_INPUT_H
