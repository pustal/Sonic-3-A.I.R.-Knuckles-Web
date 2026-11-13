/**
 * @file TransferServer.cpp
 * @brief Implementation of the transfer server (see TransferServer.h).
 *
 * HTTP API (all /api routes need the PIN, as an "X-Pin" header or ?pin=).
 * "drive" is "internal" (the TV, the default) or a USB drive's mount point.
 *   GET    /                               the transfer page (web/index.html, embedded at build time)
 *   GET    /api/status                     JSON: ROM, soundtrack, mods, drives, root, space, progress
 *   PUT    /api/file/:kind?drive=          raw body = the ROM (kind "rom") or the remastered soundtrack
 *                                          ("soundtrack"); other copies are removed
 *   POST   /api/file/:kind/move?drive=     moves it there
 *   DELETE /api/file/:kind                 removes it (every copy)
 *   PUT    /api/mods/:name?drive=          raw body = a mod's zip file
 *   POST   /api/mods/:name/move?from=&drive=   moves a mod between the TV and a drive
 *   DELETE /api/mods/:name?drive=          removes a mod
 *   GET    /api/saves                      JSON list of the save files
 *   GET    /api/saves/backup               the saves as a zip file
 *   PUT    /api/saves/backup               raw body = a backup zip to restore
 *   GET    /api/log                        the game's log file
 *   POST   /api/restart                    quits the game and starts the app again
 */
#include "TransferServer.h"

#include "MenuOverlay.h"
#include "NetInfo.h"
#include "SaveBackup.h"
#include "Storage.h"
#include "UsbAccess.h"
#include "WebPage.generated.h"

// Four workers: the page polls the status while an upload runs.
#define CPPHTTPLIB_THREAD_POOL_COUNT 4
#include <httplib.h>

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <random>
#include <sstream>
#include <system_error>

namespace s3w
{
namespace fs = std::filesystem;

namespace
{

constexpr const char *kJsonType  = "application/json";
constexpr const char *kTextType  = "text/plain; charset=utf-8";
constexpr const char *kPinHeader = "X-Pin";
constexpr const char *kPinFile   = "transfer_pin.txt";
constexpr const char *kInternal  = "internal";
constexpr int kPortAttempts      = 10;
/** The log can grow large after long sessions; the page gets its end. */
constexpr std::uint64_t kMaxLogBytes = 2ull * 1024ull * 1024ull;

/** Escapes @p text for use inside a JSON string literal. */
std::string JsonEscape(const std::string &text)
{
    std::string out;
    out.reserve(text.size() + 8);
    for (const char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                    out += buffer;
                }
                else {
                    out += c;
                }
        }
    }
    return out;
}

const char *JsonBool(bool value) { return value ? "true" : "false"; }

/** Sends a plain-text error. */
void Fail(httplib::Response &res, int status, const std::string &message)
{
    res.status = status;
    res.set_content(message, kTextType);
}

/** Reads the announced body size. @return False if the header is missing or malformed. */
bool ContentLength(const httplib::Request &req, std::uint64_t *out)
{
    const std::string value = req.get_header_value("Content-Length");
    if (value.empty() || !std::all_of(value.begin(), value.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }))
        return false;
    try {
        *out = static_cast<std::uint64_t>(std::stoull(value));
        return true;
    }
    catch (const std::exception &) {
        return false;
    }
}

/**
 * Streams the request body into @p path, refusing more than @p expected
 * bytes, and flushes it to storage.
 * @return Bytes written, or -1 on error.
 */
std::int64_t StreamBodyToFile(const httplib::ContentReader &reader, const std::string &path, std::uint64_t expected,
                              const std::function<void(std::uint64_t)> &onProgress)
{
    std::FILE *out = std::fopen(path.c_str(), "wb");
    if (!out)
        return -1;

    std::uint64_t written = 0;
    const bool completed  = reader([&](const char *data, std::size_t length) {
        if (written + length > expected)
            return false; // more than announced
        if (std::fwrite(data, 1, length, out) != length)
            return false; // disk full or I/O error
        written += length;
        onProgress(written);
        return true;
    });

    const bool flushed = std::fflush(out) == 0 && fsync(fileno(out)) == 0;
    std::fclose(out);
    if (!completed || !flushed)
        return -1;
    return static_cast<std::int64_t>(written);
}

/** Clears a "busy" flag when the request handler returns, however it returns. */
class BusyGuard
{
public:
    explicit BusyGuard(std::atomic<bool> &flag) : flag_(flag) {}
    ~BusyGuard() { flag_.store(false); }
    BusyGuard(const BusyGuard &)            = delete;
    BusyGuard &operator=(const BusyGuard &) = delete;

private:
    std::atomic<bool> &flag_;
};

/** Which single file a /api/file route is about. */
bool ParseKind(const std::string &kind, GameFile *file)
{
    if (kind == "rom")
        *file = GameFile::Rom;
    else if (kind == "soundtrack")
        *file = GameFile::Soundtrack;
    else
        return false;
    return true;
}

