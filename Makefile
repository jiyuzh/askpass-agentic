PREFIX  ?= $(HOME)/.local
CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Wformat=2 -D_FORTIFY_SOURCE=2 -fstack-protector-strong -fPIE
LDFLAGS ?= -pie -Wl,-z,relro,-z,now

all: askpass-agentic askpass-confirm

askpass-agentic: askpass-agentic.c askpass_common.h
	$(CC) $(CFLAGS) -o $@ askpass-agentic.c $(LDFLAGS)

askpass-confirm: askpass-confirm.c askpass_common.h
	$(CC) $(CFLAGS) -o $@ askpass-confirm.c $(LDFLAGS)

# Per-user install: no sudo needed. Run as your normal user.
install: all
	install -d -m 755 $(PREFIX)/bin
	install -m 755 askpass-agentic askpass-confirm $(PREFIX)/bin/

uninstall:
	rm -f $(PREFIX)/bin/askpass-agentic $(PREFIX)/bin/askpass-confirm

clean:
	rm -f askpass-agentic askpass-confirm

.PHONY: all install uninstall clean
