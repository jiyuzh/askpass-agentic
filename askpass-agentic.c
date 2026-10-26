/*
* askpass — SUDO_ASKPASS helper with out-of-band approval.
*
* When sudo -A needs a password it runs this program. We:
*   1. read the exact command line from our parent (sudo) via /proc,
*   2. create a one-shot request (random 9-digit ID) in ~/.claude-sudo,
*   3. broadcast a notice to every terminal you own (like wall),
*   4. wait for `askpass-confirm --request ID` to deliver the password,
*   5. print it to stdout for sudo, or exit 1 on deny/timeout.
*
* Fails closed: any error, an empty answer, a timeout, or the kill switch
* (~/.claude-sudo/disabled) makes sudo fail.
*/
#include "askpass_common.h"

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/random.h>
#include <time.h>

#define TIMEOUT_SEC 120

static char fifo_path[PATH_MAX];
static char cmd_path[PATH_MAX];
static volatile sig_atomic_t got_signal;

static void on_signal(int sig) { (void)sig; got_signal = 1; }

static void cleanup(void)
{
	if (fifo_path[0]) unlink(fifo_path);
	if (cmd_path[0])  unlink(cmd_path);
}

static void fail(const char *msg)
{
	fprintf(stderr, "askpass: %s\n", msg);
	cleanup();
	exit(1);
}

/* ---- command-line rendering ------------------------------------------ */

static void put(char *buf, size_t *len, const char *s, size_t n)
{
	if (*len + n >= MAX_CMD - 16) {           /* leave room for marker */
		if (*len < MAX_CMD - 16) {
			memcpy(buf + *len, " [truncated]", 12);
			*len += 12;
		}
		*len = MAX_CMD;                        /* mark as full */
		return;
	}
	memcpy(buf + *len, s, n);
	*len += n;
}

/* Render one byte safely: escape anything that is not printable ASCII so a
* crafted command cannot inject terminal escape sequences into the notice. */
static void put_byte(char *buf, size_t *len, unsigned char c)
{
	char tmp[5];
	if (c >= 0x20 && c < 0x7f) {
		tmp[0] = (char)c;
		put(buf, len, tmp, 1);
	} else {
		snprintf(tmp, sizeof tmp, "\\x%02x", c);
		put(buf, len, tmp, 4);
	}
}

static int needs_quote(const char *a, size_t n)
{
	if (n == 0) return 1;
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)a[i];
		if (!(isalnum(c) || strchr("_@%+=:,./-", c)))
			return 1;
	}
	return 0;
}

/* Turn sudo's NUL-separated argv into a shell-quoted, sanitized string. */
static void describe_parent(char *out)
{
	char raw[65536], path[64];
	size_t len = 0;
	ssize_t n;
	int fd;

	snprintf(path, sizeof path, "/proc/%d/cmdline", (int)getppid());
	fd = open(path, O_RDONLY | O_CLOEXEC);
	n = fd >= 0 ? read(fd, raw, sizeof raw - 1) : -1;
	if (fd >= 0) close(fd);
	if (n <= 0) {
		strcpy(out, "(unknown command)");
		return;
	}
	raw[n] = '\0';

	for (ssize_t i = 0; i < n && len < MAX_CMD; ) {
		const char *arg = raw + i;
		size_t alen = strnlen(arg, (size_t)(n - i));

		if (i > 0) put(out, &len, " ", 1);
		if (needs_quote(arg, alen)) {
			put(out, &len, "'", 1);
			for (size_t j = 0; j < alen && len < MAX_CMD; j++) {
				if (arg[j] == '\'') put(out, &len, "'\\''", 4);
				else put_byte(out, &len, (unsigned char)arg[j]);
			}
			put(out, &len, "'", 1);
		} else {
			for (size_t j = 0; j < alen; j++)
				put_byte(out, &len, (unsigned char)arg[j]);
		}
		i += (ssize_t)alen + 1;
	}
	if (len > MAX_CMD - 1) len = MAX_CMD - 1;
	out[len] = '\0';
}

/* ---- broadcast --------------------------------------------------------- */

#define MAX_TTYS 256

static int notify_tty(const char *path, const char *msg, size_t mlen,
					dev_t *seen, int *nseen)
{
	struct stat st;
	int fd;

	if (stat(path, &st) != 0 || !S_ISCHR(st.st_mode) || st.st_uid != getuid())
		return 0;
	for (int i = 0; i < *nseen; i++)
		if (seen[i] == st.st_rdev)
			return 0;
	if (*nseen < MAX_TTYS)
		seen[(*nseen)++] = st.st_rdev;

	fd = open(path, O_WRONLY | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return 0;
	ssize_t w = write(fd, msg, mlen);
	close(fd);
	return w > 0;
}

static int all_digits(const char *s)
{
	if (!*s) return 0;
	for (; *s; s++) if (!isdigit((unsigned char)*s)) return 0;
	return 1;
}

/* Write to every pseudo-terminal (/dev/pts/N: ssh sessions, tmux panes,
* VS Code terminals) and virtual console (/dev/ttyN) owned by this user.
* Your own ttys are owner-writable, so no setgid-tty is needed. */
static int broadcast(const char *msg)
{
	dev_t seen[MAX_TTYS];
	int nseen = 0, count = 0;
	size_t mlen = strlen(msg);
	char path[PATH_MAX];
	struct dirent *de;
	DIR *d;

	if ((d = opendir("/dev/pts"))) {
		while ((de = readdir(d)))
			if (all_digits(de->d_name)) {
				snprintf(path, sizeof path, "/dev/pts/%s", de->d_name);
				count += notify_tty(path, msg, mlen, seen, &nseen);
			}
		closedir(d);
	}
	if ((d = opendir("/dev"))) {
		while ((de = readdir(d)))
			if (!strncmp(de->d_name, "tty", 3) && all_digits(de->d_name + 3)) {
				snprintf(path, sizeof path, "/dev/%s", de->d_name);
				count += notify_tty(path, msg, mlen, seen, &nseen);
			}
		closedir(d);
	}
	return count;
}

/* ---- main --------------------------------------------------------------- */

static long ms_left(const struct timespec *deadline)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (deadline->tv_sec - now.tv_sec) * 1000L +
		(deadline->tv_nsec - now.tv_nsec) / 1000000L;
}