const char *KindLabel(GameFile file) { return file == GameFile::Rom ? "ROM" : "Remastered soundtrack"; }

const char *SourceName(Source source)
{
    switch (source) {
        case Source::Internal: return "internal";
        case Source::UsbLink: return "usb";
        case Source::UsbMount: return "usb-mount";
        default: return "none";
    }
}

/** Where an upload or move goes: the TV, or a USB drive. */
struct Target {
    bool usb = false;
    UsbDrive drive;          ///< The drive, for a USB target.
    std::string folder;      ///< Absolute folder the file goes in.
    std::uint64_t free = 0;  ///< Free bytes there.
    bool direct = true;      ///< The app writes there itself (else root moves it there).
};

/** The folder a single file or a mod goes in on the TV. */
std::string LocalFolder(const Storage &storage, bool mod, GameFile file)
{
    if (mod)
        return storage.ModsDir();
    return fs::path(UsbAccess::LocalPath(storage, file)).parent_path().string();
}

/** The folder a single file or a mod goes in on a drive. */
std::string DriveFolder(const std::string &mountPoint, bool mod)
{
    return mountPoint + "/" + (mod ? UsbAccess::kDriveModsFolder : UsbAccess::kDriveFolder);
}

/**
 * Reads the "drive" value @p drive (a USB mount point, or "internal") into
 * @p target. Answers @p res and returns false when it is not usable.
 */
bool ParseTarget(const std::string &drive, bool mod, GameFile file, Storage &storage, UsbAccess &usb, Target *target,
                 httplib::Response &res)
{
    if (drive.empty() || drive == kInternal) {
        target->folder = LocalFolder(storage, mod, file);
        target->free   = storage.FreeBytes();
        return true;
    }
    if (!usb.FindDrive(drive, &target->drive)) {
        Fail(res, 400, "That USB drive is not connected.");
        return false;
    }
    target->usb    = true;
    target->folder = DriveFolder(drive, mod);
    target->direct = target->drive.writable;
    target->free   = target->drive.spaceKnown ? target->drive.freeBytes : 0;
    if (!target->drive.writable && !usb.Root().Available()) {
        Fail(res, 403, "The app cannot write to this USB drive and has no root access.");
        return false;
    }
    if (!target->drive.spaceKnown) {
        Fail(res, 507, "The free space on this USB drive could not be read.");
        return false;
    }
    return true;
}

/** Where a single file is stored: on the TV first (it wins), else the first drive's. */
struct StoredFile {
    std::string path;  ///< Absolute path, empty when there is none.
    std::string drive; ///< Mount point of its drive, empty for the TV.
    std::uint64_t size = 0;
};

StoredFile FindStored(const Storage &storage, const std::vector<UsbDrive> &drives, GameFile file)
{
    StoredFile stored;
    const std::string local = UsbAccess::LocalPath(storage, file);
    if (Storage::IsPlainFile(local)) {
        stored.path = local;
        stored.size = Storage::FileSize(local);
        return stored;
    }
    for (const UsbDrive &drive : drives) {
        const std::uint64_t size = file == GameFile::Rom ? drive.romBytes : drive.soundtrackBytes;
        if (size == 0)
            continue;
        stored.path  = UsbAccess::DrivePath(drive.mountPoint, file);
        stored.drive = drive.mountPoint;
        stored.size  = size;
        return stored;
    }
    return stored;
}

/** Removes every stored copy of @p file except @p keep: on the TV and on each drive. */
void RemoveOtherCopies(Storage &storage, UsbAccess &usb, GameFile file, const std::string &keep)
{
    usb.ReleaseLink(storage, file);
    const std::string local = UsbAccess::LocalPath(storage, file);
    if (local != keep && Storage::IsPlainFile(local))
        usb.RemoveFile(local);
    for (const UsbDrive &drive : usb.ListDrives()) {
        const std::uint64_t size = file == GameFile::Rom ? drive.romBytes : drive.soundtrackBytes;
        const std::string path   = UsbAccess::DrivePath(drive.mountPoint, file);
        if (size != 0 && path != keep)
            usb.RemoveFile(path);
    }
}

/** Whether the mod folder @p name (at @p prefix in the mods folder) is one A.I.R. has active. */
bool IsModActive(const std::vector<std::string> &active, const std::string &prefix, const std::string &name)
{
    const std::string base = prefix + name;
    return std::any_of(active.begin(), active.end(), [&](const std::string &entry) {
        return entry == base || entry.rfind(base + "/", 0) == 0;
    });
}

/** The JSON array of @p mods. */
std::string ModsJson(const std::vector<ModEntry> &mods, const std::vector<std::string> &active, const std::string &prefix)
{
    std::ostringstream json;
    json << "[";
    bool first = true;
    for (const ModEntry &mod : mods) {
        json << (first ? "" : ",") << "{\"name\":\"" << JsonEscape(mod.name) << "\",\"size\":" << mod.size
             << ",\"folder\":" << JsonBool(mod.folder) << ",\"active\":" << JsonBool(IsModActive(active, prefix, mod.name)) << "}";
        first = false;
    }
    json << "]";
    return json.str();
}

} // namespace

