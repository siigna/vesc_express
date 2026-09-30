# Host tests for the script container parser.
#
# The parser is compiled straight from the firmware source, not copied, so the
# test cannot drift from what ships. It has no firmware dependencies, which is
# the point: untrusted-input parsing belongs where it can be fuzzed and run
# under sanitizers.

CFLAGS += -O1 -g -std=c11 -Wall -Wextra -Wconversion -Wsign-conversion
SAN ?= -fsanitize=address,undefined -fno-omit-frame-pointer

LUA_DIR ?= ../../lua
# Same configuration the firmware builds Lua with, so the engine under test is
# the engine that ships. LUA_32BITS is set in the vendored luaconf.h rather
# than here, because Lua defines it itself and a command-line define collides.
GFX_DIR ?= ../../lispBM/utils
DISP_DIR ?= ../../display
LUA_CFLAGS = -I$(LUA_DIR) -I$(GFX_DIR) -I$(DISP_DIR) -DLUA_USE_APICHECK
LUA_SRCS = $(wildcard $(LUA_DIR)/*.c)

.PHONY: all run clean fuzz
all: run

test_script_pack: test_script_pack.c ../script_pack.c ../script_pack.h
	$(CC) $(CFLAGS) $(SAN) -o $@ test_script_pack.c ../script_pack.c

# Lua itself is compiled without the warning flags used for our own code:
# it is vendored third-party source and we do not edit it.
lua_objs.a: $(LUA_SRCS)
	@mkdir -p .luaobj
	@for f in $(LUA_SRCS); do \
		$(CC) -O1 -g -std=c11 $(LUA_CFLAGS) $(SAN) -c $$f \
			-o .luaobj/`basename $$f .c`.o || exit 1; \
	done
	@$(AR) rcs $@ .luaobj/*.o

# tinygfx and tjpgd come straight from the tree: the drawing under test is the
# drawing that ships, and it needs nothing from either interpreter.
test_script_lua: test_script_lua.c ../script_lua.c ../script_pack.c \
		../lua_vesc_disp.c $(DISP_DIR)/disp_backend.c \
		$(GFX_DIR)/tinygfx.c $(GFX_DIR)/tjpgd.c lua_objs.a
	$(CC) $(CFLAGS) -Wno-conversion -Wno-sign-conversion $(LUA_CFLAGS) $(SAN) \
		-o $@ test_script_lua.c ../script_lua.c ../script_pack.c \
		../lua_vesc_disp.c $(DISP_DIR)/disp_backend.c \
		$(GFX_DIR)/tinygfx.c $(GFX_DIR)/tjpgd.c lua_objs.a -lm

# The fixture is packed by the real tool, so the Python writer and the C
# reader are checked against each other on every run.
packed.bin: fixtures/main.lua fixtures/mod.lua fixtures/pkg/sub.lua ../../../tools/luapack.py
	python3 ../../../tools/luapack.py fixtures/main.lua -o $@

run: test_script_pack test_script_lua packed.bin
	./test_script_pack
	python3 ../../../tools/luapack.py --selftest
	./test_script_lua packed.bin

# Property fuzzing: any byte string at all must either parse or be refused,
# and every import it reports must be in bounds. Needs clang.
fuzz_script_pack: fuzz_script_pack.c ../script_pack.c ../script_pack.h
	clang $(CFLAGS) -fsanitize=fuzzer,address,undefined \
		-o $@ fuzz_script_pack.c ../script_pack.c

fuzz: fuzz_script_pack
	./fuzz_script_pack -max_total_time=$(or $(FUZZ_SECONDS),30) -print_final_stats=1

clean:
	rm -f test_script_pack test_script_lua fuzz_script_pack lua_objs.a packed.bin
	rm -rf crash-* leak-* timeout-* .luaobj
