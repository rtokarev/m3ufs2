VERSION = $(shell git describe)

CFLAGS += -Wall -Wextra -Werror \
	  -fPIC -std=c99 \
	  -I. \
	  -D_FILE_OFFSET_BITS=64 -D_GNU_SOURCE -DVERSION=\"${VERSION}\"
ifeq (${DEBUG},1)
CFLAGS += -O0 -ggdb3
else
CFLAGS += -O2
endif

LDFLAGS +=
LIBS += -lfuse -lpthread

all: m3ufs2

m3ufs2: m3ufs2.o
	${CC} ${LDFLAGS} $^ -o $@ ${LIBS}

.PHONY: install clean

install: m3ufs2
	install m3ufs2 /usr/bin/m3ufs2

clean:
	rm -rf m3ufs2 m3ufs2.o
