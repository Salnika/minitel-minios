# Contributing

Issues and pull requests are welcome. miniOS runs on an ESP32 wired to a Telic Alcatel 1B Minitel (see [Wiring](README.md#wiring)) and talks to Bibliotek, the server in [`server/`](server/README.md).

## Build and test

Firmware, with [PlatformIO](https://platformio.org/):

```sh
pio run -e esp32dev     # build (pio run -e esp32dev_en for the English texts)
pio test -e native      # Unity tests on the host, no board needed
pio test -e native_en   # the same tests against the English texts
```

Server, with Vite+ (`vp`), inside `server/`:

```sh
vp install
vp check
vp test
```

CI runs the same commands on every pull request.

## Conventions

[AGENTS.md](AGENTS.md) describes the layout and the coding style (where screens and network requests go, fixed-size buffers in firmware code, how to test); the server follows [server/AGENTS.md](server/AGENTS.md). Two rules matter most:

- Renderer changes come with a byte-level test (the `test_ui_*` cases) and Minitel link changes with a test against the fake Minitel (the `test_link_*` cases): neither can be checked without the terminal.
- Network calls go through `net::submit()` and come back in a callback on the loop, never straight from the loop.
- On-screen texts come in both languages: `TR("francais", "English")` from `src/app/lang.h`, with the same printf arguments, and both must fit the 40 columns and the buffer they go to.

## Pull requests

- Use Conventional Commits (`feat(minitel): ...`, `fix(server): ...`).
- Describe the intent and the changes, say which hardware you tried them on, and include serial logs (`pio device monitor -b 115200`) when relevant.
- Never commit `src/app/local_secrets.h` or `server/.env`.
