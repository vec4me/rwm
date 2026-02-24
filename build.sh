set -e

PKGS="wlroots-0.19 wayland-server xkbcommon freetype2 glesv2 libinput"

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

CFLAGS="-std=c99 -O2 $WARNINGS -DWLR_USE_UNSTABLE $(pkg-config --cflags $PKGS)"
LDFLAGS="$(pkg-config --libs $PKGS) -lm"
SECURITY="-fstack-protector-strong -D_FORTIFY_SOURCE=2"

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
    --check-level=exhaustive \
    --force --quiet rwm.c sysinfo.c

${CC:-cc} $CFLAGS $SECURITY -I. -o rwm.elf rwm.c sysinfo.c $LDFLAGS
