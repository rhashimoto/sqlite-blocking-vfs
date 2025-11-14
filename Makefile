SHIM_NAME=vfsshim
SHIM_CHATTY=0
TARGET_LIB=${SHIM_NAME}.so

CFLAGS= -DSHIM_NAME=${SHIM_NAME} -DSHIM_CHATTY=${SHIM_CHATTY} -O3 -fPIC
LDFLAGS= -shared -Wl,-soname=${TARGET_LIB}

default: ${TARGET_LIB}

clean:
	rm -f vfsshim.o ${TARGET_LIB}

${TARGET_LIB}: vfsshim.o
	$(CC) ${LDFLAGS} -o $@ $<
