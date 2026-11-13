/**
 * @file UsbAccess.cpp
 * @brief Implementation of root access, USB drive discovery and placement of
 *        the drives' game files (see UsbAccess.h).
 */
#include "UsbAccess.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace s3w
{
namespace fs = std::filesystem;

namespace
{

/** Where webOS (and some other Linux setups) mount removable drives. */
constexpr std::array<const char *, 4> kUsbMountPrefixes = { "/tmp/usb/", "/media/usb", "/mnt/usb", "/run/media/" };
/** The Homebrew Channel's root command endpoint. */
constexpr const char *kHbChannelExec = "luna://org.webosbrew.hbchannel.service/exec";
/** luna-send-pub is the bus client apps may use; luna-send needs root. */
constexpr std::array<const char *, 2> kLunaClients = { "/usr/bin/luna-send-pub", "/usr/bin/luna-send" };
/** A drive's mods appear in the mods folder under this prefix plus the drive's label (ModLinkName). */
constexpr const char *kModLinkPrefix = "usb-";

/** Runs @p command with /bin/sh and captures stdout. @return The exit status, or -1. */
int RunShell(const std::string &command, std::string *output)
{
    std::FILE *pipe = popen(command.c_str(), "r");
    if (!pipe)
        return -1;
    std::string captured;
    std::array<char, 4096> buffer{};
    std::size_t length = 0;
    while ((length = std::fread(buffer.data(), 1, buffer.size(), pipe)) > 0)
        captured.append(buffer.data(), length);
    const int status = pclose(pipe);
    if (output)
        *output = std::move(captured);
    return status;
}

/** Escapes @p text as a JSON string body. */
std::string JsonEscape(const std::string &text)
{
    std::string out;
    for (const char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char hex[8];
                    std::snprintf(hex, sizeof(hex), "\\u%04x", c);
                    out += hex;
                }
                else {
                    out += c;
                }
        }
    }
    return out;
}

/**
 * Extracts the string value of "@p key" from a flat JSON reply (luna-send
 * output). Handles the escapes JSON strings use for shell output.
 * @return False if the key is missing.
 */
bool JsonStringField(const std::string &json, const std::string &key, std::string *value)
{
    const std::string needle = "\"" + key + "\"";
    std::size_t at           = json.find(needle);
    if (at == std::string::npos)
        return false;
    at = json.find('"', json.find(':', at + needle.size()));
    if (at == std::string::npos)
        return false;

    std::string out;
    for (std::size_t i = at + 1; i < json.size(); ++i) {
        const char c = json[i];
        if (c == '"')
            break;
        if (c != '\\' || i + 1 >= json.size()) {
            out += c;
            continue;
        }
        const char escaped = json[++i];
        switch (escaped) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'u':
                if (i + 4 < json.size()) {
                    out += static_cast<char>(std::strtol(json.substr(i + 1, 4).c_str(), nullptr, 16));
                    i += 4;
                }
                break;
            default: out += escaped; break;
        }
    }
    *value = std::move(out);
    return true;
}

/** Decodes the octal escapes /proc/mounts uses in paths ("\040" = space). */
std::string DecodeMountPath(const std::string &raw)
{
    std::string out;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\\' && i + 3 < raw.size()) {
            const std::string digits = raw.substr(i + 1, 3);
            if (std::all_of(digits.begin(), digits.end(), [](char d) { return d >= '0' && d <= '7'; })) {
                out += static_cast<char>(std::strtol(digits.c_str(), nullptr, 8));
                i += 3;
                continue;
            }
        }
        out += raw[i];
    }
    return out;
}

bool IsUsbMountPoint(const std::string &path)
{
    return std::any_of(kUsbMountPrefixes.begin(), kUsbMountPrefixes.end(),
                       [&](const char *prefix) { return path.rfind(prefix, 0) == 0; });
}

