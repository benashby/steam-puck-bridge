# Third-party notices

`steam-puck-bridge` links against nothing but libc and contains no vendored
source code. It does, however, contain **protocol knowledge** — report IDs,
struct field offsets, setting numbers, the lizard-mode heartbeat interval and
the rumble output-report layout — that was derived by reading SDL 3's Steam
Triton driver.

## SDL (Simple DirectMedia Layer)

Files read: `src/joystick/hidapi/SDL_hidapi_steam_triton.c`,
`src/joystick/hidapi/steam/controller_constants.h`,
`src/joystick/hidapi/steam/controller_structs.h` (SDL 3.5.0 development tree,
July 2026).

The Steam controller headers in that tree carry
`Copyright (c) Valve Corporation`; SDL itself is
`Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>`.

SDL is distributed under the zlib license:

```
This software is provided 'as-is', without any express or implied
warranty.  In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.
```

Upstream: <https://github.com/libsdl-org/SDL>

The full wire-format reference reconstructed from those files is in
[docs/protocol.md](docs/protocol.md), with attribution repeated at the top of
[src/steam-puck-bridge.c](src/steam-puck-bridge.c).

## Not affiliated with Valve

"Steam", "Steam Controller", "Steam Deck" and "SteamOS" are trademarks of
Valve Corporation. This is an independent, unofficial project and is not
endorsed by or affiliated with Valve.
