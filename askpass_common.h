#pragma once

#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define STATE_SUBDIR  ".claude-sudo"
#define ID_LEN        9
#define MAX_PW        1024
#define MAX_CMD       4096
#define DIR_MAX       (PATH_MAX / 2)  /* leaves room for "/<id>.fifo" etc. */

/* Resolve ~/.claude-sudo from the passwd database (not $HOME) and make sure
* it is a real directory owned by us with no group/other permissions. */
static int get_state_dir(char *out, size_t n, int create)
{
	struct passwd *pw = getpwuid(getuid());
	struct stat st;

	if (!pw || !pw->pw_dir)
		return -1;
	if (snprintf(out, n, "%s/%s", pw->pw_dir, STATE_SUBDIR) >= (int)n)
		return -1;
	if (create && mkdir(out, 0700) != 0 && errno != EEXIST)
		return -1;
	if (lstat(out, &st) != 0)
		return -1;
	if (!S_ISDIR(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 077)) {
		errno = EPERM;
		return -1;
	}
	return 0;
}

__attribute__((unused)) static int valid_id(const char *s)
{
	if (strlen(s) != ID_LEN)
		return 0;
	for (; *s; s++)
		if (!isdigit((unsigned char)*s))
			return 0;
	return 1;
}