/** Mount points (field 2) of the /proc/mounts text @p mounts that look like USB drives. */
std::vector<std::string> ParseUsbMounts(const std::string &mounts)
{
    std::vector<std::string> found;
    std::istringstream lines(mounts);
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream fields(line);
        std::string device, mountPoint;
        if (!(fields >> device >> mountPoint))
            continue;
        const std::string decoded = DecodeMountPath(mountPoint);
        if (IsUsbMountPoint(decoded) && std::find(found.begin(), found.end(), decoded) == found.end())
            found.push_back(decoded);
    }
    return found;
}

/** Whether the app can create (and remove) a file in @p dir. */
bool CanWrite(const std::string &dir)
{
    const std::string probe = dir + "/.s3air_webos_write_test";
    const int fd            = open(probe.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0)
        return false;
    close(fd);
    unlink(probe.c_str());
    return true;
}

/** Whether the app can list @p dir. */
bool CanRead(const std::string &dir)
{
    DIR *handle = opendir(dir.c_str());
    if (!handle)
        return false;
    closedir(handle);
    return true;
}

/** True if @p path is a mount point in /proc/mounts (used to spot our own bind mounts). */
bool IsMountedAt(const std::string &path)
{
    std::ifstream mounts("/proc/mounts");
    std::string device, mountPoint, rest;
    while (mounts >> device >> mountPoint && std::getline(mounts, rest)) {
        if (DecodeMountPath(mountPoint) == path)
            return true;
    }
    return false;
}

std::size_t Index(GameFile file) { return file == GameFile::Rom ? 0 : 1; }

} // namespace

std::string UsbAccess::ModLinkName(const std::string &label)
{
    // Only characters that are safe in a folder name ("sda1").
    std::string safe;
    for (const char c : label)
        safe += std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' ? c : '_';
    return kModLinkPrefix + (safe.empty() ? std::string("drive") : safe);
}

std::string UsbAccess::ShellQuote(const std::string &text)
{
    std::string out = "'";
    for (const char c : text) {
        if (c == '\'')
            out += "'\\''";
        else
            out += c;
    }
    return out + "'";
}

