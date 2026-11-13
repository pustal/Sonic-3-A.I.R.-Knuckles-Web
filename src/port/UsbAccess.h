/**
 * @file UsbAccess.h
 * @brief Root access, USB drives, and the game files kept on them.
 *
 * webOS mounts USB drives under /tmp/usb/<disk>/<partition>. An app normally
 * runs inside a jail as an unprivileged user, so whether it can see, read or
 * write a drive depends on the firmware. Everything here therefore tries the
 * direct route first and falls back to root commands, which are available
 * when the app itself runs as root or when the Homebrew Channel's
 * luna://org.webosbrew.hbchannel.service/exec accepts the app's calls (rooted
 * TVs; newer firmwares may refuse calls from apps).
 *
 * A drive keeps the game's files in its "Sonic 3 AIR" folder:
 *   Sonic 3 AIR/Sonic_Knuckles_wSonic3.bin   the ROM
 *   Sonic 3 AIR/audioremaster.bin            the remastered soundtrack
 *   Sonic 3 AIR/mods/                        mods (zip files or folders)
 * PlaceContent() makes the game see them where it looks in its app data
 * folder: a symlink, or a root bind mount when the app cannot read the drive
 * itself. A ROM or soundtrack stored on the TV always wins over a drive's;
 * a drive's mods appear next to the TV's, in mods/usb-<drive>.
 */
#ifndef S3W_USB_ACCESS_H
#define S3W_USB_ACCESS_H

