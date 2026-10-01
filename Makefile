superdrive-wake: superdrive-wake.c
	clang -O2 -Wall -Wextra -framework IOKit -framework CoreFoundation -o $@ $<

clean:
	rm -f superdrive-wake

.PHONY: clean
