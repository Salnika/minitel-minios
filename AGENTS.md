# Repository Guidelines

## Project Structure & Module Organization
- Source: `src/` (Arduino/ESP32 C++).
- Main entry: `src/main.cpp`, app state machine in `src/app/app.cpp`.
- New screens go in `src/app/screens/` and only use `src/app/app_shell.h` (status, navigation, text input callbacks, documents, services); add their id to `screen_id_t` and a case in `render_current_screen` / `dispatch_input`.
- Network calls go through `net::submit()` (`src/app/net_requests.h`, included by the shell) and come back in a callback on the loop: never call a service's HTTP method, a Wi-Fi scan or a Wi-Fi connect from the loop, not even at boot. A new kind of request adds its parameters/result to `net_request_t` / `net_reply_t` and its code to `run_request()`, which runs on the network task and must not touch the UART or the screens' state.
- UI rendering: `src/ui/`.
- Services: `src/services/`.
- Storage abstractions and mock store: `src/storage/`.
- Hardware/platform: `src/platform/`.
- Bundled low-level lib: `src/lib_minitel/`.
- Documentation: `docs/`.
- Server: `server/` (Bibliotek, Node/Fastify with Vite+), outside the PlatformIO build. Check it with `vp check` and `vp test` inside `server/`; Coolify deploys it from this folder (`docs/COOLIFY.md`).

## Build, Run & Development Commands
- Build: `pio run -e esp32dev` (uses `platformio.ini`), `pio run -e esp32dev_en` for the English texts.
- Flash app: `pio run -e esp32dev -t upload --upload-port /dev/cu.usbserial-0001`.
- Serial monitor: `pio device monitor -b 115200 -p /dev/cu.usbserial-0001`.
 

## Coding Style & Naming Conventions
- Language: C/C++ (Arduino). Indent 2 spaces; braces on same line (K&R).
- Files: headers `.h`, sources `.cpp`; file names `snake_case` (e.g., `minitel_io.cpp`).
- Identifiers: functions/vars `snake_case`; constants/macros `UPPER_CASE`; types `PascalCase` if introduced.
- Prefer fixed-size buffers over STL containers in firmware code.
- Keep Minitel rendering deterministic and explicit; avoid hidden clears/resets.
- On-screen texts are written `TR("francais", "English")` (`src/app/lang.h`): French by default, English in the `_en` environments. Both take the same printf arguments and fit the same width (40 columns, the buffer they are copied to). Logs, file and folder names (`projets/`, `taches.md`), JSON fields and NVS keys are never translated, and Minitel key names (`ENVOI`, `RETOUR`...) stay as printed on the keyboard.

## Testing Guidelines
- Native Unity tests live in `test/test_editor_layout.cpp`: it `#include`s the sources under test, stubs the Minitel UART (capturing every byte sent) and uses `test/stubs/Arduino.h` in place of the core. Run `pio test -e native` and `pio test -e native_en` (the same tests against the English texts: expected texts are written with `TR` too).
- Keep hardware interactions thin; test pure logic where possible. Renderer changes should come with a byte-level test (see the `test_ui_*` cases), since they cannot be checked without the terminal. Link changes are tested against the fake Minitel of the `test_link_*` cases, which answers Protocole commands at its own speed.

## Commit & Pull Request Guidelines
- Use Conventional Commits: `feat(minitel): init UART 7E1`, `fix(minitel): correct RX/TX pins`.
- PRs: describe intent, list changes, link issues, target hardware (ESP32 + Minitel), and include serial logs when relevant.

## Hardware Notes
- Default pins: RX=`GPIO22`, TX=`GPIO23`, `SERIAL_7E1`; `1200` baud at power-on, then `4800` with the Minitel routing set by `src/ui/minitel_link.*` (Protocole commands, see `docs/ARCHITECTURE.md`).
- Target terminal: Telic Alcatel 1B, 40x25.
- Cursor positioning uses Videotex `US/APA` (`0x1F`, row, col), not `ESC Y`.
- Keep line 1 for the top bar, line 23 for status, and line 24 for footer hints.

## Agent Notes
- Keep changes minimal and idiomatic to existing patterns.
- Do not commit `src/app/local_secrets.h`; use `src/app/local_secrets.example.h` as the template.
- When adding features, provide `pio` commands to reproduce and verify on ESP32.
