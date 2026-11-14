/**
 * @file PortUpdateCheck.h
 * @brief The game's "Check for updates" option, answered for this port.
 *
 * Sonic 3 A.I.R.'s own check asks Eukaryot's server about the official
 * downloads, none of which run on a TV, and "Open download page" would open a
 * browser the TV does not have. patches/0003 makes the option call the C
 * functions below instead: they look up the latest release of this project
 * on GitHub (on a thread, with the TV's libcurl), and the download page is
 * shown in the port's menu (MenuOverlay.h), with the address to open on a
 * phone and a reminder that the Homebrew Channel installs updates.
 */
#ifndef S3W_PORT_UPDATE_CHECK_H
#define S3W_PORT_UPDATE_CHECK_H

extern "C" {

/** Where a check is (s3w_update_check_poll). */
enum S3wUpdateCheckState {
    S3W_UPDATE_CHECK_IDLE    = 0, ///< Not started.
    S3W_UPDATE_CHECK_RUNNING = 1, ///< Asking GitHub.
    S3W_UPDATE_CHECK_DONE    = 2, ///< Answered.
    S3W_UPDATE_CHECK_FAILED  = 3, ///< No answer (offline, no libcurl, ...).
};

/** Starts a check, unless one is running. */
void s3w_update_check_start(void);

/** The state of the last check; @p hasUpdate receives whether a newer release exists (when DONE). */
int s3w_update_check_poll(int *hasUpdate);

/** The port's version ("1.0.0"). */
const char *s3w_update_check_current_version(void);

/** The latest release's version ("1.1.0"), or "" before a check answered. */
const char *s3w_update_check_latest_version(void);

/** Shows where to get the update, in the port's menu. */
void s3w_update_check_show_page(void);

} // extern "C"

#endif // S3W_PORT_UPDATE_CHECK_H
