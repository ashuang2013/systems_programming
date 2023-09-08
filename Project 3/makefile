
CFLAGS = -Wall -Wextra -Werror

mcron: mcron.c mu.c 
	gcc -o $@ $(CFLAGS) $^ -lrt

clean:
	rm -f mcron

.PHONY: clean
