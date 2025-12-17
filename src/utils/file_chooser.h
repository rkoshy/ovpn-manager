#ifndef FILE_CHOOSER_H
#define FILE_CHOOSER_H

/**
 * File Chooser Utilities
 *
 * GTK-based file selection dialogs
 */

/**
 * Show a file chooser dialog for selecting an OVPN file
 *
 * @param title Dialog title
 * @return Selected file path (must be freed with g_free), or NULL if cancelled
 */
char* file_chooser_select_ovpn(const char *title);

/**
 * Read entire file contents into a string
 *
 * @param file_path Path to file
 * @param contents Output: file contents (must be freed with g_free)
 * @param error Output: error message if failed (must be freed with g_free)
 * @return 0 on success, negative on error
 */
int file_read_contents(const char *file_path, char **contents, char **error);

#endif /* FILE_CHOOSER_H */
