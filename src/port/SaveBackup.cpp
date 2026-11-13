/**
 * @file SaveBackup.cpp
 * @brief Implementation of the save backup zip (see SaveBackup.h), with minizip.
 */
#include "SaveBackup.h"

#include <unzip.h>
#include <zip.h>

#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>

namespace s3w
{
namespace SaveBackup
{
namespace fs = std::filesystem;

namespace
{

/** Single files a backup holds. */
constexpr std::array<const char *, 4> kFiles = { "settings.json", "settings_input.json", "playerprogress.bin",
                                                 "mods/active-mods.json" };
/** Folders a backup holds, with everything in them. */
constexpr std::array<const char *, 2> kFolders = { "storage", "savestates" };
/** No single save file comes near this; it guards the restore against zip bombs. */
constexpr std::uint64_t kMaxEntryBytes = 64ull * 1024ull * 1024ull;
/** Entries per backup, for the same reason. */
constexpr int kMaxEntries = 4096;

/** Whether every part of @p relative is a plain name (no "", ".", ".." or hidden parts). */
bool HasPlainParts(const std::string &relative)
{
    std::size_t start = 0;
    while (start <= relative.size()) {
        const std::size_t end     = relative.find('/', start);
        const std::string segment = relative.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (segment.empty() || segment.front() == '.' || segment.find('\\') != std::string::npos)
            return false;
        if (end == std::string::npos)
            return true;
        start = end + 1;
    }
    return true;
}

/** Fills minizip's time fields from @p seconds (local time, as zip tools show it). */
void FillZipTime(std::time_t seconds, tm_zip *out)
{
    std::tm local{};
    localtime_r(&seconds, &local);
    out->tm_sec  = static_cast<uInt>(local.tm_sec);
    out->tm_min  = static_cast<uInt>(local.tm_min);
    out->tm_hour = static_cast<uInt>(local.tm_hour);
    out->tm_mday = static_cast<uInt>(local.tm_mday);
    out->tm_mon  = static_cast<uInt>(local.tm_mon);
    out->tm_year = static_cast<uInt>(local.tm_year + 1900);
}

} // namespace

bool IsAllowedPath(const std::string &relative)
{
    if (relative.empty() || relative.size() > 400 || relative.front() == '/' || !HasPlainParts(relative))
        return false;
    if (std::any_of(kFiles.begin(), kFiles.end(), [&](const char *file) { return relative == file; }))
        return true;
    return std::any_of(kFolders.begin(), kFolders.end(), [&](const char *folder) {
        const std::string prefix = std::string(folder) + "/";
        return relative.size() > prefix.size() && relative.compare(0, prefix.size(), prefix) == 0;
    });
}

std::vector<std::string> CollectFiles(const std::string &appDataDir)
{
    std::vector<std::string> files;
    std::error_code ec;
    const fs::path base(appDataDir);
    for (const char *file : kFiles) {
        if (fs::is_regular_file(base / file, ec))
            files.emplace_back(file);
    }
    for (const char *folder : kFolders) {
        for (fs::recursive_directory_iterator it(base / folder, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec))
                continue;
            const std::string relative = fs::relative(it->path(), base, ec).generic_string();
            if (!ec && IsAllowedPath(relative))
                files.push_back(relative);
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::string Create(const std::string &appDataDir, const std::string &zipPath)
{
    zipFile zip = zipOpen64(zipPath.c_str(), APPEND_STATUS_CREATE);
    if (!zip)
        return "Could not create the backup file.";

    std::string error;
    std::vector<char> buffer(64 * 1024);
    for (const std::string &relative : CollectFiles(appDataDir)) {
        const fs::path path = fs::path(appDataDir) / relative;
        std::ifstream in(path, std::ios::binary);
        if (!in)
            continue; // removed meanwhile

        zip_fileinfo info{};
        std::error_code ec;
        const auto writeTime = fs::last_write_time(path, ec);
        const auto systemTime = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            writeTime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
        FillZipTime(ec ? std::time(nullptr) : std::chrono::system_clock::to_time_t(systemTime), &info.tmz_date);

        if (zipOpenNewFileInZip64(zip, relative.c_str(), &info, nullptr, 0, nullptr, 0, nullptr, Z_DEFLATED,
                                  Z_DEFAULT_COMPRESSION, 0) != ZIP_OK) {
            error = "Could not add " + relative + " to the backup.";
            break;
        }
        while (in) {
            in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize length = in.gcount();
            if (length > 0 && zipWriteInFileInZip(zip, buffer.data(), static_cast<unsigned>(length)) != ZIP_OK) {
                error = "Could not write " + relative + " to the backup.";
                break;
            }
        }
        zipCloseFileInZip(zip);
        if (!error.empty())
            break;
    }
    if (zipClose(zip, "Sonic 3 A.I.R. saves (webOS)") != ZIP_OK && error.empty())
        error = "Could not finish the backup file.";
    return error;
}

std::string Validate(const std::string &zipPath)
{
    unzFile zip = unzOpen64(zipPath.c_str());
    if (!zip)
        return "That is not a zip file.";
    int entries        = 0;
    std::string reason;
    for (int status = unzGoToFirstFile(zip); status == UNZ_OK; status = unzGoToNextFile(zip)) {
        std::array<char, 512> name{};
        unz_file_info64 info{};
        if (unzGetCurrentFileInfo64(zip, &info, name.data(), static_cast<uLong>(name.size() - 1), nullptr, 0, nullptr, 0) != UNZ_OK) {
            reason = "The zip file is damaged.";
            break;
        }
        const std::string entry(name.data());
        if (!entry.empty() && entry.back() == '/')
            continue; // folder entries some zip tools add
        if (!IsAllowedPath(entry)) {
            reason = "\"" + entry + "\" is not part of the saves; only backups made by this page can be restored.";
            break;
        }
        if (info.uncompressed_size > kMaxEntryBytes || ++entries > kMaxEntries) {
            reason = "The backup is larger than any saves could be.";
            break;
        }
    }
    unzClose(zip);
    if (reason.empty() && entries == 0)
        reason = "The zip file holds no saves.";
    return reason;
}

std::string Restore(const std::string &zipPath, const std::string &appDataDir)
{
    const std::string invalid = Validate(zipPath);
    if (!invalid.empty())
        return invalid;

    unzFile zip = unzOpen64(zipPath.c_str());
    if (!zip)
        return "That is not a zip file.";
    std::string error;
    std::vector<char> buffer(64 * 1024);
    for (int status = unzGoToFirstFile(zip); status == UNZ_OK && error.empty(); status = unzGoToNextFile(zip)) {
        std::array<char, 512> name{};
        unz_file_info64 info{};
        unzGetCurrentFileInfo64(zip, &info, name.data(), static_cast<uLong>(name.size() - 1), nullptr, 0, nullptr, 0);
        const std::string entry(name.data());
        if (entry.empty() || entry.back() == '/')
            continue;

        const fs::path destination = fs::path(appDataDir) / entry;
        const fs::path partPath    = destination.string() + ".part";
        std::error_code ec;
        fs::create_directories(destination.parent_path(), ec);
        if (unzOpenCurrentFile(zip) != UNZ_OK) {
            error = "Could not read " + entry + " from the backup.";
            break;
        }
        std::FILE *out = std::fopen(partPath.c_str(), "wb");
        if (!out) {
            unzCloseCurrentFile(zip);
            error = "Could not write " + entry + ".";
            break;
        }
        std::uint64_t total = 0;
        int length          = 0;
        while ((length = unzReadCurrentFile(zip, buffer.data(), static_cast<unsigned>(buffer.size()))) > 0) {
            total += static_cast<std::uint64_t>(length);
            if (total > kMaxEntryBytes || std::fwrite(buffer.data(), 1, static_cast<std::size_t>(length), out) != static_cast<std::size_t>(length)) {
                length = -1;
                break;
            }
        }
        const bool flushed = std::fflush(out) == 0 && fsync(fileno(out)) == 0;
        std::fclose(out);
        // unzCloseCurrentFile checks the entry's CRC.
        if (unzCloseCurrentFile(zip) != UNZ_OK || length < 0 || !flushed) {
            fs::remove(partPath, ec);
            error = "Could not restore " + entry + " (damaged backup, or the TV is full).";
            break;
        }
        fs::rename(partPath, destination, ec);
        if (ec) {
            fs::remove(partPath, ec);
            error = "Could not put " + entry + " in place.";
        }
    }
    unzClose(zip);
    return error;
}

} // namespace SaveBackup
} // namespace s3w