#include "Storage.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace s3w
{

/** What kind of root access the app has. */
struct RootStatus {
    bool runningAsRoot = false;   ///< The app process itself is root.
    bool homebrewChannel = false; ///< Homebrew Channel exec works from this app.
    std::string detail;           ///< Short explanation for the page (e.g. why exec failed).

    bool Available() const { return runningAsRoot || homebrewChannel; }
};

/** A mounted USB drive, and the game files in its "Sonic 3 AIR" folder. */
struct UsbDrive {
    std::string mountPoint;           ///< e.g. /tmp/usb/sda/sda1
    std::string label;                ///< Short name for people, e.g. "sda1".
    std::uint64_t freeBytes = 0;
    std::uint64_t totalBytes = 0;
    bool spaceKnown = false;          ///< statvfs (or root df) succeeded.
    bool readable = false;            ///< The app can list it directly.
    bool writable = false;            ///< The app can create files on it directly.
    std::uint64_t romBytes = 0;       ///< Size of its ROM, 0 when it has none.
    std::uint64_t soundtrackBytes = 0;///< Size of its soundtrack package, 0 when it has none.
    bool modsFolder = false;          ///< "Sonic 3 AIR/mods" exists.
    std::vector<ModEntry> mods;       ///< The mods in it.
};

/** Where a file the game reads comes from in this session. */
enum class Source {
    None,     ///< Not there.
    Internal, ///< A file in the app data folder.
    UsbLink,  ///< A symlink to a file on a USB drive the app can read.
    UsbMount, ///< A root bind mount of a file the app cannot read on its own.
};

/** The two single files the game can take from a drive. */
enum class GameFile {
    Rom,
    Soundtrack,
};

class UsbAccess
{
public:
    /** The game's folder on a drive, relative to its mount point. */
    static constexpr const char *kDriveFolder = "Sonic 3 AIR";
    /** The mods folder on a drive, relative to its mount point. */
    static constexpr const char *kDriveModsFolder = "Sonic 3 AIR/mods";

    /** Probes root access (once; it takes up to a few seconds). */
    void ProbeRoot();

    /** Last probe result. */
    RootStatus Root() const;

    /** Lists mounted USB drives with their space, access and game files. */
    std::vector<UsbDrive> ListDrives();

    /** Fills @p drive for the drive mounted at @p mountPoint. @return False if it is not a mounted USB drive. */
    bool FindDrive(const std::string &mountPoint, UsbDrive *drive);

    /** Whether @p mountPoint is currently a mounted USB drive (guards upload targets). */
    bool IsUsbMount(const std::string &mountPoint);

    /** The file name @p file has, in the app data folder and on a drive. */
    static const char *FileName(GameFile file);

    /** Where the game reads @p file in the app data folder. */
    static std::string LocalPath(const Storage &storage, GameFile file);

    /** Where @p file lives on the drive mounted at @p mountPoint. */
    static std::string DrivePath(const std::string &mountPoint, GameFile file);

    /**
     * Makes the game see the drives' files (see the file comment): the ROM,
     * the soundtrack and the mod folders. Removes the links and bind mounts
     * of earlier sessions first. Called at launch, and again by the transfer
     * page after it changed what a drive holds.
     */
    void PlaceContent(const Storage &storage);

    /**
     * Removes the link or lazily unmounts the bind mount at @p file's local
     * path, so a file can replace it. A game that has it open keeps reading it.
     */
    void ReleaseLink(const Storage &storage, GameFile file);

    /** Where @p file came from at the last PlaceContent(), and the file it is. */
    Source ActiveSource(GameFile file) const;
    std::string ActivePath(GameFile file) const;

    /** The name a drive's mods folder has in the game's mods folder: "usb-<label>". */
    static std::string ModLinkName(const std::string &label);

    /** The drives whose mods are linked into the mods folder (mount points). */
    std::vector<std::string> LinkedModDrives() const;

    /**
     * Creates @p relative (a folder path) on the drive mounted at
     * @p mountPoint, directly or with root.
     * @return Empty string on success, otherwise the reason.
     */
    std::string CreateDriveFolder(const std::string &mountPoint, const std::string &relative);

    /**
     * Moves @p sourcePath (a finished upload in the data folder) to
     * @p destination on a USB drive, creating its folders, using root.
     * @return Empty string on success, otherwise the reason.
     */
    std::string MoveWithRoot(const std::string &sourcePath, const std::string &destination);

    /** Removes the file at @p path, directly or with root. @return True if it is gone. */
    bool RemoveFile(const std::string &path);

    /** Removes the folder at @p path and everything in it, directly or with root. @return True if it is gone. */
    bool RemoveFolder(const std::string &path);

    /** Renames @p from to @p to (same file system), directly or with root. @return True on success. */
    bool RenameFile(const std::string &from, const std::string &to);

    /**
     * Copies @p source to @p destination (whose folder exists), directly with
     * progress (@p onProgress receives the bytes copied), or with root when
     * the app cannot read the source or write the destination.
     * @return Empty string on success, otherwise the reason.
     */
    std::string CopyFile(const std::string &source, const std::string &destination,
                         const std::function<void(std::uint64_t)> &onProgress);

    /**
     * Runs @p command as root (sh -c). Requires Root().Available().
     * @return True if it exited successfully; @p output receives stdout.
     */
    bool RunAsRoot(const std::string &command, std::string *output = nullptr);

    /** Quotes @p text for a POSIX shell (single quotes). */
    static std::string ShellQuote(const std::string &text);

private:
    /** Mount points that look like USB drives, from /proc/mounts (or root's view). */
    std::vector<std::string> UsbMountPoints();

    /** Fills the game file fields of @p drive. */
    void InspectDrive(UsbDrive *drive);

    /** Size of the file at @p path, read directly or with root; 0 when it is missing. */
    std::uint64_t SizeOf(const std::string &path, bool readable);

    /** Links or bind-mounts @p source (a drive's file) at @p local. @return How, or Source::None. */
    Source LinkFile(const std::string &source, const std::string &local, bool readable);

    /** Removes the usb-* links and bind mounts in the mods folder. */
    void ReleaseModLinks(const Storage &storage);

    mutable std::mutex mutex_;
    RootStatus root_;
    Source sources_[2] = { Source::None, Source::None };
    std::string paths_[2];
    std::vector<std::string> modDrives_;
    /** Serialises PlaceContent() and ReleaseLink() (launch, and the page's threads). */
    std::recursive_mutex placeMutex_;
};

} // namespace s3w

#endif // S3W_USB_ACCESS_H
