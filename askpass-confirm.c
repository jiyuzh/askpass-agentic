/*
* askpass-confirm — approve or deny a pending askpass request.
*
*   askpass-confirm --request ID     show the command, prompt for your sudo
*                                    password, and hand it to the waiting sudo
*   askpass-confirm --request ID --deny
*   askpass-confirm --list           show pending requests
*   askpass-confirm --disable        kill switch: deny all requests instantly
*   askpass-confirm --enable         remove the kill switch
*
* Approval requires a controlling terminal (/dev/tty), so processes without
* one (such as commands run by Claude) cannot approve.
*/
#include "askpass_common.h"

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <termios.h>

static int tty_fd = -1;
static struct termios saved_tio;
static int tio_saved;

static void restore_tty(void)
{
	if (tio_saved)
		tcsetattr(tty_fd, TCSAFLUSH, &saved_tio);
}

static void on_signal(int sig)
{
	restore_tty();
	if (tty_fd >= 0) (void)!write(tty_fd, "\n", 1);
	_exit(128 + sig);
}

static void usage(void)
{
	fprintf(stderr,
		"usage: askpass-confirm --request ID [--deny]\n"
		"       askpass-confirm --list | --disable | --enable\n");
	exit(2);
}

static void read_cmd(const char *dir, const char *id, char *out, size_t n)
{
	char path[PATH_MAX];
	ssize_t r;
	int fd;

	snprintf(path, sizeof path, "%s/%s.cmd", dir, id);
	fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	r = fd >= 0 ? read(fd, out, n - 1) : -1;
	if (fd >= 0) close(fd);
	out[r > 0 ? r : 0] = '\0';
	if (r <= 0) snprintf(out, n, "(unknown command)");
}

static int list_requests(const char *dir)
{
	struct dirent *de;
	int count = 0;
	DIR *d = opendir(dir);

	if (!d) { perror("askpass-confirm"); return 1; }
	while ((de = readdir(d))) {
		char id[ID_LEN + 1], cmd[MAX_CMD];
		const char *dot = strchr(de->d_name, '.');
		if (!dot || strcmp(dot, ".fifo") || dot - de->d_name != ID_LEN)
			continue;
		memcpy(id, de->d_name, ID_LEN);
		id[ID_LEN] = '\0';
		if (!valid_id(id)) continue;
		read_cmd(dir, id, cmd, sizeof cmd);
		printf("%s  > %s\n", id, cmd);
		count++;
	}
	closedir(d);
	if (!count) printf("No pending requests.\n");
	return 0;
}

static int read_password(char *pw, size_t n)
{
	struct termios tio;
	struct sigaction sa;
	size_t len = 0;
	const char *prompt = "Your sudo password (empty to deny): ";

	tty_fd = open("/dev/tty", O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (tty_fd < 0 || !isatty(tty_fd)) {
		fprintf(stderr, "askpass-confirm: must be run from an interactive terminal\n");
		return -1;
	}

	memset(&sa, 0, sizeof sa);
	sa.sa_handler = on_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	sigaction(SIGTSTP, &sa, NULL);

	if (tcgetattr(tty_fd, &saved_tio) != 0) return -1;
	tio = saved_tio;
	tio.c_lflag &= ~(tcflag_t)(ECHO | ECHONL);
	tio.c_lflag |= ICANON;
	tio_saved = 1;
	if (tcsetattr(tty_fd, TCSAFLUSH, &tio) != 0) return -1;

	(void)!write(tty_fd, prompt, strlen(prompt));
	for (;;) {
		char c;
		ssize_t r = read(tty_fd, &c, 1);
		if (r < 0 && errno == EINTR) continue;
		if (r <= 0 || c == '\n') break;
		if (len < n - 1) pw[len++] = c;
	}
	pw[len] = '\0';
	restore_tty();
	(void)!write(tty_fd, "\n", 1);
	return (int)len;
}

int main(int argc, char **argv)
{
	char dir[DIR_MAX], path[PATH_MAX], cmd[MAX_CMD], pw[MAX_PW + 2];
	const char *id = NULL;
	int deny = 0, list = 0, disable = 0, enable = 0;
	struct stat st;
	int fd, len;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--request") && i + 1 < argc) id = argv[++i];
		else if (!strcmp(argv[i], "--deny"))    deny = 1;
		else if (!strcmp(argv[i], "--list"))    list = 1;
		else if (!strcmp(argv[i], "--disable")) disable = 1;
		else if (!strcmp(argv[i], "--enable"))  enable = 1;
		else usage();
	}
	if ((id != NULL) + list + disable + enable != 1) usage();

	umask(077);
	if (get_state_dir(dir, sizeof dir, 1) != 0) {
		fprintf(stderr, "askpass-confirm: ~/" STATE_SUBDIR " is missing or unsafe\n");
		return 1;
	}

	if (list) return list_requests(dir);

	snprintf(path, sizeof path, "%s/disabled", dir);
	if (disable) {
		fd = open(path, O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
		if (fd < 0) { perror("askpass-confirm"); return 1; }
		close(fd);
		printf("sudo approvals disabled: all requests will be denied.\n");
		return 0;
	}
	if (enable) {
		if (unlink(path) != 0 && errno != ENOENT) { perror("askpass-confirm"); return 1; }
		printf("sudo approvals enabled.\n");
		return 0;
	}

	if (!valid_id(id)) {
		fprintf(stderr, "askpass-confirm: request ID must be %d digits\n", ID_LEN);
		return 1;
	}

	snprintf(path, sizeof path, "%s/%s.fifo", dir, id);
	if (lstat(path, &st) != 0 || !S_ISFIFO(st.st_mode) || st.st_uid != getuid()) {
		fprintf(stderr, "askpass-confirm: no pending request %s (expired?)\n", id);
		return 1;
	}

	read_cmd(dir, id, cmd, sizeof cmd);
	printf("Request %s wants to run:\n> %s\n", id, cmd);
	fflush(stdout);

	if (deny) {
		pw[0] = '\0';
		len = 0;
	} else {
		len = read_password(pw, MAX_PW + 1);
		if (len < 0) return 1;
	}

	fd = open(path, O_WRONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0) {
		explicit_bzero(pw, sizeof pw);
		fprintf(stderr, "askpass-confirm: request %s is no longer waiting\n", id);
		return 1;
	}
	pw[len] = '\n';
	ssize_t w = write(fd, pw, (size_t)len + 1);
	explicit_bzero(pw, sizeof pw);
	close(fd);

	if (w != len + 1) {
		fprintf(stderr, "askpass-confirm: failed to deliver response\n");
		return 1;
	}
	printf(len ? "Approved (sudo will verify the password).\n" : "Denied.\n");
	return 0;
}
