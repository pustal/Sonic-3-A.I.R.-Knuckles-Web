/**
 * @file GameFolder.cpp
 * @brief Implementation of the game's working directory (see GameFolder.h).
 */
#include "GameFolder.h"

#include "Storage.h"

#include <unistd.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <system_error>

namespace s3w
{
namespace GameFolder
{
namespace fs = std::filesystem;

namespace
{

/** What the game reads from its working directory. */
constexpr std::array<const char *, 4> kGameDataEntries = { "data", "scripts", "config.json", "oxygenproject.json" };

} // namespace

std::string InstallDir()
{
    std::error_code ec;
    const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    if (ec)
        return ".";
    return exe.parent_path().string();
}

std::string GameDataDir()
{
    if (const char *overrideDir = std::getenv("S3AIR_GAME_DATA")) {
        if (*overrideDir)
            return overrideDir;
    }
    return InstallDir() + "/game";
}

std::string Prepare(const Storage &storage)
{
    const fs::path source = GameDataDir();
    std::error_code ec;
    if (!fs::exists(source / "data" / "metadata.json", ec))
        return "The game data is missing from the app (" + (source / "data").string() + "). Reinstall the app.";

    const fs::path game = storage.GameDir();
    fs::create_directories(game / "saves", ec);
    for (const char *entry : kGameDataEntries) {
        const fs::path link = game / entry;
        // Replace whatever is there: an older link, or a copy someone made by hand.
        if (fs::is_symlink(link, ec) || fs::is_regular_file(link, ec))
            fs::remove(link, ec);
        else if (fs::is_directory(link, ec))
            fs::remove_all(link, ec);
        if (!fs::exists(source / entry, ec))
            continue;
        fs::create_symlink(source / entry, link, ec);
        if (ec)
            return "Could not prepare the game folder: " + ec.message();
    }
    return {};
}

} // namespace GameFolder
} // namespace s3w
