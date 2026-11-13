/**
 * @file SdlHooks.h
 * @brief The game's SDL calls the port sees first (linked with --wrap, see
 *        CMakeLists.txt): SDL_PollEvent, SDL_GL_SwapWindow,
 *        SDL_OpenAudioDevice and SDL_CreateWindow.
 *
 *  - Events: remote buttons become the keyboard keys the game knows
 *    (RemoteInput.h); the menu keys open the port's menu instead of reaching
 *    the game; game controllers are opened as SDL game controllers so their
 *    Guide, Back and Start buttons can be told apart.
 *  - Buffer swap: the menu and the start-up hint are drawn (MenuOverlay.h).
 *  - Audio device: remembered, so the menu can pause it.
 *  - Window: fullscreen on the TV, whatever the game's settings say.
 */
#ifndef S3W_SDL_HOOKS_H
#define S3W_SDL_HOOKS_H

#include <SDL.h>

namespace s3w
{
namespace SdlHooks
{

/** SDL's own SDL_PollEvent, past the hook (for the menu's own event loop). */
int PollRealEvent(SDL_Event *event);

/**
 * Opens or closes the game controller of a device added or removed event.
 * @return True if @p event was such an event.
 */
bool HandleDeviceEvent(const SDL_Event &event);

} // namespace SdlHooks
} // namespace s3w

#endif // S3W_SDL_HOOKS_H