int main(void)
{
	char dir[DIR_MAX], path[PATH_MAX], id[ID_LEN + 1];
	char cmd[MAX_CMD], msg[MAX_CMD + 512], pw[MAX_PW + 2];
	struct sigaction sa;
	struct timespec deadline;
	size_t pwlen = 0;
	int fd, ok = 0;

	umask(077);
	signal(SIGPIPE, SIG_IGN);
	memset(&sa, 0, sizeof sa);
	sa.sa_handler = on_signal;               /* no SA_RESTART: poll gets EINTR */
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);

	if (get_state_dir(dir, sizeof dir, 1) != 0)
		fail("state directory ~/" STATE_SUBDIR " is missing or unsafe");

	snprintf(path, sizeof path, "%s/disabled", dir);
	if (access(path, F_OK) == 0)
		fail("sudo approvals are disabled (askpass-confirm --enable to re-enable)");

	describe_parent(cmd);

	/* Create the request: <id>.cmd first, then <id>.fifo (marks it pending). */
	for (int tries = 0; tries < 10 && !ok; tries++) {
		uint32_t r;
		if (getrandom(&r, sizeof r, 0) != sizeof r)
			fail("getrandom failed");
		snprintf(id, sizeof id, "%09u", r % 1000000000u);

		snprintf(cmd_path, sizeof cmd_path, "%s/%s.cmd", dir, id);
		fd = open(cmd_path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
		if (fd < 0) { cmd_path[0] = '\0'; continue; }
		if (write(fd, cmd, strlen(cmd)) < 0) { close(fd); fail("write failed"); }
		close(fd);

		snprintf(fifo_path, sizeof fifo_path, "%s/%s.fifo", dir, id);
		if (mkfifo(fifo_path, 0600) != 0) {
			fifo_path[0] = '\0';
			unlink(cmd_path);
			cmd_path[0] = '\0';
			continue;
		}
		ok = 1;
	}
	if (!ok)
		fail("could not create request");

	/* O_RDWR keeps a writer reference on our side (Linux semantics), so poll
	* waits for data instead of reporting hang-up before confirm connects. */
	fd = open(fifo_path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		fail("could not open request fifo");

	snprintf(msg, sizeof msg,
			"\r\n\a"
			"A coding agent is requesting sudo permission to execute:\r\n"
			"> %s\r\n"
			"Run askpass-confirm --request %s to approve the request.\r\n",
			cmd, id);
	if (broadcast(msg) == 0)
		fprintf(stderr, "askpass: no terminals found to notify; "
						"the user can run askpass-confirm --list\n");
	fprintf(stderr, "askpass: waiting for approval of request %s (%ds)\n",
			id, TIMEOUT_SEC);

	clock_gettime(CLOCK_MONOTONIC, &deadline);
	deadline.tv_sec += TIMEOUT_SEC;

	for (;;) {
		struct pollfd p = { .fd = fd, .events = POLLIN };
		long left = ms_left(&deadline);
		int pr;

		if (got_signal) { close(fd); fail("interrupted"); }
		if (left <= 0)  { close(fd); fail("timed out waiting for approval"); }

		pr = poll(&p, 1, (int)left);
		if (pr < 0 && errno == EINTR) continue;
		if (pr < 0) { close(fd); fail("poll failed"); }
		if (pr == 0) continue;

		ssize_t n = read(fd, pw + pwlen, sizeof pw - 1 - pwlen);
		if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
		if (n <= 0) { close(fd); fail("read failed"); }
		pwlen += (size_t)n;

		char *nl = memchr(pw, '\n', pwlen);
		if (nl) { *nl = '\0'; pwlen = (size_t)(nl - pw); break; }
		if (pwlen >= sizeof pw - 1) {
			explicit_bzero(pw, sizeof pw);
			close(fd);
			fail("response too long");
		}
	}
	close(fd);
	cleanup();

	if (pwlen == 0) {
		fprintf(stderr, "askpass: request %s denied\n", id);
		return 1;
	}
	pw[pwlen++] = '\n';
	ssize_t w = write(STDOUT_FILENO, pw, pwlen);
	explicit_bzero(pw, sizeof pw);
	return w == (ssize_t)pwlen ? 0 : 1;
}
