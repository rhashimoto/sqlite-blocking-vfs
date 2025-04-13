SHIM_NAME=vfsshim
TARGET_LIB=${SHIM_NAME}.so

CFLAGS= -DSHIM_NAME=${SHIM_NAME} -O3 -fPIC
LDFLAGS= -shared -Wl,-soname=${TARGET_LIB}

default: ${TARGET_LIB}

clean:
	rm -f vfsshim.o ${TARGET_LIB}

${TARGET_LIB}: vfsshim.o
	$(CC) ${LDFLAGS} -o $@ $<
