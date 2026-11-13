/**
 * @file SetupScreen.h
 * @brief First-run screen shown while the ROM is missing.
 *
 * It shows the address to open on a phone or computer, the PIN, the free
 * space, USB and root state and the upload progress, and returns as soon as a
 * ROM is in place (uploaded, or found on a USB drive plugged in meanwhile).
 */
#ifndef S3W_SETUP_SCREEN_H
#define S3W_SETUP_SCREEN_H

namespace s3w
{

class Storage;
class TransferServer;
class UsbAccess;

/** How the setup screen ended. */
enum class SetupResult {
    RomReady, ///< A ROM is in place; start the game.
    Exit,     ///< The player closed the app.
    Failed,   ///< The screen could not be created.
};

/**
 * Runs the setup screen until a ROM is in place or the player exits.
 * Creates and destroys its own window; SDL video is shut down on return so
 * the game can initialise it from scratch.
 *
 * @param server May be null if the transfer server failed to start.
 */
SetupResult RunSetupScreen(const Storage &storage, UsbAccess &usb, const TransferServer *server);

} // namespace s3w

#endif // S3W_SETUP_SCREEN_H
