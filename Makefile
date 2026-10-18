CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra
EXE :=
ifeq ($(OS),Windows_NT)
EXE := .exe
LDLIBS := -lws2_32 -lshell32
LDFLAGS += -mwindows
else
LDLIBS := -pthread
endif

all: YokiCore$(EXE)

tools/embed$(EXE): tools/embed.c
	$(CC) $(CFLAGS) -o $@ $<

src/ui.h: src/ui.html tools/embed$(EXE)
	./tools/embed$(EXE) src/ui.html src/ui.h

YokiCore$(EXE): src/main.c src/ui.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ src/main.c $(LDLIBS)

test: YokiCore$(EXE)
	sh tests/run.sh ./YokiCore$(EXE)

clean:
	rm -f YokiCore YokiCore.exe tools/embed tools/embed.exe

.PHONY: all test clean
