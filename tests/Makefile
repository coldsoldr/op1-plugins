# The adapter's host tests (any C compiler: WSL's gcc here).
#   make -C tests
CC ?= gcc
CFLAGS = -std=gnu11 -O1 -Wall -Wextra -Werror -I../adapter -I../coop-sdk -I../plugin-kit/sdk
OUT ?= /tmp/op1-plugins-tests

.PHONY: test
test: $(OUT)/ctl_test $(OUT)/input_test
	$(OUT)/ctl_test
	$(OUT)/input_test

$(OUT)/ctl_test: ctl_test.c ../adapter/copa_ctl.c ../adapter/copa.h
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) ctl_test.c ../adapter/copa_ctl.c -o $@

$(OUT)/input_test: input_test.c ../adapter/copa_input.c ../adapter/copa_ctl.c ../adapter/copa.h
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) input_test.c ../adapter/copa_input.c ../adapter/copa_ctl.c -o $@
