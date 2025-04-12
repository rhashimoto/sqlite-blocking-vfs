SHIM_NAME=vfsshim
TARGET_LIB=${SHIM_NAME}.so

SQLITE_VERSION=3490100
SQLITE_AUTOCONF=sqlite-autoconf-${SQLITE_VERSION}
SQLITE_AUTOCONF_URL=https://sqlite.org/2025/${SQLITE_AUTOCONF}.tar.gz

CFLAGS= -DSHIM_NAME=${SHIM_NAME} -I${SQLITE_AUTOCONF} -O3 -fPIC
LDFLAGS= -shared -Wl,-soname=${TARGET_LIB}

default: ${TARGET_LIB}

${SQLITE_AUTOCONF}/sqlite3ext.h:
	curl ${SQLITE_AUTOCONF_URL} | tar xzf -
	(cd ${SQLITE_AUTOCONF}; ./configure)

vfsshim.c: ${SQLITE_AUTOCONF}/sqlite3ext.h

${TARGET_LIB}: vfsshim.o
	$(CC) ${LDFLAGS} -o $@ $<
