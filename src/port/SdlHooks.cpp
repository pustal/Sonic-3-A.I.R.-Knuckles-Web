/**
 * @file SdlHooks.cpp
 * @brief Implementation of the SDL call hooks (see SdlHooks.h).
 *
 * The linker's --wrap=<symbol> sends the game's calls to __wrap_<symbol>;
 * __real_<symbol> is SDL's own function.
 */
#include "SdlHooks.h"

#include "Log.h"
#include "MenuOverlay.h"
#include "RemoteInput.h"

#include <map>

extern "C" {
int __real_SDL_PollEvent(SDL_Event *event);
void __real_SDL_GL_SwapWindow(SDL_Window *window);
SDL_AudioDeviceID __real_SDL_OpenAudioDevice(const char *device, int iscapture, const SDL_AudioSpec *desired,
                                             SDL_AudioSpec *obtained, int allowed_changes);
SDL_Window *__real_SDL_CreateWindow(const char *title, int x, int y, int w, int h, Uint32 flags);
int __real_SDL_ShowMessageBox(const SDL_MessageBoxData *data, int *buttonid);
int __real_SDL_ShowSimpleMessageBox(Uint32 flags, const char *title, const char *message, SDL_Window *window);
}

namespace s3w
{
namespace SdlHooks
{
namespace
{

/** Game controllers we opened, by joystick instance id. */
std::map<SDL_JoystickID, SDL_GameController *> &Controllers()
{
    static std::map<SDL_JoystickID, SDL_GameController *> controllers;
    return controllers;
}

/** Whether a controller's Back or Start is held, for the Back + Start menu shortcut. */
struct ComboState {
    bool back  = false;
    bool start = false;
};

std::map<SDL_JoystickID, ComboState> &Combos()
{
    static std::map<SDL_JoystickID, ComboState> combos;
    return combos;
}

/**
 * Tracks Back and Start on each controller.
 * @return True when this press completes Back + Start (the menu shortcut).
 */
bool CompletesMenuCombo(const SDL_Event &event)
{
    if (event.type != SDL_CONTROLLERBUTTONDOWN && event.type != SDL_CONTROLLERBUTTONUP)
        return false;
    const bool down    = event.type == SDL_CONTROLLERBUTTONDOWN;
    ComboState &state  = Combos()[event.cbutton.which];
    const bool wasBoth = state.back && state.start;
    if (event.cbutton.button == SDL_CONTROLLER_BUTTON_BACK)
        state.back = down;
    else if (event.cbutton.button == SDL_CONTROLLER_BUTTON_START)
        state.start = down;
    return down && !wasBoth && state.back && state.start;
}

} // namespace

int PollRealEvent(SDL_Event *event) { return __real_SDL_PollEvent(event); }

bool HandleDeviceEvent(const SDL_Event &event)
{
    if (event.type == SDL_JOYDEVICEADDED) {
        const int index = event.jdevice.which;
        if (SDL_IsGameController(index)) {
            if (SDL_GameController *controller = SDL_GameControllerOpen(index)) {
                const SDL_JoystickID id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller));
                if (Controllers().count(id) == 0)
                    Controllers()[id] = controller;
                else
                    SDL_GameControllerClose(controller); // already ours: drop the extra reference
            }
        }
        return true;
    }
    if (event.type == SDL_JOYDEVICEREMOVED) {
        const auto found = Controllers().find(event.jdevice.which);
        if (found != Controllers().end()) {
            SDL_GameControllerClose(found->second);
            Controllers().erase(found);
        }
        Combos().erase(event.jdevice.which);
        return true;
    }
    return false;
}

} // namespace SdlHooks
} // namespace s3w

extern "C" {

int __wrap_SDL_PollEvent(SDL_Event *event)
{
    using namespace s3w;
    if (!event)
        return __real_SDL_PollEvent(nullptr); // only asks whether an event is waiting
    SDL_Event *target = event;
    while (__real_SDL_PollEvent(target)) {
        // The game still sees device events: it opens the joysticks itself.
        SdlHooks::HandleDeviceEvent(*target);

        if (RemoteInput::IsMenuOpenRequest(*target) || SdlHooks::CompletesMenuCombo(*target)) {
            MenuOverlay::RequestOpen();
            continue;
        }
        // The rest of a menu key's presses (repeats, the release) never reach the game either.
        if (RemoteInput::IsMenuKey(*target))
            continue;

        RemoteInput::RewriteForGame(target);
        return 1;
    }
    return 0;
}

void __wrap_SDL_GL_SwapWindow(SDL_Window *window)
{
    // Start-up progress for the log: the game has drawn its first frame.
    static bool firstFrame = true;
    if (firstFrame) {
        firstFrame = false;
        s3w::Log::Printf("the game drew its first frame");
    }
    s3w::MenuOverlay::OnSwap(window, &__real_SDL_GL_SwapWindow);
    __real_SDL_GL_SwapWindow(window);
}

SDL_AudioDeviceID __wrap_SDL_OpenAudioDevice(const char *device, int iscapture, const SDL_AudioSpec *desired,
                                             SDL_AudioSpec *obtained, int allowed_changes)
{
    const SDL_AudioDeviceID id = __real_SDL_OpenAudioDevice(device, iscapture, desired, obtained, allowed_changes);
    s3w::Log::Printf("audio device: %s", id != 0 ? "open" : SDL_GetError());
    if (id != 0 && !iscapture)
        s3w::MenuOverlay::SetAudioDevice(id);
    return id;
}

SDL_Window *__wrap_SDL_CreateWindow(const char *title, int x, int y, int w, int h, Uint32 flags)
{
#if defined(S3W_TARGET_WEBOS)
    // A TV has one screen and no window frame: always the whole panel.
    flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
#endif
    SDL_Window *window = __real_SDL_CreateWindow(title, x, y, w, h, flags);
    s3w::Log::Printf("window \"%s\" %dx%d (flags 0x%x): %s", title ? title : "", w, h, static_cast<unsigned>(flags),
                     window ? "created" : SDL_GetError());
    return window;
}

int __wrap_SDL_ShowMessageBox(const SDL_MessageBoxData *data, int *buttonid)
{
    // The game reports errors in message boxes: keep them in the port's log too.
    s3w::Log::Printf("message box \"%s\": %s", data && data->title ? data->title : "", data && data->message ? data->message : "");
#if defined(S3W_TARGET_WEBOS)
    // No dialogs on the TV: the game takes the failure as "Cancel", as on any platform without message boxes.
    (void)buttonid;
    return SDL_SetError("No message boxes on webOS");
#else
    return __real_SDL_ShowMessageBox(data, buttonid);
#endif
}

int __wrap_SDL_ShowSimpleMessageBox(Uint32 flags, const char *title, const char *message, SDL_Window *window)
{
    s3w::Log::Printf("message box \"%s\": %s", title ? title : "", message ? message : "");
#if defined(S3W_TARGET_WEBOS)
    (void)flags;
    (void)window;
    return SDL_SetError("No message boxes on webOS");
#else
    return __real_SDL_ShowSimpleMessageBox(flags, title, message, window);
#endif
}

} // extern "C"
