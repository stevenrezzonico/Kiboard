/* Bluetooth transport: starts the helper kiboard-bt (through bt.sh, which prepares the Kindle's
 * Bluetooth stack) and talks to it over two pipes. Reports go in as: length byte, report.
 * Status comes back as lines: "adv", "connected", "disconnected", "fail". */
#include "bt.h"

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int to = -1, from = -1, state = BT_STARTING;
static pid_t pid;
static char script[300];

int bt_open(const char *dir)
{
	int in[2], out[2];
	snprintf(script, sizeof script, "%s/bin/bt.sh", dir);
	signal(SIGPIPE, SIG_IGN); /* a dead helper must not kill the app */
	if (pipe(in) || pipe(out) || (pid = fork()) < 0) { perror("bt: start"); return -1; }
	if (pid == 0) {
		dup2(in[0], 0); dup2(out[1], 1);
		close(in[0]); close(in[1]); close(out[0]); close(out[1]);
		execl("/bin/sh", "sh", script, "run", (char *) NULL);
		_exit(127);
	}
	close(in[0]); close(out[1]);
	to = in[1]; from = out[0];
	fcntl(from, F_SETFL, O_NONBLOCK);
	fcntl(to, F_SETFD, FD_CLOEXEC); fcntl(from, F_SETFD, FD_CLOEXEC);
	state = BT_STARTING;
	return from;
}

int bt_event(void)
{
	static char line[64];
	static int n;
	char c;
	ssize_t got;
	while ((got = read(from, &c, 1)) == 1) {
		if (c != '\n') { if (n < (int) sizeof line - 1) line[n++] = c; continue; }
		line[n] = 0; n = 0;
		fprintf(stderr, "bt: helper says \"%s\"\n", line);
		if (!strcmp(line, "connected")) state = BT_CONNECTED;
		else if (!strcmp(line, "adv") || !strcmp(line, "disconnected")) state = BT_WAITING;
		else if (!strcmp(line, "fail")) state = BT_FAILED;
	}
	if (got == 0) state = BT_FAILED; /* the helper is gone */
	return state;
}

void bt_send(const unsigned char *report, int len)
{
	unsigned char buf[17];
	if (state != BT_CONNECTED || len > 16) return;
	buf[0] = (unsigned char) len;
	memcpy(buf + 1, report, len);
	if (write(to, buf, len + 1) < 0) perror("bt: send");
}

void bt_close(void)
{
	char cmd[340];
	if (!script[0]) return;
	/* bt.sh off stops the Kindle's Bluetooth service first and the helper second: see the note there. */
	snprintf(cmd, sizeof cmd, "sh %s off", script);
	if (system(cmd)) fprintf(stderr, "bt: bt.sh off failed\n");
	if (to >= 0) close(to);
	if (from >= 0) close(from);
	to = from = -1;
	kill(pid, SIGKILL); /* the shell, if it is still preparing */
	waitpid(pid, NULL, 0);
	script[0] = 0;
}
