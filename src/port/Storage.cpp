/**
 * @file Storage.cpp
 * @brief Implementation of the data folder helpers (see Storage.h).
 */
#include "Storage.h"
#include "Log.h"

#include "SaveBackup.h"

#include <unzip.h>

#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace rmx
{
// librmx's MurmurHash64A (rmxbase/tools/Tools.h), the checksum A.I.R. checks the ROM with.
std::uint64_t getMurmur2_64(const unsigned char *data, std::size_t bytes);
} // namespace rmx

namespace s3w
{
namespace fs = std::filesystem;

namespace
{

/** Used when no persistent folder can be created (nothing then survives a restart). */
constexpr const char *kVolatileFallback = "/tmp/sonic3air-webos";
/** The checksum of Sonic 3 & Knuckles A.I.R. expects (ConfigurationImpl::fillDefaultGameProfile). */
constexpr std::uint64_t kRomChecksum = 0x344983ffcfeff8cbull;
/** The byte A.I.R. sets before checking (a header byte that differs between releases). */
constexpr std::size_t kRomPatchOffset = 0x2001f0;
constexpr unsigned char kRomPatchValue = 0x4a;
/** The pending save backup's name in incoming/. */
constexpr const char *kPendingBackup = "saves-backup.zip";

/** True if @p name ends with @p suffix, ignoring case. */
bool EndsWithNoCase(const std::string &name, const std::string &suffix)
{
    if (name.size() < suffix.size())
        return false;
    return std::equal(suffix.rbegin(), suffix.rend(), name.rbegin(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    });
}

/** Creates @p dir (and its parents) and checks the app can write there. */
bool EnsureWritableDirectory(const fs::path &dir)
{
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (!fs::is_directory(dir, ec))
        return false;
    return access(dir.c_str(), W_OK) == 0;
}

/** Modification time of @p path in seconds since the Unix epoch, or 0. */
std::int64_t ModificationTime(const fs::path &path)
{
    struct stat info {};
    if (stat(path.c_str(), &info) != 0)
        return 0;
    return static_cast<std::int64_t>(info.st_mtime);
}

} // namespace

std::string HumanBytes(std::uint64_t bytes)
{
    constexpr const char *kUnits[] = { "B", "KB", "MB", "GB", "TB" };
    double value     = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < sizeof(kUnits) / sizeof(kUnits[0])) {
        value /= 1024.0;
        ++unit;
    }
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), unit == 0 ? "%.0f %s" : "%.1f %s", value, kUnits[unit]);
    return buffer;
}

Storage Storage::Resolve()
{
    if (const char *overrideDir = std::getenv("S3AIR_DATA_DIR")) {
        if (*overrideDir && EnsureWritableDirectory(overrideDir))
            return Storage(overrideDir, true);
    }

    if (const char *home = std::getenv("HOME")) {
        // On webOS HOME is the app's private, persistent data folder. On a
        // desktop it is the user's home, so use a dedicated sub-folder.
#if defined(S3W_TARGET_WEBOS)
        const fs::path dir = fs::path(home) / "data";
#else
        const fs::path dir = fs::path(home) / ".local" / "share" / "Sonic3AIR-webOS";
#endif
        if (*home && EnsureWritableDirectory(dir))
            return Storage(dir.string(), true);
    }

    EnsureWritableDirectory(kVolatileFallback);
    return Storage(kVolatileFallback, false);
}

Storage::Storage(std::string root, bool persistent) : root_(std::move(root)), persistent_(persistent)
{
    while (root_.size() > 1 && root_.back() == '/')
        root_.pop_back();
}

std::string Storage::PathFor(const std::string &relative) const { return root_ + "/" + relative; }

void Storage::CreateFolders() const
{
    std::error_code ec;
    fs::create_directories(AppDataDir() + "/data", ec);
    fs::create_directories(ModsDir(), ec);
    fs::create_directories(GameDir() + "/saves", ec);
}

std::uint64_t Storage::FreeBytes(const std::string &path) const
{
    struct statvfs info {};
    if (statvfs((path.empty() ? root_ : path).c_str(), &info) != 0)
        return 0;
    return static_cast<std::uint64_t>(info.f_bavail) * info.f_frsize;
}

std::uint64_t Storage::FileSize(const std::string &path)
{
    struct stat info {};
    if (stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode))
        return 0;
    return static_cast<std::uint64_t>(info.st_size);
}

