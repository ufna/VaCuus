VaCuus HUD Demo (RmlUi + QuickJS-ng)
====================================

Run (windowed, from this directory):

    cd /tmp/claude-1000/-w-Unreal-VaCuus/00503194-0da6-4afd-80a7-cee3278ced44/scratchpad/vacuus-jsdemo
    ./build/vacuus_hud

Controls:
    1-4    trigger abilities (cooldown sweep + ready flash)
    Space  burst fire (damage numbers + hit marker)
    Esc    settings panel (backdrop blur, scale-in)
    Mouse  click ability slots / RESUME button

Optional flags:
    --seconds N   auto-exit after N seconds (also enables scripted AUTO
                  input + per-second stats on stdout)
    --shot PREFIX save PREFIX_{1,2,3}.bmp framebuffer screenshots at
                  t=2.7/3.7/5.5s (needs --seconds >= 6)
    --data DIR    data directory (default: ./data)

Headless smoke test (no window):

    SDL_VIDEODRIVER=offscreen ./build/vacuus_hud --seconds 6

Rebuild:

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
    ninja -C build

Prebuilt deps expected:
    ../bench/RmlUi/build/librmlui.a   (RmlUi master, static)
    ./qjs/build/libqjs.a              (quickjs-ng, static; cmake -B build
                                       -DCMAKE_BUILD_TYPE=Release && ninja -C build qjs)
System deps: sdl2, SDL2_image, freetype2 (pkg-config).
