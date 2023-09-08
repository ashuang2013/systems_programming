CFLAGS= -Wall -Wextra -Werror -ggdb -Wno-unused-function -DMU_DEBUG

progs = adns adns_lookup 
objects = adns.o adns_lookup.o common.o mu.o
headers = mu.h uthash.h

all: $(progs)

adns: adns.o common.o mu.o
	$(CC) -o $@ $^

adns_lookup: adns_lookup.o common.o mu.o
	$(CC) -o $@ $^

$(objects) : %.o : %.c $(headers)
	$(CC) -c -o $@ $(CFLAGS) $<

clean:
	rm -rf $(progs) $(objects)

.PHONY: all clean