std::string LoadOrCreatePin(const Storage &storage)
{
    const std::string path = storage.PathFor(kPinFile);
    std::ifstream in(path);
    std::string pin;
    if (in >> pin && pin.size() == 4 && std::all_of(pin.begin(), pin.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }))
        return pin;

    std::random_device device;
    std::uniform_int_distribution<int> digits(0, 9999);
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), "%04d", digits(device));
    std::ofstream(path) << buffer << "\n";
    return buffer;
}

TransferServer::TransferServer(Storage &storage, UsbAccess &usb, std::string pin)
    : storage_(storage), usb_(usb), pin_(std::move(pin))
{
}

TransferServer::~TransferServer() { Stop(); }

bool TransferServer::Start()
{
    if (server_)
        return port_.load() != 0;

    server_ = std::make_unique<httplib::Server>();
    server_->new_task_queue = [] { return new httplib::ThreadPool(CPPHTTPLIB_THREAD_POOL_COUNT); };
    server_->set_payload_max_length(std::numeric_limits<std::size_t>::max());
    server_->set_read_timeout(30, 0);
    server_->set_write_timeout(30, 0);
    RegisterRoutes();

    for (int attempt = 0; attempt < kPortAttempts; ++attempt) {
        const int port = kPreferredPort + attempt;
        if (server_->bind_to_port("0.0.0.0", port)) {
            port_.store(static_cast<std::uint16_t>(port));
            thread_ = std::thread([this] { server_->listen_after_bind(); });
            return true;
        }
    }
    server_.reset();
    return false;
}

void TransferServer::Stop()
{
    if (!server_)
        return;
    server_->stop();
    if (thread_.joinable())
        thread_.join();
    server_.reset();
    port_.store(0);
}

TransferStatus TransferServer::Snapshot() const
{
    std::lock_guard<std::mutex> lock(statusMutex_);
    return status_;
}

void TransferServer::PublishEvent(const std::string &message)
{
    std::lock_guard<std::mutex> lock(statusMutex_);
    status_.lastEvent = message;
    ++status_.eventSerial;
    std::printf("[S3AIR/webOS] transfer: %s\n", message.c_str());
}

void TransferServer::MarkRestartPending()
{
    if (setupMode_.load())
        return;
    std::lock_guard<std::mutex> lock(statusMutex_);
    status_.restartPending = true;
}

void TransferServer::SetProgress(bool busy, const std::string &what, std::uint64_t done, std::uint64_t total)
{
    std::lock_guard<std::mutex> lock(statusMutex_);
    status_.busy       = busy;
    status_.busyWhat   = what;
    status_.doneBytes  = done;
    status_.totalBytes = total;
}

