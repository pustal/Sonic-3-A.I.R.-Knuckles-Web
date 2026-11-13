/**
 * @file main.cpp
 * @brief Entry point of Sonic 3 A.I.R. for webOS.
 *
 * Before the game starts:
 *  1. the data folder is chosen and the game is pointed at it (XDG_DATA_HOME),
 *  2. a save backup received during the last session is restored,
 *  3. root access is probed and the USB drives' game files are linked in,
 *  4. the transfer page starts (it keeps running while the game plays),
 *  5. without a ROM, the setup screen waits for one,
 *  6. the game's working directory is prepared,
 * then A.I.R.'s own main() runs (renamed s3air_main by CMakeLists.txt). When
 * the game quits after a restart request (the menu or the transfer page), the
 * app starts itself again.
 */
#include "port/GameFolder.h"
#include "port/MenuOverlay.h"
#include "port/NetInfo.h"
#include "port/RemoteInput.h"
#include "port/SetupScreen.h"
#include "port/Storage.h"
#include "port/TransferServer.h"
#include "port/UsbAccess.h"

#include <SDL.h>

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

/** Sonic 3 A.I.R.'s main() (Oxygen/sonic3air/source/sonic3air/main.cpp). */
int s3air_main(int argc, char **argv);

namespace
{

using namespace s3w;

/** "on the TV", "on USB drive sda1", ... for a file the game reads. */
std::string DescribeSource(Source source, const std::string &path)
{
    switch (source) {
        case Source::Internal: return "on the TV";
        case Source::UsbLink:
        case Source::UsbMount: {
            // <mount>/Sonic 3 AIR/<file>: the drive is the mount point's last part.
            const std::filesystem::path mount = std::filesystem::path(path).parent_path().parent_path();
            return "on USB drive " + mount.filename().string();
        }
        default: return std::string();
    }
}

/** Gathers what the menu shows (called on the game's thread). */
OverlayInfo GatherOverlayInfo(const Storage &storage, UsbAccess &usb, const TransferServer &server)
{
    OverlayInfo info;
    const std::string address = FindLanAddress();
    if (server.Port() != 0 && !address.empty())
        info.address = "http://" + address + ":" + std::to_string(server.Port());
    info.pin = server.Pin();

    info.romSource = DescribeSource(usb.ActiveSource(GameFile::Rom), usb.ActivePath(GameFile::Rom));
    if (info.romSource.empty())
        info.romSource = "missing";
    info.soundtrackSource = DescribeSource(usb.ActiveSource(GameFile::Soundtrack), usb.ActivePath(GameFile::Soundtrack));
    if (info.soundtrackSource.empty())
        info.soundtrackSource = "not installed (add it on the transfer page)";

    const std::size_t tvMods  = Storage::ListMods(storage.ModsDir()).size();
    const std::size_t drives  = usb.LinkedModDrives().size();
    const std::size_t active  = storage.ActiveMods().size();
    info.modsSource = std::to_string(tvMods) + " on the TV";
    if (drives > 0)
        info.modsSource += ", " + std::to_string(drives) + (drives == 1 ? " USB drive" : " USB drives") + " linked";
    info.modsSource += ", " + std::to_string(active) + " active";

    const RootStatus root = usb.Root();
    info.root = root.runningAsRoot ? "yes (the app runs as root)" : root.homebrewChannel ? "yes (Homebrew Channel)" : "no";
    info.restartPending = server.Snapshot().restartPending;
    return info;
}

} // namespace

int main(int argc, char **argv)
{
    (void)argc;
    RemoteInput::ApplyPlatformHints();

    // 1. The data folder; A.I.R. keeps its app data in $XDG_DATA_HOME/Sonic3AIR.
    Storage storage = Storage::Resolve();
    storage.CreateFolders();
    setenv("XDG_DATA_HOME", storage.Root().c_str(), 1);
    std::printf("[S3AIR/webOS] data folder: %s%s\n", storage.Root().c_str(), storage.Persistent() ? "" : " (temporary)");

    // 2. Saves restored from the transfer page while the game was running.
    storage.ApplyPendingBackup();

    // 3. Root access and the USB drives.
    UsbAccess usb;
    usb.ProbeRoot();
    usb.PlaceContent(storage);

    // 4. The transfer page.
    TransferServer server(storage, usb, LoadOrCreatePin(storage));
    if (server.Start())
        std::printf("[S3AIR/webOS] transfer page on port %u, PIN %s\n", static_cast<unsigned>(server.Port()), server.Pin().c_str());
    else
        std::printf("[S3AIR/webOS] the transfer page could not start\n");
    MenuOverlay::SetInfoProvider([&storage, &usb, &server] { return GatherOverlayInfo(storage, usb, server); });

    // 5. No ROM yet: wait for one.
    if (Storage::FileSize(storage.RomPath()) != Storage::kRomBytes) {
        server.SetSetupMode(true);
        const SetupResult setup = RunSetupScreen(storage, usb, server.Port() != 0 ? &server : nullptr);
        if (setup != SetupResult::RomReady) {
            server.Stop();
            return setup == SetupResult::Exit ? 0 : 1;
        }
    }
    server.SetSetupMode(false);

    // 6. The game's working directory.
    const std::string folderError = GameFolder::Prepare(storage);
    if (!folderError.empty()) {
        std::printf("[S3AIR/webOS] %s\n", folderError.c_str());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Sonic 3 A.I.R.", folderError.c_str(), nullptr);
        server.Stop();
        return 1;
    }

    // A.I.R. moves into the folder its executable path names: give it the game folder.
    // webOS passes the launch parameters as an argument; the game does not get them.
    std::string executable = storage.GameDir() + "/Sonic3AIR";
    std::vector<char *> gameArgs = { executable.data(), nullptr };
    if (chdir(storage.GameDir().c_str()) != 0)
        std::printf("[S3AIR/webOS] could not enter %s\n", storage.GameDir().c_str());
    const int result = s3air_main(1, gameArgs.data());

    server.Stop();
    if (MenuOverlay::RestartRequested()) {
        std::printf("[S3AIR/webOS] restarting\n");
        std::fflush(stdout);
        execv("/proc/self/exe", argv);
        std::printf("[S3AIR/webOS] could not restart\n");
    }
    return result;
}