void UsbAccess::ProbeRoot()
{
    RootStatus status;
    status.runningAsRoot = geteuid() == 0;
    if (status.runningAsRoot) {
        status.detail = "The app runs as root.";
    }
    else {
        // Ask the Homebrew Channel to run "id -u" as root.
        std::string luna;
        for (const char *client : kLunaClients) {
            if (access(client, X_OK) == 0) {
                luna = client;
                break;
            }
        }
        if (luna.empty()) {
            status.detail = "No luna-send client available to this app.";
        }
        else {
            const std::string payload = "{\"command\":\"id -u\"}";
            std::string reply;
            RunShell(luna + " -n 1 -w 5000 " + kHbChannelExec + " " + ShellQuote(payload) + " 2>&1", &reply);
            std::string stdoutText;
            if (JsonStringField(reply, "stdoutString", &stdoutText) && stdoutText.rfind("0", 0) == 0) {
                status.homebrewChannel = true;
                status.detail          = "Root commands run through the Homebrew Channel.";
            }
            else {
                std::string error;
                if (!JsonStringField(reply, "errorText", &error))
                    error = reply.substr(0, 160);
                while (!error.empty() && (error.back() == '\n' || error.back() == ' '))
                    error.pop_back();
                status.detail = "Homebrew Channel root commands unavailable" + (error.empty() ? std::string(".") : ": " + error);
            }
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    root_ = status;
    std::printf("[S3AIR/webOS] root: %s\n", status.detail.c_str());
}

RootStatus UsbAccess::Root() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return root_;
}

bool UsbAccess::RunAsRoot(const std::string &command, std::string *output)
{
    const RootStatus status = Root();
    if (status.runningAsRoot)
        return RunShell("sh -c " + ShellQuote(command), output) == 0;
    if (!status.homebrewChannel)
        return false;

    std::string luna;
    for (const char *client : kLunaClients) {
        if (access(client, X_OK) == 0) {
            luna = client;
            break;
        }
    }
    // The exec method reports success through returnValue; append a marker to
    // learn the command's own exit status as well.
    const std::string wrapped = "(" + command + ") && echo __S3W_OK__";
    const std::string payload = "{\"command\":\"" + JsonEscape(wrapped) + "\"}";
    std::string reply;
    RunShell(luna + " -n 1 -w 60000 " + kHbChannelExec + " " + ShellQuote(payload) + " 2>&1", &reply);

    std::string stdoutText;
    if (!JsonStringField(reply, "stdoutString", &stdoutText))
        return false;
    const std::size_t marker = stdoutText.rfind("__S3W_OK__");
    if (output)
        *output = stdoutText.substr(0, marker == std::string::npos ? stdoutText.size() : marker);
    return marker != std::string::npos;
}

std::vector<std::string> UsbAccess::UsbMountPoints()
{
    // A stand-in for a drive, for tests on a desktop (no /tmp/usb there).
    if (const char *standIn = std::getenv("S3AIR_USB_ROOT")) {
        std::vector<std::string> found;
        std::error_code ec;
        for (const fs::directory_entry &entry : fs::directory_iterator(standIn, ec)) {
            if (entry.is_directory(ec))
                found.push_back(entry.path().string());
        }
        std::sort(found.begin(), found.end());
        return found;
    }

    std::string mounts;
    {
        std::ifstream file("/proc/mounts");
        std::ostringstream text;
        text << file.rdbuf();
        mounts = text.str();
    }
    std::vector<std::string> found = ParseUsbMounts(mounts);

    // Inside the jail /proc/mounts may not list the drives; root sees the real table.
    if (found.empty() && Root().Available()) {
        std::string rootMounts;
        if (RunAsRoot("cat /proc/mounts", &rootMounts))
            found = ParseUsbMounts(rootMounts);
    }
    return found;
}

std::uint64_t UsbAccess::SizeOf(const std::string &path, bool readable)
{
    if (readable)
        return Storage::FileSize(path);
    std::string bytes;
    if (Root().Available() && RunAsRoot("test -f " + ShellQuote(path) + " && stat -L -c %s " + ShellQuote(path), &bytes))
        return std::strtoull(bytes.c_str(), nullptr, 10);
    return 0;
}

void UsbAccess::InspectDrive(UsbDrive *drive)
{
    const bool root = Root().Available();
    struct statvfs info {};
    if (statvfs(drive->mountPoint.c_str(), &info) == 0 && info.f_blocks > 0) {
        drive->freeBytes  = static_cast<std::uint64_t>(info.f_bavail) * info.f_frsize;
        drive->totalBytes = static_cast<std::uint64_t>(info.f_blocks) * info.f_frsize;
        drive->spaceKnown = true;
    }
    else if (root) {
        // df -k prints: Filesystem 1K-blocks Used Available Use% Mounted-on
        std::string df;
        if (RunAsRoot("df -k " + ShellQuote(drive->mountPoint) + " | tail -n 1", &df)) {
            std::istringstream fields(df);
            std::string filesystem;
            unsigned long long blocks = 0, used = 0, available = 0;
            if (fields >> filesystem >> blocks >> used >> available) {
                drive->totalBytes = blocks * 1024ull;
                drive->freeBytes  = available * 1024ull;
                drive->spaceKnown = true;
            }
        }
    }

    drive->romBytes        = SizeOf(DrivePath(drive->mountPoint, GameFile::Rom), drive->readable);
    drive->soundtrackBytes = SizeOf(DrivePath(drive->mountPoint, GameFile::Soundtrack), drive->readable);

    const std::string modsDir = drive->mountPoint + "/" + kDriveModsFolder;
    if (drive->readable) {
        std::error_code ec;
        drive->modsFolder = fs::is_directory(modsDir, ec);
        if (drive->modsFolder)
            drive->mods = Storage::ListMods(modsDir);
    }
    else if (root) {
        // One line per entry: "<size>|<type>|<path>", as busybox stat prints it.
        std::string listing;
        drive->modsFolder = RunAsRoot("test -d " + ShellQuote(modsDir));
        if (drive->modsFolder && RunAsRoot("cd " + ShellQuote(modsDir) + " && for f in *; do [ -e \"$f\" ] && stat -c '%s|%F|%n' \"$f\"; done", &listing)) {
            std::istringstream lines(listing);
            std::string line;
            while (std::getline(lines, line)) {
                const std::size_t first  = line.find('|');
                const std::size_t second = line.find('|', first + 1);
                if (first == std::string::npos || second == std::string::npos)
                    continue;
                const std::string name = line.substr(second + 1);
                const bool folder      = line.compare(first + 1, second - first - 1, "directory") == 0;
                if (name.empty() || name.front() == '.')
                    continue;
                if (folder || Storage::IsAllowedModName(name))
                    drive->mods.push_back({ name, folder ? 0 : std::strtoull(line.c_str(), nullptr, 10), folder });
            }
            std::sort(drive->mods.begin(), drive->mods.end(), [](const ModEntry &a, const ModEntry &b) { return a.name < b.name; });
        }
    }
}

std::vector<UsbDrive> UsbAccess::ListDrives()
{
    std::vector<UsbDrive> drives;
    for (const std::string &mountPoint : UsbMountPoints()) {
        UsbDrive drive;
        drive.mountPoint = mountPoint;
        drive.label      = fs::path(mountPoint).filename().string();
        drive.readable   = CanRead(mountPoint);
        drive.writable   = drive.readable && CanWrite(mountPoint);
        InspectDrive(&drive);
        drives.push_back(std::move(drive));
    }
    return drives;
}

bool UsbAccess::FindDrive(const std::string &mountPoint, UsbDrive *drive)
{
    if (!IsUsbMount(mountPoint))
        return false;
    *drive            = UsbDrive{};
    drive->mountPoint = mountPoint;
    drive->label      = fs::path(mountPoint).filename().string();
    drive->readable   = CanRead(mountPoint);
    drive->writable   = drive->readable && CanWrite(mountPoint);
    InspectDrive(drive);
    return true;
}

bool UsbAccess::IsUsbMount(const std::string &mountPoint)
{
    if (mountPoint.empty())
        return false;
    const std::vector<std::string> mounts = UsbMountPoints();
    return std::find(mounts.begin(), mounts.end(), mountPoint) != mounts.end();
}

const char *UsbAccess::FileName(GameFile file) { return file == GameFile::Rom ? Storage::kRomName : Storage::kSoundtrackName; }

std::string UsbAccess::LocalPath(const Storage &storage, GameFile file)
{
    return file == GameFile::Rom ? storage.RomPath() : storage.SoundtrackPath();
}

std::string UsbAccess::DrivePath(const std::string &mountPoint, GameFile file)
{
    return mountPoint + "/" + kDriveFolder + "/" + FileName(file);
}

Source UsbAccess::LinkFile(const std::string &source, const std::string &local, bool readable)
{
    std::error_code ec;
    if (readable) {
        fs::create_symlink(source, local, ec);
        return ec ? Source::None : Source::UsbLink;
    }
    if (!Root().Available())
        return Source::None;
    // Root binds the file over an empty one, so the app can open it by that name.
    std::ofstream(local).close();
    if (RunAsRoot("mount --bind " + ShellQuote(source) + " " + ShellQuote(local)) && Storage::FileSize(local) > 0)
        return Source::UsbMount;
    RunAsRoot("umount " + ShellQuote(local));
    fs::remove(local, ec);
    std::printf("[S3AIR/webOS] could not make %s readable to the app\n", source.c_str());
    return Source::None;
}

void UsbAccess::ReleaseLink(const Storage &storage, GameFile file)
{
    std::lock_guard<std::recursive_mutex> placing(placeMutex_);
    const std::string local = LocalPath(storage, file);
    std::error_code ec;
    if (IsMountedAt(local)) {
        // Lazy: detaches it even while the game reads through it.
        RunAsRoot("umount -l " + ShellQuote(local));
        fs::remove(local, ec); // the empty file it covered
    }
    else if (fs::is_symlink(local, ec)) {
        fs::remove(local, ec);
    }
    else if (Storage::IsPlainFile(local) && Storage::FileSize(local) == 0) {
        fs::remove(local, ec); // left where a bind mount failed
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (sources_[Index(file)] != Source::Internal) {
        sources_[Index(file)] = Source::None;
        paths_[Index(file)].clear();
    }
}

void UsbAccess::ReleaseModLinks(const Storage &storage)
{
    std::error_code ec;
    std::vector<fs::path> links;
    for (const fs::directory_entry &entry : fs::directory_iterator(storage.ModsDir(), ec)) {
        if (entry.path().filename().string().rfind(kModLinkPrefix, 0) == 0)
            links.push_back(entry.path());
    }
    for (const fs::path &link : links) {
        const std::string path = link.string();
        if (IsMountedAt(path)) {
            RunAsRoot("umount -l " + ShellQuote(path));
            fs::remove(link, ec); // the empty folder it covered
        }
        else if (fs::is_symlink(link, ec)) {
            fs::remove(link, ec);
        }
        else if (fs::is_directory(link, ec) && fs::is_empty(link, ec)) {
            fs::remove(link, ec);
        }
    }
}

void UsbAccess::PlaceContent(const Storage &storage)
{
    std::lock_guard<std::recursive_mutex> placing(placeMutex_);
    const std::vector<UsbDrive> drives = ListDrives();
    std::error_code ec;

    for (const GameFile file : { GameFile::Rom, GameFile::Soundtrack }) {
        ReleaseLink(storage, file);
        const std::string local = LocalPath(storage, file);
        fs::create_directories(fs::path(local).parent_path(), ec);

        Source source = Source::None;
        std::string path;
        if (Storage::IsPlainFile(local)) {
            // A file on the TV wins: it is what the player put there.
            source = Source::Internal;
            path   = local;
        }
        else {
            for (const UsbDrive &drive : drives) {
                const std::uint64_t size = file == GameFile::Rom ? drive.romBytes : drive.soundtrackBytes;
                if (size == 0)
                    continue;
                const std::string drivePath = DrivePath(drive.mountPoint, file);
                source                      = LinkFile(drivePath, local, drive.readable);
                if (source != Source::None) {
                    path = drivePath;
                    break;
                }
            }
        }
        std::printf("[S3AIR/webOS] %s: %s\n", FileName(file), path.empty() ? "none" : path.c_str());
        std::lock_guard<std::mutex> lock(mutex_);
        sources_[Index(file)] = source;
        paths_[Index(file)]   = path;
    }

    // Each drive's mods folder appears as mods/usb-<label>.
    ReleaseModLinks(storage);
    std::vector<std::string> linked;
    for (const UsbDrive &drive : drives) {
        if (!drive.modsFolder)
            continue;
        const std::string source = drive.mountPoint + "/" + kDriveModsFolder;
        const std::string local  = storage.ModsDir() + "/" + ModLinkName(drive.label);
        bool done                = false;
        if (drive.readable) {
            fs::create_directory_symlink(source, local, ec);
            done = !ec;
        }
        else if (Root().Available()) {
            fs::create_directories(local, ec);
            done = RunAsRoot("mount --bind " + ShellQuote(source) + " " + ShellQuote(local)) && CanRead(local);
            if (!done) {
                RunAsRoot("umount " + ShellQuote(local));
                fs::remove(local, ec);
            }
        }
        if (done) {
            linked.push_back(drive.mountPoint);
            std::printf("[S3AIR/webOS] mods: %s linked\n", source.c_str());
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    modDrives_ = std::move(linked);
}

Source UsbAccess::ActiveSource(GameFile file) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return sources_[Index(file)];
}

std::string UsbAccess::ActivePath(GameFile file) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return paths_[Index(file)];
}

std::vector<std::string> UsbAccess::LinkedModDrives() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return modDrives_;
}

std::string UsbAccess::CreateDriveFolder(const std::string &mountPoint, const std::string &relative)
{
    const std::string folder = mountPoint + "/" + relative;
    std::error_code ec;
    if (CanWrite(mountPoint)) {
        fs::create_directories(folder, ec);
        return ec ? "Could not create the folder: " + ec.message() : std::string();
    }
    if (Root().Available())
        return RunAsRoot("mkdir -p " + ShellQuote(folder)) ? std::string() : "Root could not create the folder on the drive.";
    return "The app cannot write to this drive and has no root access.";
}

std::string UsbAccess::MoveWithRoot(const std::string &sourcePath, const std::string &destination)
{
    const std::string dir = fs::path(destination).parent_path().string();
    // mv across filesystems copies, then removes the source.
    if (RunAsRoot("mkdir -p " + ShellQuote(dir) + " && mv -f " + ShellQuote(sourcePath) + " " + ShellQuote(destination)))
        return {};
    return "Root could not move the file onto the drive.";
}

bool UsbAccess::RemoveFile(const std::string &path)
{
    std::error_code ec;
    if (fs::remove(path, ec) || (!ec && !fs::exists(fs::symlink_status(path, ec))))
        return true;
    return Root().Available() && RunAsRoot("rm -f " + ShellQuote(path));
}

bool UsbAccess::RemoveFolder(const std::string &path)
{
    std::error_code ec;
    fs::remove_all(path, ec);
    if (!ec && !fs::exists(fs::symlink_status(path, ec)))
        return true;
    return Root().Available() && RunAsRoot("rm -rf " + ShellQuote(path));
}

bool UsbAccess::RenameFile(const std::string &from, const std::string &to)
{
    std::error_code ec;
    fs::rename(from, to, ec);
    if (!ec)
        return true;
    return Root().Available() && RunAsRoot("mv -f " + ShellQuote(from) + " " + ShellQuote(to));
}

std::string UsbAccess::CopyFile(const std::string &source, const std::string &destination,
                                const std::function<void(std::uint64_t)> &onProgress)
{
    // Directly, with progress, when the app may read the source and write the destination's folder.
    const std::string folder = fs::path(destination).parent_path().string();
    std::FILE *in            = access(folder.c_str(), W_OK) == 0 ? std::fopen(source.c_str(), "rb") : nullptr;
    if (in) {
        std::FILE *out = std::fopen(destination.c_str(), "wb");
        if (!out) {
            std::fclose(in);
            return "Could not create " + destination + ".";
        }
        std::vector<char> buffer(1u << 20);
        std::uint64_t copied = 0;
        bool ok              = true;
        std::size_t length   = 0;
        while ((length = std::fread(buffer.data(), 1, buffer.size(), in)) > 0) {
            if (std::fwrite(buffer.data(), 1, length, out) != length) {
                ok = false; // disk full or I/O error
                break;
            }
            copied += length;
            onProgress(copied);
        }
        ok = ok && !std::ferror(in) && std::fflush(out) == 0 && fsync(fileno(out)) == 0;
        std::fclose(in);
        std::fclose(out);
        return ok ? std::string() : "The copy could not be written (is the destination full?).";
    }

    if (!Root().Available())
        return "The app cannot read the file or write there, and has no root access.";
    if (RunAsRoot("cp -f " + ShellQuote(source) + " " + ShellQuote(destination) + " && sync"))
        return {};
    return "Root could not copy the file there (is the destination full?).";
}

} // namespace s3w
