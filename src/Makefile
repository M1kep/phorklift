CFLAGS = -g -Wall -Werror -O2
LDFLAGS = -Llibloop -Llibhttp2 -Llibwuya
LDFLAGS += -Wl,-export-dynamic # for dynamic modules
LDLIBS = -lloop -lhttp2 -lwuya -lpthread -ldl -lrt
LDLIBS += -lluajit-5.1 # use `-llua5.1` for original Lua
LDLIBS += -lssl -lcrypto
LDLIBS += -lz

SOURCES = $(wildcard *.c)
OBJECTS = $(patsubst %.c,%.o,$(SOURCES))

MOD_SRCS = $(wildcard modules/*.c loadbalances/*.c luaapis/*.c)
MOD_OBJS = $(patsubst %.c,%.o,$(MOD_SRCS))

phorklift: $(OBJECTS)
	make -C modules
	make -C loadbalances
	make -C luaapis
	$(CC) -o $@ $^ $(MOD_OBJS) $(LDFLAGS) $(LDLIBS)

include auto_headers.mk

clean:
	make -C loadbalances clean
	make -C modules clean
	make -C luaapis clean
	make clean_auto_headers
	rm -f *.o depends phorklift

all:
	make -C libwuya
	make -C libloop
	make -C libhttp2
	rm -f phorklift
	make phorklift

depends: $(SOURCES)
	$(CC) -MG -MM *.c > depends

include depends
