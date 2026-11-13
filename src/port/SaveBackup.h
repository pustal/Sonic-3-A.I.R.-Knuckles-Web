/**
 * @file SaveBackup.h
 * @brief The saves as one zip file, for the transfer page's backup and restore.
 *
 * A backup holds, relative to the game's app data folder:
 *   settings.json, settings_input.json   options and controls
 *   storage/                             save slots, achievements, unlocked secrets
 *   savestates/                          save states
 *   playerprogress.bin                   progress from older A.I.R. versions
 *   mods/active-mods.json                which mods are on
 * It never holds the ROM, the soundtrack or the mods themselves, so it is small
 * and can be shared. A restore only writes these same paths.
 */
#ifndef S3W_SAVE_BACKUP_H
#define S3W_SAVE_BACKUP_H

#include <string>
#include <vector>

namespace s3w
{
namespace SaveBackup
{

/** The save files present in @p appDataDir, as paths relative to it. */
std::vector<std::string> CollectFiles(const std::string &appDataDir);

/** Whether @p relative (a path inside a backup) is one a restore may write. */
bool IsAllowedPath(const std::string &relative);

/**
 * Writes the saves in @p appDataDir to the zip file @p zipPath.
 * @return Empty string on success, otherwise the reason.
 */
std::string Create(const std::string &appDataDir, const std::string &zipPath);

/**
 * Checks that @p zipPath is a backup: a zip whose entries are all allowed
 * paths, with at least one of them.
 * @return Empty string if it is, otherwise the reason.
 */
std::string Validate(const std::string &zipPath);

/**
 * Restores the backup @p zipPath into @p appDataDir (it overwrites the files
 * it holds and leaves the others).
 * @return Empty string on success, otherwise the reason.
 */
std::string Restore(const std::string &zipPath, const std::string &appDataDir);

} // namespace SaveBackup
} // namespace s3w

#endif // S3W_SAVE_BACKUP_H
