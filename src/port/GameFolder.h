/**
 * @file GameFolder.h
 * @brief The game's working directory, made of the game data in the app and
 *        a writable saves/ folder.
 *
 * Sonic 3 A.I.R. reads its data (data/, scripts/, config.json and the game
 * profile oxygenproject.json) relative to its
 * working directory and caches the compiled scripts in saves/scripts.bin
 * there. The app's own folder is read-only on the TV, so the working directory
 * is <data folder>/game, with links to the game data shipped in the app
 * (<app>/game) and a real saves/ folder. The links are made again at every
 * launch, so they always point at the installed version.
 */
#ifndef S3W_GAME_FOLDER_H
#define S3W_GAME_FOLDER_H

#include <string>

namespace s3w
{

class Storage;

namespace GameFolder
{

/** The folder the executable is in (no trailing slash). */
std::string InstallDir();

/** The game data shipped with the app: $S3AIR_GAME_DATA, else <app>/game. */
std::string GameDataDir();

/**
 * Builds <data folder>/game (see the file comment).
 * @return Empty string on success, otherwise the reason.
 */
std::string Prepare(const Storage &storage);

} // namespace GameFolder
} // namespace s3w

#endif // S3W_GAME_FOLDER_H