void TransferServer::RegisterRoutes()
{
    httplib::Server &server = *server_;

    // Every request: private networks only, and /api needs the PIN shown on the TV.
    server.set_pre_routing_handler([this](const httplib::Request &req, httplib::Response &res) {
        if (!IsPrivateNetworkAddress(req.remote_addr)) {
            Fail(res, 403, "Only devices on the local network may connect.");
            return httplib::Server::HandlerResponse::Handled;
        }
        const std::string pin = req.has_header(kPinHeader) ? req.get_header_value(kPinHeader) : req.get_param_value("pin");
        if (req.path.rfind("/api/", 0) == 0 && pin != pin_) {
            Fail(res, 401, "Wrong or missing PIN. Use the PIN shown on the TV.");
            return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });

    server.Get("/", [](const httplib::Request &, httplib::Response &res) {
        res.set_header("Cache-Control", "no-store");
        res.set_content(kWebPageHtml, "text/html; charset=utf-8");
    });

    server.Get("/api/status", [this](const httplib::Request &, httplib::Response &res) {
        const TransferStatus status         = Snapshot();
        const RootStatus root               = usb_.Root();
        const std::vector<UsbDrive> drives  = usb_.ListDrives();
        const std::vector<std::string> active = storage_.ActiveMods();

        std::ostringstream json;
        json << "{\"version\":\"" << S3W_VERSION << "\",\"gameVersion\":\"" << S3W_AIR_VERSION << "\""
             << ",\"mode\":\"" << (setupMode_.load() ? "setup" : "game") << "\""
             << ",\"persistent\":" << JsonBool(storage_.Persistent())
             << ",\"freeBytes\":" << storage_.FreeBytes() << ",\"safetyMarginBytes\":" << Storage::kSafetyMarginBytes
             << ",\"driveFolder\":\"" << UsbAccess::kDriveFolder << "\""
             << ",\"restartPending\":" << JsonBool(status.restartPending)
             << ",\"event\":{\"serial\":" << status.eventSerial << ",\"text\":\"" << JsonEscape(status.lastEvent) << "\"}"
             << ",\"progress\":{\"busy\":" << JsonBool(status.busy) << ",\"what\":\"" << JsonEscape(status.busyWhat)
             << "\",\"done\":" << status.doneBytes << ",\"total\":" << status.totalBytes << "}"
             << ",\"root\":{\"process\":" << JsonBool(root.runningAsRoot) << ",\"homebrewChannel\":"
             << JsonBool(root.homebrewChannel) << ",\"detail\":\"" << JsonEscape(root.detail) << "\"}";

        for (const GameFile file : { GameFile::Rom, GameFile::Soundtrack }) {
            const StoredFile stored = FindStored(storage_, drives, file);
            json << ",\"" << (file == GameFile::Rom ? "rom" : "soundtrack") << "\":{\"present\":" << JsonBool(!stored.path.empty())
                 << ",\"location\":\"" << (stored.path.empty() ? "none" : stored.drive.empty() ? "internal" : "usb") << "\""
                 << ",\"drive\":\"" << JsonEscape(stored.drive) << "\",\"size\":" << stored.size
                 << ",\"source\":\"" << SourceName(usb_.ActiveSource(file)) << "\",\"file\":\"" << UsbAccess::FileName(file) << "\"}";
        }

        json << ",\"mods\":" << ModsJson(Storage::ListMods(storage_.ModsDir()), active, "");
        json << ",\"drives\":[";
        bool first = true;
        for (const UsbDrive &drive : drives) {
            json << (first ? "" : ",") << "{\"mount\":\"" << JsonEscape(drive.mountPoint) << "\",\"label\":\""
                 << JsonEscape(drive.label) << "\",\"spaceKnown\":" << JsonBool(drive.spaceKnown)
                 << ",\"freeBytes\":" << drive.freeBytes << ",\"totalBytes\":" << drive.totalBytes
                 << ",\"readable\":" << JsonBool(drive.readable) << ",\"writable\":" << JsonBool(drive.writable)
                 << ",\"romBytes\":" << drive.romBytes << ",\"soundtrackBytes\":" << drive.soundtrackBytes
                 << ",\"modsFolder\":" << JsonBool(drive.modsFolder)
                 << ",\"mods\":" << ModsJson(drive.mods, active, UsbAccess::ModLinkName(drive.label) + "/") << "}";
            first = false;
        }
        json << "]}";
        res.set_content(json.str(), kJsonType);
    });

    server.Get("/api/log", [this](const httplib::Request &, httplib::Response &res) {
        const std::string path   = storage_.AppDataDir() + "/logfile.txt";
        const std::uint64_t size = Storage::FileSize(path);
        if (size == 0)
            return Fail(res, 404, "The game has not written a log yet.");
        std::ifstream in(path, std::ios::binary);
        const std::uint64_t skip = size > kMaxLogBytes ? size - kMaxLogBytes : 0;
        in.seekg(static_cast<std::streamoff>(skip));
        std::string text(static_cast<std::size_t>(size - skip), '\0');
        in.read(&text[0], static_cast<std::streamsize>(text.size()));
        text.resize(static_cast<std::size_t>(in.gcount()));
        res.set_header("Cache-Control", "no-store");
        res.set_content(std::move(text), kTextType);
    });

    server.Post("/api/restart", [this](const httplib::Request &, httplib::Response &res) {
        if (setupMode_.load())
            return Fail(res, 409, "The game has not started yet: it starts on its own once the ROM is installed.");
        PublishEvent("Restarting the game...");
        MenuOverlay::RequestRestart();
        res.set_content("restarting", kTextType);
    });

    RegisterFileRoutes();
    RegisterModRoutes();
    RegisterSaveRoutes();
}

void TransferServer::RegisterFileRoutes()
{
    httplib::Server &server = *server_;

    // ---- The ROM and the remastered soundtrack: one copy, on the TV or a drive ----
    server.Put("/api/file/:kind", [this](const httplib::Request &req, httplib::Response &res, const httplib::ContentReader &reader) {
        GameFile file;
        if (!ParseKind(req.path_params.at("kind"), &file))
            return Fail(res, 404, "Unknown file kind.");
        const char *label = KindLabel(file);

        std::uint64_t expected = 0;
        if (!ContentLength(req, &expected) || expected == 0)
            return Fail(res, 411, "The upload must state its size (Content-Length).");
        if (file == GameFile::Rom && expected != Storage::kRomBytes) {
            return Fail(res, 415, "That is not the Sonic 3 & Knuckles ROM A.I.R. needs: it must be exactly 4 MB ("
                                      + std::to_string(Storage::kRomBytes) + " bytes), this file has " + std::to_string(expected) + ".");
        }
        if (file == GameFile::Soundtrack && expected > Storage::kMaxSoundtrackBytes)
            return Fail(res, 413, "That is far larger than the remastered soundtrack; refusing " + HumanBytes(expected) + ".");

        Target target;
        if (!ParseTarget(req.get_param_value("drive"), false, file, storage_, usb_, &target, res))
            return;
        const std::uint64_t needed = expected + Storage::kSafetyMarginBytes;
        if (needed > target.free) {
            return Fail(res, 507, std::string("Not enough space on the ") + (target.usb ? "USB drive" : "TV") + ": this file needs "
                                      + HumanBytes(needed) + " (including a safety margin), " + HumanBytes(target.free) + " is free.");
        }
        // A drive the app cannot write to: the upload waits in the data folder until root moves it.
        const bool staged = target.usb && !target.direct;
        if (staged && needed > storage_.FreeBytes()) {
            return Fail(res, 507, "The app writes to this USB drive through root, so the upload first goes to the TV, which has "
                                      + HumanBytes(storage_.FreeBytes()) + " free; it needs " + HumanBytes(needed) + ".");
        }

        if (busy_.exchange(true))
            return Fail(res, 409, "Another upload or move is already running.");
        BusyGuard busyGuard(busy_);

        std::error_code ec;
        if (target.usb) {
            const std::string folderError = usb_.CreateDriveFolder(target.drive.mountPoint, UsbAccess::kDriveFolder);
            if (!folderError.empty())
                return Fail(res, 500, folderError);
        }
        fs::create_directories(target.usb ? storage_.IncomingDir() : target.folder, ec);

        SetProgress(true, label, 0, expected);
        PublishEvent(std::string("Receiving the ") + label + (target.usb ? " for the USB drive" : "") + "...");
        const std::string partPath =
            (staged ? storage_.IncomingDir() : target.folder) + "/" + UsbAccess::FileName(file) + ".part";
        const std::int64_t written = StreamBodyToFile(reader, partPath, expected, [this, label, expected](std::uint64_t total) {
            SetProgress(true, label, total, expected);
        });
        SetProgress(false, label, written < 0 ? 0 : static_cast<std::uint64_t>(written), expected);

        if (written < 0 || static_cast<std::uint64_t>(written) != expected) {
            usb_.RemoveFile(partPath);
            PublishEvent("Upload interrupted. Please try again.");
            return Fail(res, 400, "The upload was interrupted or could not be written.");
        }
        if (file == GameFile::Rom && !Storage::IsValidRom(partPath)) {
            usb_.RemoveFile(partPath);
            PublishEvent("That file is not the Sonic 3 & Knuckles ROM.");
            return Fail(res, 415, "That file is not the Sonic 3 & Knuckles ROM A.I.R. needs (\"Sonic_Knuckles_wSonic3.bin\", "
                                  "Sonic & Knuckles locked on to Sonic 3, as the Steam and Mega Drive Classics releases include it).");
        }
        if (file == GameFile::Soundtrack && !Storage::IsSoundtrackPackage(partPath)) {
            usb_.RemoveFile(partPath);
            PublishEvent("That file is not A.I.R.'s audioremaster.bin.");
            return Fail(res, 415, "That file is not a Sonic 3 A.I.R. package this version reads. Upload \"audioremaster.bin\" "
                                  "from the data folder of a current official Sonic 3 A.I.R. download.");
        }

        // Into place, then every other copy goes, so exactly one is left.
        const std::string destination = target.folder + "/" + UsbAccess::FileName(file);
        usb_.ReleaseLink(storage_, file); // a link or bind mount may hold the TV's name
        const bool placed = staged ? usb_.MoveWithRoot(partPath, destination).empty() : usb_.RenameFile(partPath, destination);
        if (!placed) {
            usb_.RemoveFile(partPath);
            usb_.PlaceContent(storage_);
            return Fail(res, 500, "Could not move the file into place.");
        }
        RemoveOtherCopies(storage_, usb_, file, destination);
        usb_.PlaceContent(storage_);

        MarkRestartPending();
        PublishEvent(std::string(label) + " installed" + (target.usb ? " on the USB drive" : " on the TV") + ".");
        res.set_content(setupMode_.load() ? "installed" : "staged", kTextType);
    });

    server.Post("/api/file/:kind/move", [this](const httplib::Request &req, httplib::Response &res) {
        GameFile file;
        if (!ParseKind(req.path_params.at("kind"), &file))
            return Fail(res, 404, "Unknown file kind.");
        const char *label       = KindLabel(file);
        const StoredFile source = FindStored(storage_, usb_.ListDrives(), file);
        if (source.path.empty())
            return Fail(res, 404, std::string("There is no ") + label + " to move.");

        Target target;
        if (!ParseTarget(req.get_param_value("drive"), false, file, storage_, usb_, &target, res))
            return;
        if (target.usb == !source.drive.empty() && target.drive.mountPoint == source.drive)
            return Fail(res, 400, std::string("It is already on ") + (target.usb ? "that USB drive." : "the TV."));
        const std::uint64_t needed = source.size + Storage::kSafetyMarginBytes;
        if (needed > target.free) {
            return Fail(res, 507, "Not enough space there: it needs " + HumanBytes(needed) + " (including a safety margin), "
                                      + HumanBytes(target.free) + " is free.");
        }

        if (busy_.exchange(true))
            return Fail(res, 409, "Another upload or move is already running.");
        BusyGuard busyGuard(busy_);

        std::error_code ec;
        if (target.usb) {
            const std::string folderError = usb_.CreateDriveFolder(target.drive.mountPoint, UsbAccess::kDriveFolder);
            if (!folderError.empty())
                return Fail(res, 500, folderError);
        }
        fs::create_directories(target.folder, ec);

        const std::string where       = target.usb ? "the USB drive" : "the TV";
        const std::string destination = target.folder + "/" + UsbAccess::FileName(file);
        const std::string partPath    = destination + ".part";
        SetProgress(true, label, 0, source.size);
        PublishEvent(std::string("Moving the ") + label + " to " + where + "...");

        // Copy, then remove the original: the TV and a drive are different file systems.
        const std::string copyError = usb_.CopyFile(source.path, partPath, [this, label, &source](std::uint64_t total) {
            SetProgress(true, label, total, source.size);
        });
        SetProgress(false, label, source.size, source.size);
        // A copy root made may not be readable to the app (an unreadable drive): only a readable one is size-checked.
        const std::uint64_t copiedSize = Storage::FileSize(partPath);
        if (!copyError.empty() || (copiedSize != 0 && copiedSize != source.size)) {
            usb_.RemoveFile(partPath);
            PublishEvent(std::string("Could not move the ") + label + ".");
            return Fail(res, 500, (copyError.empty() ? std::string("The copy came out the wrong size.") : copyError)
                                      + " The original is untouched.");
        }
        usb_.ReleaseLink(storage_, file); // a link or bind mount may hold the TV's name
        if (!usb_.RenameFile(partPath, destination)) {
            usb_.RemoveFile(partPath);
            usb_.PlaceContent(storage_);
            return Fail(res, 500, "Could not move the copy into place. The original is untouched.");
        }
        if (!usb_.RemoveFile(source.path)) {
            // Keep exactly one copy: the original stays, the new one goes.
            usb_.RemoveFile(destination);
            usb_.PlaceContent(storage_);
            return Fail(res, 500, "Could not remove the original, so it was left where it was.");
        }
        usb_.PlaceContent(storage_);
        MarkRestartPending();
        PublishEvent(std::string(label) + " moved to " + where + ".");
        res.set_content("moved", kTextType);
    });

    server.Delete("/api/file/:kind", [this](const httplib::Request &req, httplib::Response &res) {
        GameFile file;
        if (!ParseKind(req.path_params.at("kind"), &file))
            return Fail(res, 404, "Unknown file kind.");
        if (busy_.exchange(true))
            return Fail(res, 409, "An upload or move is running; try again when it is done.");
        BusyGuard busyGuard(busy_);
        RemoveOtherCopies(storage_, usb_, file, std::string());
        usb_.PlaceContent(storage_);
        MarkRestartPending();
        PublishEvent(std::string(KindLabel(file)) + " removed.");
        res.set_content("removed", kTextType);
    });
}

void TransferServer::RegisterModRoutes()
{
    httplib::Server &server = *server_;

    server.Put("/api/mods/:name", [this](const httplib::Request &req, httplib::Response &res, const httplib::ContentReader &reader) {
        const std::string name = req.path_params.at("name");
        if (!Storage::IsAllowedModName(name))
            return Fail(res, 400, "Mods are uploaded as .zip files with plain names.");

        std::uint64_t expected = 0;
        if (!ContentLength(req, &expected) || expected == 0)
            return Fail(res, 411, "The upload must state its size (Content-Length).");
        if (expected > Storage::kMaxModBytes)
            return Fail(res, 413, "That is larger than any mod; refusing " + HumanBytes(expected) + ".");

        Target target;
        if (!ParseTarget(req.get_param_value("drive"), true, GameFile::Rom, storage_, usb_, &target, res))
            return;
        const std::uint64_t needed = expected + Storage::kSafetyMarginBytes;
        if (needed > target.free)
            return Fail(res, 507, "Not enough space: this mod needs " + HumanBytes(needed) + ", " + HumanBytes(target.free) + " is free.");
        const bool staged = target.usb && !target.direct;
        if (staged && needed > storage_.FreeBytes())
            return Fail(res, 507, "The upload goes to the TV first (root moves it to the drive), and the TV is too full.");

        if (busy_.exchange(true))
            return Fail(res, 409, "Another upload or move is already running.");
        BusyGuard busyGuard(busy_);

        std::error_code ec;
        if (target.usb) {
            const std::string folderError = usb_.CreateDriveFolder(target.drive.mountPoint, UsbAccess::kDriveModsFolder);
            if (!folderError.empty())
                return Fail(res, 500, folderError);
        }
        fs::create_directories(staged ? storage_.IncomingDir() : target.folder, ec);

        SetProgress(true, name, 0, expected);
        const std::string partPath = (staged ? storage_.IncomingDir() : target.folder) + "/" + name + ".part";
        const std::int64_t written = StreamBodyToFile(reader, partPath, expected, [this, &name, expected](std::uint64_t total) {
            SetProgress(true, name, total, expected);
        });
        SetProgress(false, name, written < 0 ? 0 : static_cast<std::uint64_t>(written), expected);
        if (written < 0 || static_cast<std::uint64_t>(written) != expected) {
            usb_.RemoveFile(partPath);
            return Fail(res, 400, "The upload was interrupted or could not be written.");
        }
        if (!Storage::ZipHoldsMod(partPath)) {
            usb_.RemoveFile(partPath);
            return Fail(res, 415, name + " is not a Sonic 3 A.I.R. mod (no mod.json inside the zip).");
        }

        const std::string destination = target.folder + "/" + name;
        const bool placed = staged ? usb_.MoveWithRoot(partPath, destination).empty() : usb_.RenameFile(partPath, destination);
        if (!placed) {
            usb_.RemoveFile(partPath);
            return Fail(res, 500, "Could not move the mod into place.");
        }
        // A drive's mods folder that did not exist at launch is linked now.
        if (target.usb)
            usb_.PlaceContent(storage_);
        PublishEvent(name + " added" + (target.usb ? " to the USB drive" : "") + ". Turn it on in the game's Mods menu.");
        res.set_content("installed", kTextType);
    });

    server.Delete("/api/mods/:name", [this](const httplib::Request &req, httplib::Response &res) {
        const std::string name  = req.path_params.at("name");
        const std::string drive = req.get_param_value("drive");
        if (name.empty() || name.front() == '.' || name.find('/') != std::string::npos || name.find('\\') != std::string::npos)
            return Fail(res, 400, "That mod name is not allowed.");

        std::string folder;
        if (drive.empty() || drive == kInternal) {
            folder = storage_.ModsDir();
        }
        else {
            if (!usb_.IsUsbMount(drive))
                return Fail(res, 400, "That USB drive is not connected.");
            folder = DriveFolder(drive, true);
        }
        const std::string path = folder + "/" + name;
        // Zip mods are files; anything else in a mods folder is a mod folder.
        const bool removed = Storage::IsAllowedModName(name) ? usb_.RemoveFile(path) : usb_.RemoveFolder(path);
        if (!removed)
            return Fail(res, 500, "Could not remove " + name + ".");
        MarkRestartPending();
        PublishEvent(name + " removed.");
        res.set_content("removed", kTextType);
    });

    server.Post("/api/mods/:name/move", [this](const httplib::Request &req, httplib::Response &res) {
        const std::string name = req.path_params.at("name");
        if (!Storage::IsAllowedModName(name))
            return Fail(res, 400, "Only zip mods can be moved.");
        const std::string from = req.get_param_value("from");
        std::string sourcePath;
        if (from.empty() || from == kInternal) {
            sourcePath = storage_.ModsDir() + "/" + name;
        }
        else {
            if (!usb_.IsUsbMount(from))
                return Fail(res, 400, "That USB drive is not connected.");
            sourcePath = DriveFolder(from, true) + "/" + name;
        }
        UsbDrive sourceDrive;
        const bool sourceReadable = from.empty() || from == kInternal || (usb_.FindDrive(from, &sourceDrive) && sourceDrive.readable);
        std::uint64_t size = Storage::FileSize(sourcePath);
        if (size == 0 && !sourceReadable) {
            for (const ModEntry &mod : sourceDrive.mods) {
                if (mod.name == name)
                    size = mod.size;
            }
        }
        if (size == 0)
            return Fail(res, 404, name + " is not there.");

        Target target;
        if (!ParseTarget(req.get_param_value("drive"), true, GameFile::Rom, storage_, usb_, &target, res))
            return;
        if (fs::path(target.folder) == fs::path(sourcePath).parent_path())
            return Fail(res, 400, "It is already there.");
        if (size + Storage::kSafetyMarginBytes > target.free)
            return Fail(res, 507, "Not enough space there for " + name + ".");

        if (busy_.exchange(true))
            return Fail(res, 409, "Another upload or move is already running.");
        BusyGuard busyGuard(busy_);

        if (target.usb) {
            const std::string folderError = usb_.CreateDriveFolder(target.drive.mountPoint, UsbAccess::kDriveModsFolder);
            if (!folderError.empty())
                return Fail(res, 500, folderError);
        }
        std::error_code ec;
        fs::create_directories(target.folder, ec);
        const std::string destination = target.folder + "/" + name;
        SetProgress(true, name, 0, size);
        const std::string copyError = usb_.CopyFile(sourcePath, destination + ".part", [this, &name, size](std::uint64_t total) {
            SetProgress(true, name, total, size);
        });
        SetProgress(false, name, size, size);
        if (!copyError.empty() || !usb_.RenameFile(destination + ".part", destination)) {
            usb_.RemoveFile(destination + ".part");
            return Fail(res, 500, (copyError.empty() ? std::string("Could not move the copy into place.") : copyError)
                                      + " The original is untouched.");
        }
        usb_.RemoveFile(sourcePath);
        if (target.usb)
            usb_.PlaceContent(storage_);
        MarkRestartPending();
        PublishEvent(name + " moved to " + (target.usb ? "the USB drive" : "the TV") + ". Turn it on again in the Mods menu.");
        res.set_content("moved", kTextType);
    });
}

void TransferServer::RegisterSaveRoutes()
{
    httplib::Server &server = *server_;

    server.Get("/api/saves", [this](const httplib::Request &, httplib::Response &res) {
        std::ostringstream json;
        json << "{\"saves\":[";
        bool first = true;
        for (const SaveFile &save : storage_.ListSaves()) {
            json << (first ? "" : ",") << "{\"path\":\"" << JsonEscape(save.path) << "\",\"size\":" << save.size
                 << ",\"mtime\":" << save.mtime << "}";
            first = false;
        }
        json << "],\"pendingRestore\":" << JsonBool(Storage::FileSize(storage_.PendingBackupPath()) > 0) << "}";
        res.set_content(json.str(), kJsonType);
    });

    server.Get("/api/saves/backup", [this](const httplib::Request &, httplib::Response &res) {
        // Each download gets its own file, removed once it has been sent.
        static std::atomic<unsigned> counter{ 0 };
        const std::string path = storage_.PathFor("backup-download-" + std::to_string(++counter) + ".zip");
        const std::string error = SaveBackup::Create(storage_.AppDataDir(), path);
        const std::uint64_t size = Storage::FileSize(path);
        if (!error.empty() || size == 0) {
            std::error_code ec;
            fs::remove(path, ec);
            return Fail(res, 500, error.empty() ? std::string("There are no saves yet.") : error);
        }
        auto in = std::make_shared<std::ifstream>(path, std::ios::binary);
        res.set_header("Content-Disposition", "attachment; filename=\"sonic3air-saves.zip\"");
        res.set_header("Cache-Control", "no-store");
        res.set_content_provider(
            static_cast<std::size_t>(size), "application/zip",
            [in](std::size_t offset, std::size_t length, httplib::DataSink &sink) {
                std::vector<char> buffer(std::min<std::size_t>(length, 64 * 1024));
                in->seekg(static_cast<std::streamoff>(offset));
                in->read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                const std::streamsize got = in->gcount();
                return got > 0 && sink.write(buffer.data(), static_cast<std::size_t>(got));
            },
            [in, path](bool) {
                in->close();
                std::error_code ec;
                fs::remove(path, ec);
            });
    });

    server.Put("/api/saves/backup", [this](const httplib::Request &req, httplib::Response &res, const httplib::ContentReader &reader) {
        std::uint64_t expected = 0;
        if (!ContentLength(req, &expected) || expected == 0)
            return Fail(res, 411, "The upload must state its size (Content-Length).");
        if (expected > Storage::kMaxBackupBytes)
            return Fail(res, 413, "That is larger than any save backup; refusing " + HumanBytes(expected) + ".");
        if (expected + Storage::kSafetyMarginBytes > storage_.FreeBytes())
            return Fail(res, 507, "Not enough space on the TV.");

        std::error_code ec;
        fs::create_directories(storage_.IncomingDir(), ec);
        const std::string partPath = storage_.IncomingDir() + "/backup-upload.part";
        const std::int64_t written = StreamBodyToFile(reader, partPath, expected, [](std::uint64_t) {});
        if (written < 0 || static_cast<std::uint64_t>(written) != expected) {
            fs::remove(partPath, ec);
            return Fail(res, 400, "The upload was interrupted.");
        }
        const std::string invalid = SaveBackup::Validate(partPath);
        if (!invalid.empty()) {
            fs::remove(partPath, ec);
            return Fail(res, 415, invalid);
        }

        if (setupMode_.load()) {
            // The game has not started: restore straight away.
            const std::string error = SaveBackup::Restore(partPath, storage_.AppDataDir());
            fs::remove(partPath, ec);
            if (!error.empty())
                return Fail(res, 500, error);
            PublishEvent("Saves restored.");
            return res.set_content("installed", kTextType);
        }
        // While playing, the game owns its saves (and would write over them): restore at the next start.
        fs::rename(partPath, storage_.PendingBackupPath(), ec);
        if (ec) {
            fs::remove(partPath, ec);
            return Fail(res, 500, "Could not keep the backup for the next start.");
        }
        MarkRestartPending();
        PublishEvent("Save backup received. It is restored when the game starts again.");
        res.set_content("staged", kTextType);
    });
}

} // namespace s3w