bool Storage::IsPlainFile(const std::string &path)
{
    struct stat info {};
    return lstat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

bool Storage::IsValidRom(const std::string &path)
{
    if (FileSize(path) != kRomBytes)
        return false;
    std::vector<unsigned char> rom(static_cast<std::size_t>(kRomBytes));
    std::ifstream in(path, std::ios::binary);
    if (!in.read(reinterpret_cast<char *>(rom.data()), static_cast<std::streamsize>(rom.size())))
        return false;
    return IsValidRomData(rom);
}

bool Storage::IsValidRomData(const std::vector<unsigned char> &rom)
{
    if (rom.size() != kRomBytes)
        return false;
    std::vector<unsigned char> patched = rom;
    patched[kRomPatchOffset]           = kRomPatchValue;
    return rmx::getMurmur2_64(patched.data(), patched.size()) == kRomChecksum;
}

bool Storage::IsRomFileName(const std::string &name)
{
    return EndsWithNoCase(name, ".bin") || EndsWithNoCase(name, ".md") || EndsWithNoCase(name, ".gen");
}

bool Storage::IsSoundtrackPackage(const std::string &path)
{
    // The header A.I.R. checks (oxygen/file/FilePackage.cpp): "OPCK", format
    // version, content version, size of the table of contents, entry count
    // (little-endian 32-bit values). A file that fails here would make the
    // game stop with an error at start-up.
    constexpr std::uint32_t kFormatVersion = 3;
    constexpr std::size_t kHeaderSize      = 20;
    std::ifstream in(path, std::ios::binary);
    std::array<unsigned char, kHeaderSize> header{};
    if (!in.read(reinterpret_cast<char *>(header.data()), static_cast<std::streamsize>(header.size())))
        return false;
    const auto readU32 = [&](std::size_t offset) {
        return static_cast<std::uint32_t>(header[offset]) | static_cast<std::uint32_t>(header[offset + 1]) << 8
               | static_cast<std::uint32_t>(header[offset + 2]) << 16 | static_cast<std::uint32_t>(header[offset + 3]) << 24;
    };
    return std::memcmp(header.data(), "OPCK", 4) == 0 && readU32(4) == kFormatVersion
           && kHeaderSize + readU32(12) <= FileSize(path) && readU32(16) > 0;
}

bool Storage::ZipHoldsMod(const std::string &path)
{
    unzFile zip = unzOpen64(path.c_str());
    if (!zip)
        return false;
    bool found = false;
    for (int status = unzGoToFirstFile(zip); status == UNZ_OK && !found; status = unzGoToNextFile(zip)) {
        std::array<char, 512> name{};
        unz_file_info64 info{};
        if (unzGetCurrentFileInfo64(zip, &info, name.data(), static_cast<uLong>(name.size() - 1), nullptr, 0, nullptr, 0) != UNZ_OK)
            break;
        const std::string entry(name.data());
        found = entry == "mod.json" || EndsWithNoCase(entry, "/mod.json");
    }
    unzClose(zip);
    return found;
}

bool Storage::IsAllowedModName(const std::string &name)
{
    if (name.empty() || name.size() > 200 || name.front() == '.' || !EndsWithNoCase(name, ".zip") || name.size() <= 4)
        return false;
    // No folders, no control characters, nothing FAT/exFAT drives reject.
    return std::none_of(name.begin(), name.end(), [](char c) {
        const unsigned char u = static_cast<unsigned char>(c);
        return u < 0x20 || u == 0x7F || std::strchr("/\\:*?\"<>|", c) != nullptr;
    });
}

std::vector<ModEntry> Storage::ListMods(const std::string &dir)
{
    std::vector<ModEntry> mods;
    std::error_code ec;
    for (const fs::directory_entry &entry : fs::directory_iterator(dir, ec)) {
        const std::string name = entry.path().filename().string();
        // Hidden files, our links to the USB drives, and A.I.R.'s own list.
        if (name.empty() || name.front() == '.' || name.rfind("usb-", 0) == 0 || name == "active-mods.json")
            continue;
        if (EndsWithNoCase(name, ".part"))
            continue;
        if (entry.is_directory(ec))
            mods.push_back({ name, 0, true });
        else if (EndsWithNoCase(name, ".zip"))
            mods.push_back({ name, FileSize(entry.path().string()), false });
    }
    std::sort(mods.begin(), mods.end(), [](const ModEntry &a, const ModEntry &b) { return a.name < b.name; });
    return mods;
}

std::vector<std::string> Storage::ActiveMods() const
{
    // {"ActiveMods": ["name.zip/folder", ...]}: a small, flat file A.I.R. writes itself.
    std::ifstream in(ModsDir() + "/active-mods.json");
    std::stringstream text;
    text << in.rdbuf();
    const std::string json = text.str();

    std::vector<std::string> active;
    const std::size_t list = json.find("\"ActiveMods\"");
    if (list == std::string::npos)
        return active;
    const std::size_t open  = json.find('[', list);
    const std::size_t close = json.find(']', open);
    if (open == std::string::npos || close == std::string::npos)
        return active;
    std::size_t at = open;
    while ((at = json.find('"', at + 1)) != std::string::npos && at < close) {
        std::string name;
        std::size_t end = at + 1;
        for (; end < close && json[end] != '"'; ++end) {
            if (json[end] == '\\' && end + 1 < close)
                ++end;
            name += json[end];
        }
        active.push_back(name);
        at = end;
    }
    return active;
}

std::vector<SaveFile> Storage::ListSaves() const
{
    std::vector<SaveFile> saves;
    const fs::path base(AppDataDir());
    for (const std::string &relative : SaveBackup::CollectFiles(AppDataDir())) {
        const fs::path path = base / relative;
        saves.push_back({ relative, FileSize(path.string()), ModificationTime(path) });
    }
    return saves;
}

bool Storage::ApplyPendingBackup() const
{
    const std::string pending = PendingBackupPath();
    if (FileSize(pending) == 0)
        return false;
    const std::string error = SaveBackup::Restore(pending, AppDataDir());
    std::error_code ec;
    fs::remove(pending, ec);
    Log::Printf("pending save backup: %s", error.empty() ? "restored" : error.c_str());
    return error.empty();
}

std::string Storage::PendingBackupPath() const { return IncomingDir() + "/" + kPendingBackup; }

} // namespace s3w
