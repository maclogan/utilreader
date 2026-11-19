# Lua 5.4.7 (https://www.lua.org, MIT licence: src/LICENSE), unmodified sources from the
# official release. Only the library: the standalone interpreter and compiler (lua.c, luac.c)
# are left out, and so are the io/os/debug libraries and linit.c, because the script host
# (components/app_script) opens just the libraries apps may use.
idf_component_register(SRCS "src/lapi.c" "src/lauxlib.c" "src/lbaselib.c" "src/lcode.c"
                            "src/lcorolib.c" "src/lctype.c" "src/ldebug.c" "src/ldo.c"
                            "src/ldump.c" "src/lfunc.c" "src/lgc.c" "src/llex.c"
                            "src/lmathlib.c" "src/lmem.c" "src/loadlib.c" "src/lobject.c"
                            "src/lopcodes.c" "src/lparser.c" "src/lstate.c" "src/lstring.c"
                            "src/lstrlib.c" "src/ltable.c" "src/ltablib.c" "src/ltm.c"
                            "src/lundump.c" "src/lutf8lib.c" "src/lvm.c" "src/lzio.c"
                       INCLUDE_DIRS "src")

# Third-party code: build it as it is, without turning its warnings into errors
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error -Wno-implicit-fallthrough)
