include config.mk

SRC = ewd.c
OBJ = ${SRC:.c=.o}

all: options ewd

options:
	@echo ewd build options:
	@echo "CFLAGS	= ${CFLAGS}"
	@echo "LDFLAGS	= ${LDFLAGS}"
	@echo "CC	= ${CC}"

.c.o:
	@echo CC $<
	@${CC} -c ${CFLAGS} $<

ewd: ${OBJ}
	@echo CC -o $a
	@${CC} -o $@ ${OBJ} ${LDFLAGS}

clean:
	@echo cleaning
	@rm -f ewd ${OBJ}

install:
	@echo installing executable file to ${PREFIX}/bin
	@mkdir -p ${PREFIX}/bin
	@cp -f ewd ${PREFIX}/bin
	@chmod 755 ${PREFIX}/bin/ewd

uninstall:
	@echo removing executable file from ${PREFIX}/bin
	@rm -f ${PREFIX}/bin/ewd

.PHONY: all options clean install uninstall
