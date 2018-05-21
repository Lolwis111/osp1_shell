#!/ usr/bin/make
.SUFFIXES:
SRC = shell.c shell.h prog.c
TAR = shell prog
PCK = lab-2.zip

CFLAGS = -std=gnu11 -c -ggdb -O0 -Wall -Werror

%.o: %.c
	$(CC) $(CFLAGS) $^ -o $@

%: %.o
	$(CC) -o $@ $^

all: $(TAR)

run: $(TAR)
	./shell

pack:
	zip $(PCK) $(SRC) Makefile

clean:
	$(RM) $(RMFILES) $(TAR) $(PCK)
