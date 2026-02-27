set -e

DEBUG=${DEBUG:-0}
PKGS="wlroots-0.19 wayland-server xkbcommon freetype2 glesv2 libinput libsystemd"

# Extremely strict warning flags
WARNINGS="
    -Wall
    -Wextra
    -Werror
    -Wshadow
    -Wconversion
    -Wsign-conversion
    -Wcast-qual
    -Wcast-align
    -Wstrict-prototypes
    -Wmissing-prototypes
    -Wmissing-declarations
    -Wredundant-decls
    -Wwrite-strings
    -Wdouble-promotion
    -Wfloat-equal
    -Wundef
    -Wformat=2
    -Wformat-overflow=2
    -Wformat-truncation=2
    -Wnull-dereference
    -Winit-self
    -Wuninitialized
    -Wstrict-overflow=5
    -Walloca
    -Wduplicated-cond
    -Wduplicated-branches
    -Wlogical-op
    -Wrestrict
    -Wvla
    -Wswitch-default
    -Wstack-protector
    -Wpointer-arith
    -Wbad-function-cast
    -Wnested-externs
    -Wold-style-definition
    -Wstrict-aliasing=3
    -Wjump-misses-init
    -Wtrampolines
    -Winvalid-pch
"

WARNINGS=$(echo $WARNINGS | tr -s ' ')

LDFLAGS="$(pkg-config --libs $PKGS) -lm"

if [ "$DEBUG" = "1" ]; then
    # Debug build: all sanitizers, no optimization, debug symbols
    CC=clang
    CFLAGS="-std=c99 -O0 -g3 -Wall -Wextra -DWLR_USE_UNSTABLE $(pkg-config --cflags $PKGS)"
    SANITIZE="
        -fsanitize=address
        -fsanitize=undefined
        -fsanitize=leak
        -fsanitize=signed-integer-overflow
        -fsanitize=unsigned-integer-overflow
        -fsanitize=null
        -fsanitize=bounds
        -fsanitize=alignment
        -fsanitize=float-divide-by-zero
        -fsanitize=shift
        -fsanitize=integer-divide-by-zero
        -fsanitize=unreachable
        -fsanitize=return
        -fsanitize=vla-bound
        -fsanitize=pointer-overflow
        -fsanitize=builtin
        -fno-omit-frame-pointer
        -fno-optimize-sibling-calls
    "
    SANITIZE=$(echo $SANITIZE | tr -s ' ')
    SECURITY=""
    LDFLAGS="$LDFLAGS $SANITIZE"
else
    # Release build: optimized, maximum hardening
    CFLAGS="-std=c99 -O2 $WARNINGS -DWLR_USE_UNSTABLE $(pkg-config --cflags $PKGS)"
    SANITIZE=""
    SECURITY="
        -fstack-protector-strong
        -fstack-clash-protection
        -fcf-protection
        -D_FORTIFY_SOURCE=2
        -fPIE
    "
    SECURITY=$(echo $SECURITY | tr -s ' ')
    LDFLAGS="$LDFLAGS -pie -Wl,-z,relro,-z,now -Wl,-z,noexecstack"
fi

wayland-scanner server-header \
	/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml \
	xdg-shell-protocol.h

wayland-scanner server-header \
	/usr/share/wayland-protocols/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml \
	pointer-constraints-unstable-v1-protocol.h

wayland-scanner server-header \
	/usr/share/wayland-protocols/unstable/relative-pointer/relative-pointer-unstable-v1.xml \
	relative-pointer-unstable-v1-protocol.h

# Static analysis
cppcheck --enable=all --std=c99 --error-exitcode=1 \
    --suppress=missingIncludeSystem \
    --suppress=constParameterCallback \
    --suppress=checkersReport \
    --suppress=unusedFunction \
    --check-level=exhaustive \
    --force --quiet rwm.c sysinfo.c

# GCC static analyzer
gcc -fanalyzer -std=c99 -O2 -Wall -Wextra -DWLR_USE_UNSTABLE \
    $(pkg-config --cflags $PKGS) $SECURITY -I. -fsyntax-only rwm.c sysinfo.c 2>&1 \
    | grep -v "note:" || true

# Clang static analyzer
CLANG_FLAGS="-std=c99 -O2 -Wall -Wextra -DWLR_USE_UNSTABLE $(pkg-config --cflags $PKGS)"
scan-build --status-bugs \
    -enable-checker unix.Malloc \
    -enable-checker core.NullDereference \
    -enable-checker deadcode.DeadStores \
    clang $CLANG_FLAGS $SECURITY -I. -o /dev/null rwm.c sysinfo.c $LDFLAGS

${CC:-cc} $CFLAGS $SANITIZE $SECURITY -I. -o rwm.elf rwm.c sysinfo.c $LDFLAGS

[ "$DEBUG" = "1" ] && echo "Debug build with ASan+UBSan enabled"
