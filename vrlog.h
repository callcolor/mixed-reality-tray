/* vrlog.h -- one place every diagnostic line goes.
 *
 * vrlog() writes to stderr (so a terminal run looks as it always did) and to a
 * timestamped log file under $XDG_STATE_HOME/vrmirror/, so a user who launched
 * from a desktop icon still has one file to send in a bug report. Both streams
 * are flushed per line: the lease/present sequence is the interesting part and
 * it must survive a crash, and stdout/stderr interleaving must stay truthful.
 */
#ifndef VRLOG_H
#define VRLOG_H

/* Open the log for `app` ("vrmirror-x11"/"vrmirror-wl") and write the header
 * block (version, distro, kernel, session, GPUs, VRMIRROR_* overrides). Safe to
 * skip: if the file cannot be opened, vrlog() still writes to stderr. */
void vrlog_open(const char *app);

/* Log one line. A trailing newline is added if the format lacks one. */
void vrlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Absolute path of the log file, or NULL if none was opened. */
const char *vrlog_path(void);

void vrlog_close(void);

#endif
