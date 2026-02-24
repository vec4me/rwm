# rwm

A minimal, retro-inspired Wayland window manager. Currently built on wlroots, with the goal of eventually dropping it in favor of direct Wayland/libinput.

## Dependencies

- wlroots 0.19
- wayland-protocols
- xkbcommon
- freetype2
- libinput

## Building

Generate the required Wayland protocol headers:

```sh
wayland-scanner server-header \
    /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml \
    xdg-shell-protocol.h

wayland-scanner server-header \
    /usr/share/wayland-protocols/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml \
    pointer-constraints-unstable-v1-protocol.h

wayland-scanner server-header \
    /usr/share/wayland-protocols/unstable/relative-pointer/relative-pointer-unstable-v1.xml \
    relative-pointer-unstable-v1-protocol.h
```

Then build with:

```sh
./build.sh
```
