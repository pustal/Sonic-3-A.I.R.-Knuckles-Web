/**
 * @file Storage.h
 * @brief The port's data folder: where the game finds the ROM, the remastered
 *        soundtrack, mods and saves, and where the transfer page puts them.
 *
 * Layout of the data folder (on the TV: $HOME/data, the app's private,
 * persistent storage):
 *   Sonic3AIR/                          the game's app data folder (A.I.R. puts it
 *                                       in $XDG_DATA_HOME/Sonic3AIR; the port sets
 *                                       XDG_DATA_HOME to the data folder)
 *     Sonic_Knuckles_wSonic3.bin        the ROM (a file, or a link to a USB drive's)
 *     data/audioremaster.bin            the remastered soundtrack (optional package)
 *     mods/                             mods (zip files or folders); mods/usb-<drive>
 *                                       are links to the USB drives' mod folders
 *     settings.json, settings_input.json, storage/, savestates/   the saves
 *   game/                               the game's working directory: links to the
 *                                       game data in the app, plus saves/ (compiled
 *                                       scripts cache)
 *   incoming/                           a save backup received while the game ran,
 *                                       applied at the next launch
 *   transfer_pin.txt                    the transfer page's PIN
 */
#ifndef S3W_STORAGE_H
#define S3W_STORAGE_H

#include <cstdint>
#include <string>
#include <vector>

namespace s3w
{

/** A mod file or folder in a mods folder. */
struct ModEntry {
    std::string name;   ///< File or folder name in the mods folder.
    std::uint64_t size; ///< Bytes (0 for a folder).
    bool folder;        ///< A folder rather than a zip file.
};

/** A file in the saves, relative to the app data folder. */
struct SaveFile {
    std::string path;   ///< e.g. "storage/persistentdata.bin"
    std::uint64_t size; ///< Bytes.
    std::int64_t mtime; ///< Last change, seconds since the Unix epoch.
};

class Storage
{
public:
    /** Space kept free on top of any upload, so the TV is never filled to the brim. */
    static constexpr std::uint64_t kSafetyMarginBytes = 32ull * 1024ull * 1024ull;
    /** The ROM's file name, as A.I.R. stores its copy (and as the Steam release names it). */
    static constexpr const char *kRomName = "Sonic_Knuckles_wSonic3.bin";
    /** The remastered soundtrack package, as the official A.I.R. downloads ship it. */
    static constexpr const char *kSoundtrackName = "audioremaster.bin";
    /** The ROM A.I.R. accepts: Sonic 3 & Knuckles, 4 MB. */
    static constexpr std::uint64_t kRomBytes = 0x400000;
    /** The remastered soundtrack is about 126 MB; anything far larger is not it. */
    static constexpr std::uint64_t kMaxSoundtrackBytes = 512ull * 1024ull * 1024ull;
    /** Largest mod zip accepted (the largest published mods are a few hundred MB). */
    static constexpr std::uint64_t kMaxModBytes = 1024ull * 1024ull * 1024ull;
    /** Largest save backup accepted (saves are a few MB, savestates a few hundred KB each). */
    static constexpr std::uint64_t kMaxBackupBytes = 256ull * 1024ull * 1024ull;

    /**
     * Picks the data folder: $S3AIR_DATA_DIR if set, else $HOME/data on the TV
     * ($HOME/.local/share/Sonic3AIR-webOS on a desktop), else a temporary
     * folder (then nothing survives a restart). Creates it.
     */
    static Storage Resolve();

    /** Absolute path of the data folder, without a trailing slash. */
    const std::string &Root() const { return root_; }

    /** Whether the data folder survives restarts. */
    bool Persistent() const { return persistent_; }

    /** Absolute path of @p relative inside the data folder. */
    std::string PathFor(const std::string &relative) const;

    /** The game's app data folder (Sonic3AIR/). */
    std::string AppDataDir() const { return PathFor("Sonic3AIR"); }

    /** Where the game reads the ROM. */
    std::string RomPath() const { return AppDataDir() + "/" + kRomName; }

    /** Where the game reads the remastered soundtrack package. */
    std::string SoundtrackPath() const { return AppDataDir() + "/data/" + kSoundtrackName; }

    /** The mods folder. */
    std::string ModsDir() const { return AppDataDir() + "/mods"; }

    /** The game's working directory. */
    std::string GameDir() const { return PathFor("game"); }

    /** Where a save backup received while the game runs waits for the next launch. */
    std::string IncomingDir() const { return PathFor("incoming"); }

    /** Where a save backup received while the game runs is kept until the next launch. */
    std::string PendingBackupPath() const;

    /** Creates the folders above. */
    void CreateFolders() const;

    /** Bytes available to the app on the filesystem holding @p path (default: the data folder). */
    std::uint64_t FreeBytes(const std::string &path = std::string()) const;

    /** Size of the file at @p path in bytes (following links), or 0 if it does not exist. */
    static std::uint64_t FileSize(const std::string &path);

    /** Whether @p path is a regular file itself (not a link, not missing). */
    static bool IsPlainFile(const std::string &path);

    /**
     * Checks a ROM the way A.I.R. does (ResourcesCache::loadRomFile): 4 MB,
     * and, with the byte A.I.R. patches set, the checksum of Sonic 3 &
     * Knuckles. Also accepts the ROM with that byte already patched (A.I.R.'s
     * own copy).
     */
    static bool IsValidRom(const std::string &path);

    /** Whether the file at @p path is an Oxygen engine package of the version this game reads. */
    static bool IsSoundtrackPackage(const std::string &path);

    /**
     * Whether the zip file at @p path holds a mod: a "mod.json" in it, at any
     * depth (A.I.R. looks for the mod's folder inside the zip).
     */
    static bool ZipHoldsMod(const std::string &path);

    /** Whether @p name may be used for a mod uploaded through the page: a plain file name ending in .zip. */
    static bool IsAllowedModName(const std::string &name);

    /** The mods in the mods folder at @p dir (not the usb-* links), sorted by name. */
    static std::vector<ModEntry> ListMods(const std::string &dir);

    /** The mod folders A.I.R. has active (mods/active-mods.json), as it names them ("name.zip/folder"). */
    std::vector<std::string> ActiveMods() const;

    /** The files the save backup holds (see SaveBackup.h). */
    std::vector<SaveFile> ListSaves() const;

    /**
     * Applies a save backup received while the game ran (incoming/), before
     * the game starts. @return True if one was applied.
     */
    bool ApplyPendingBackup() const;

private:
    Storage(std::string root, bool persistent);

    std::string root_;
    bool persistent_ = true;
};

/** Formats a byte count for people ("1.5 GB"). */
std::string HumanBytes(std::uint64_t bytes);

} // namespace s3w

#endif // S3W_STORAGE_H
