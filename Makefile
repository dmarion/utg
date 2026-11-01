APP := utg
SRC := utg.c cli.c stats.c arp.c
OBJ := $(SRC:.c=.o)
DPDK_DIR ?= /opt/dpdk
RPATH_DIRS := $(DPDK_DIR)/lib/x86_64-linux-gnu:$(DPDK_DIR)/lib64:$(DPDK_DIR)/lib
CWD := $(shell pwd)

PKG_CONFIG_PATH ?= $(DPDK_DIR)/lib/x86_64-linux-gnu/pkgconfig:$(DPDK_DIR)/lib64/pkgconfig:$(DPDK_DIR)/lib/pkgconfig
export PKG_CONFIG_PATH

CC ?= gcc
CFLAGS += -O2 -g -Wall -Wextra -Wpedantic $(shell pkg-config --cflags libdpdk)
LDFLAGS += $(shell pkg-config --libs libdpdk)

ifeq ($(V),1)
Q :=
else
Q := @
endif

.PHONY: all clean run-example

all: $(APP) compile_commands.json

$(APP): $(OBJ)
	$(Q)echo "  LD      $@"
	$(Q)$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)
	$(Q)patchelf --force-rpath --set-rpath "$(RPATH_DIRS)" $@

%.o: %.c
	$(Q)echo "  CC      $<"
	$(Q)$(CC) $(CFLAGS) -c -o $@ $<

compile_commands.json: $(SRC) Makefile
	$(Q)echo "  GEN     $@"
	$(Q)printf '[\n' > $@
	$(Q)count=0; \
	total=$(words $(SRC)); \
	for src in $(SRC); do \
		count=$$((count + 1)); \
		printf '  {\n' >> $@; \
		printf '    "directory": "%s",\n' "$(CWD)" >> $@; \
		printf '    "file": "%s/%s",\n' "$(CWD)" "$$src" >> $@; \
		printf '    "command": "%s %s -c %s -o %s.o"\n' \
			"$(CC)" "$(CFLAGS)" "$$src" "$$src" >> $@; \
		if [ "$$count" -eq "$$total" ]; then \
			printf '  }\n' >> $@; \
		else \
			printf '  },\n' >> $@; \
		fi; \
	done
	$(Q)printf ']\n' >> $@

clean:
	rm -f $(APP) $(OBJ) compile_commands.json

run-example: $(APP)
	./$(APP) -l 0-1 -n 4 -- --dst-mac 02:00:00:00:00:02 --src-ip 10.0.0.1 --dst-ip 10.0.0.2 --count 1000000
