CFLAGS= -Wall -Wextra -Werror -ggdb -Wno-unused-function -DMU_DEBUG

progs = adns 
objects = adns.o common.o mu.o
headers = mu.h uthash.h

all: $(progs)

adns: adns.o common.o mu.o
	$(CC) -o $@ $^

$(objects) : %.o : %.c $(headers)
	$(CC) -c -o $@ $(CFLAGS) $<

clean:
	rm -rf $(progs) $(objects)

.PHONY: all clean