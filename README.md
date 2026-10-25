# miniOS Minitel (minisek)

[![CI](https://github.com/Salnika/minitel-minios/actions/workflows/ci.yml/badge.svg)](https://github.com/Salnika/minitel-minios/actions/workflows/ci.yml) [![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

ESP32 firmware that turns a Telic Alcatel 1B Minitel into an AI-assisted project and note manager: projects with tasks, due dates, reminders and time tracking, quick capture, a daily journal, full-text search, a dense text editor and an AI assistant that can read and change the notes, all rendered in Videotex over UART. Notes and projects live on Bibliotek, a small file API in [`server/`](server/README.md), deployed with Coolify.

## What it does

The firmware drives the Minitel terminal as its only display and keyboard. `src/app/app.cpp` owns navigation, the status line, text input, the explorer, editor, chat and settings; the other screens are modules in `src/app/screens/` that only use the shell API of `src/app/app_shell.h`:

- **Boot**: wakes the terminal with a timed sequence, draws a mosaic boot logo, then shows the home screen (or the PIN screen) without waiting for Wi-Fi: the saved network is joined in the background, the top bar shows `offline` until the link is up, and NTP, network updates and the server check start then. With the network away, the radio keeps trying it.
- **Network requests** (Bibliotek, AI chat, updates, Wi-Fi scan and connect) never freeze the screen: they run one at a time on a FreeRTOS task while the loop keeps drawing and reading keys. The status line shows the request with a timer (`Chargement des projets... 3s ANN annule`); `ANNULATION` cancels it, and so does leaving its screen. Background requests (the boot Wi-Fi connect, today's tasks, server health) stay silent; a request made during the boot connect waits for it (`Attente du WiFi...`, 15 s at most when the network is away).
- **Top bar / status / footer**: a permanent top bar shows the active screen, network state and NTP time; line 23 is a status line and line 24 a footer hint. Rendering is buffered and diffed against what the terminal shows: only changed cells are sent (nearby changes merged into one run), stale line ends are erased with a single `CAN` (`0x18`), and a full refresh after `FF` skips blank cells (~100 bytes instead of ~1000 for a sparse page). Changing screen clears the page first (`FF`), as at boot, so nothing of the previous screen stays behind; within a screen only the changes are sent. Nothing is sent when nothing changed, and drawing stops between two rows when a key is waiting: the key is handled first, so typing or scrolling never waits for a page that is already out of date.
- **Minitel link**: at boot the firmware finds the Minitel (1200 baud after power-on), switches it to **4800 baud** and sets its routing with Protocole commands: the socket draws straight on the screen and the keyboard sends to the socket only. The Minitel no longer prints typed keys itself (its local echo), which used to land in the middle of a page being drawn. `CONNEXION/FIN` puts the Minitel back to its standard routing: the link is checked again once its status codes settle, and a Minitel switched off and on is set up again and redrawn. `SYSTEME` shows the result (`Minitel: 4800 bauds, echo coupe`).
- **ACCUEIL (dashboard)**: date, today's task count (due today or late), the running project timer, a menu (`1-9` opens directly) and the last 4 opened documents.
- **PROJETS**: projects from `projets/<name>/` with their open and late tasks. A project shows its sheet (status, goal, next step), the week's logged time and its open tasks. Keys:
  - `ENVOI` checks or unchecks the selected task;
  - `A` adds a task, typed as `Commander colle !! @25/10 14h30` (also `@demain`, `@auj` or `@2026-10-25`);
  - `F` and `T` open `projet.md` and `taches.md` in the editor;
  - `C` starts or stops the timer: the time goes to `temps.md` and the timer survives a reboot;
  - `N`, from the project list, creates a project.
- **TACHES**: tasks due today or late across every project (`T` shows all open tasks). `ENVOI` checks a task, `O` opens its file on the task line.
- **Reminders**: tasks due today with a time ring the Minitel bell and show a status line when their time comes. The list is refreshed in the background every 5 minutes, and soon after a task changes.
- **Capture rapide**: one line appended to `inbox/inbox.md` with the date and time.
- **Journal du jour**: opens `journal/YYYY-MM-DD.md` under a new `## HH:MM` heading.
- **RECHERCHE**: case- and accent-insensitive search in every note and project. `ENVOI` opens the document on the matching line.
- **SYSTEME**: firmware version, chip, free memory, flash use, uptime, last reset reason, Wi-Fi signal, IP, server and AI model; checks and installs updates published on the server; restarts. Actions need a second `ENVOI`.
- **Code PIN** (optional, `Reglages`): asked at boot, with a 30 s wait after 3 wrong codes.
- **WIFI**: scan networks, connect, and remember a default network (persisted in NVS). A connect that fails, or that `ANNULATION` or `RETOUR` stops, goes back to the saved network.
- **SETTINGS**: docs API URL/token, AI base URL/key, the AI model, the assistant's tools (`Outils IA`), timezone, PIN and reset. The model is picked from the live list of DigitalOcean-owned chat models (embedding, speech, image and router models are filtered out), with `M` to type an id by hand. The default is `deepseek-v4.1-flash`, and devices still on the old `openai-gpt-oss-120b` default are moved to it once.
- **FILES (Notes)**: a file explorer (folders + documents) with tag filtering and folder/document creation, backed by the Bibliotek API store.
- **EDITOR**: a dense 40-column text editor over ~19 usable rows with visual wrapping (no auto-inserted newlines), a command overlay (search / goto-line / quit / redraw), dirty-state exit confirmation, and save-without-exit. A document that does not fit the device buffers (body over 2 KB, title over 32 bytes, more than 4 tags or a tag over 16 bytes) opens **read-only**, so a save can never write a truncated copy back to the server.
- **CHAT**: an AI chat client bounded to a few in-RAM exchanges, targeting DigitalOcean Gradient inference (`https://inference.do-ai.run/v1`). Replies are streamed (`"stream": true`, server-sent events) and drawn as they arrive, word-wrapped; when a reply reaches the bottom row the view jumps a page instead of scrolling line by line, which would resend the whole transcript. `ANNULATION` stops a reply and keeps what arrived. The newest exchange always stays visible, and replies cut by the token limit, the buffer or `ANNULATION` are marked `[reponse tronquee]`.
- **Assistant with tools**: with `Outils IA` on (the default) and the Docs URL and token set, the question goes to Bibliotek's assistant, which runs the model with tools on the notes: list folders, read, search, create, append to, edit, move and delete files, list the projects and their tasks, create projects, add and tick tasks. It sends the device's AI settings along with the question, so the key is only set on the Minitel (Bibliotek must be reached over `https`). Each action shows under the question as it runs (` * lit notes/courses.md`, ` * modifie ... (refuse)`), and the actions of an exchange go back to the model with the history. An action that replaces, edits or deletes existing text waits for the user: ` ? Modifier notes/courses.md : "- pain" -> "- pain complet"`, with `ENVOI` to accept, `CORRECTION` to refuse (the model is told and goes on) and `ANNULATION` to drop the exchange; creating a file, appending, moving a file and ticking a task run without asking. `RETOUR` leaves the question waiting (5 min on the server), to check the note first. With `Outils IA` off, or without Docs URL and token, the chat talks to the model directly, as before.

Supporting modules:
- `src/app/net_requests.*`: every network request (Bibliotek, AI chat, server updates, Wi-Fi scan and connect), with its parameters, the code that runs on the network task and the callback the loop gets.
- `src/app/workspace.*`: today's tasks, reminders, the timer, recent documents.
- `src/services/`: Wi-Fi, clock/NTP, NVS settings, chat client and its stream decoder (`sse_parser`), model filter, updates (`ota_service`), TLS roots, the network task (`net_worker`) and its queue (`net_queue`).
- `src/storage/`: the Bibliotek client and its parsers (`api_document_store_utils`, `workspace_utils`).
- `src/platform/`: Arduino/UART wrappers, logging.
- `src/ui/`: the buffered Videotex renderer.

`src/lib_minitel/` is a bundled low-level toolkit (graphics engine, mosaic map, Wi-Fi, SSH stub, screen manager).

HTTPS is checked against a small bundled set of root CAs (`src/services/tls_roots.cpp`, exported from the macOS trust store): Let's Encrypt ISRG Root X1/X2 for a Coolify server and Google Trust Services for DigitalOcean inference.

## Hardware

- **Board**: `esp32dev` (ESP32 dev module), Arduino framework.
- **Terminal**: Telic Alcatel 1B Minitel, 40x25 geometry, driven in Videotex mode.
- Serial monitor / logging runs at 115200 baud on the USB port.
- Cursor positioning uses Videotex `US/APA` (`0x1F row col`), not `ESC Y`. See `SEQUENCES.md` for the control sequences and `docs/KEYMAP_TELIC_1B.md` for the key mapping.

### Wiring

The Minitel's 5-pin DIN socket at the back (the "péri-informatique" port) connects to the ESP32's UART1 (`Serial1`):

| Minitel DIN pin | Signal | ESP32 |
|---|---|---|
| 1 | RX: data into the Minitel | `GPIO23` (UART TX) |
| 2 | Ground | `GND` |
| 3 | TX: data out of the Minitel (open collector) | `GPIO22` (UART RX), plus a ~10 kΩ pull-up to **3.3 V** |
| 4, 5 | Not used | Leave unconnected |

- Link settings: `SERIAL_7E1` (forced in `arduino_wrapper.cpp` regardless of caller), `1200` baud at power-on, then `4800` once the Minitel acknowledges the change (`MINITEL_LINK_BAUD` in `src/app/app.cpp`; build with `-DMINITEL_LINK_BAUD=1200` to keep 1200). If the Minitel refuses or does not answer, the link stays at 1200 with the echo filter, as before. The pins are `MINITEL_RX_PIN` / `MINITEL_TX_PIN` in `src/app/app.cpp`.
- Pull the Minitel's TX up to 3.3 V, not 5 V: ESP32 GPIOs are not 5 V tolerant, and the open-collector output works just as well at 3.3 V.
- Power the ESP32 from its USB port. Besides the two data lines, the only connection to the Minitel is the shared ground.
- On the DIN connector, pin 2 is the centre pin of the arc and pins 1 and 3 are the two outer ones. The numbers are usually moulded next to the pins: check them before soldering, since the order is mirrored between the plug and the socket.

## Build & flash

```sh
pio run -e esp32dev
pio run -e esp32dev -t upload            # or add --upload-port /dev/cu.usbserial-0001
pio device monitor -b 115200             # add -p /dev/cu.usbserial-0001 if needed
```

The firmware uses the `min_spiffs.csv` partition table (two 1.9 MB app slots for over-the-air updates). The **first flash after this change must be over USB**: an update cannot rewrite the partition table. NVS stays at `0x9000`, so settings survive.

### Language

The on-screen texts are chosen at build time: French by default, English with the `_en` environments (`src/app/lang.h`). The Minitel's key names (`ENVOI`, `RETOUR`, `ANNULATION`...) stay as printed on its keyboard, and the notes keep the same layout in both languages (`projets/`, `taches.md`, `Statut:`), so both builds share one server and the same data. The assistant answers in the build's language: the Minitel sends it with each question. Tasks take `@today` and `@tomorrow` as well as `@auj` and `@demain`. This README quotes the French screens.

```sh
pio run -e esp32dev_en -t upload                          # English build over USB
MINIOS_OTA_PASSWORD=<token> pio run -e ota_en -t upload   # English build, same Wi-Fi
MINIOS_LANG=en MINIOS_API_URL=https://bibliotek.example.fr MINIOS_API_TOKEN=<token> scripts/publish_firmware.sh
```

### Updates without a cable

Bump `MINIOS_FIRMWARE_VERSION` in `src/app/version.h` first: the device only installs a newer version.

- **Same Wi-Fi** (ArduinoOTA, host `minitel-minios.local`, shown as `Maj par PC` in `SYSTEME`; the password is the docs API token, and without a token this mode stays off):

  ```sh
  MINIOS_OTA_PASSWORD=<token> pio run -e ota -t upload
  ```

- **From anywhere**: publish on Bibliotek, then on the Minitel go to `Systeme > Verifier la mise a jour` and press `ENVOI` again to install. The download is checked against the MD5 from the manifest before the device reboots. The Minitel asks `<Docs URL>/firmware/manifest` with the `Docs token` of `Reglages`: the host shown on `Serveur ok:` in `SYSTEME`, so publish with that same URL. `Deja a jour (1.2.1)` means the server holds this same version; `Deja a jour (1.2.1), serveur 1.2.0` that it still holds an older build; `Deja a jour (1.2.1), rien de publie` that nothing was published there; `Pas de /firmware: serveur a redeployer` that the Docs URL serves an old Bibliotek without the update routes (`docs/COOLIFY.md`). Before 1.2.2, a trailing `/` in the Docs URL broke the check (the Minitel asked `//firmware/manifest` and showed `Aucune version publiee`): on older firmware, remove it in `Reglages`.

  ```sh
  MINIOS_API_URL=https://bibliotek.example.fr MINIOS_API_TOKEN=<token> scripts/publish_firmware.sh
  ```

## Server

Notes, projects, tasks and published firmware live in Bibliotek, in [`server/`](server/README.md) (Fastify, plain files on disk). Its README explains the routes the Minitel uses, and `docs/COOLIFY.md` explains the deployment: this repository, Base Directory `/server`, Dockerfile, a volume on `/data`, `API_TOKEN`, HTTPS domain. On the Minitel, set `Reglages > Docs URL` to the **https** URL and `Docs token` to `API_TOKEN`.

There is also a native unit-test environment (Unity). It covers:
- the pure logic: editor layout, input parser, API/chat/project parsing, typed tasks and dates, text wrapping, timezone rules, model filtering, update manifest;
- the exact bytes the Videotex renderer sends.

```sh
pio test -e native
pio test -e native_en   # the same tests against the English texts
```

### Secrets

Wi-Fi and AI credentials are not committed. Copy `src/app/local_secrets.example.h` to `src/app/local_secrets.h` (git-ignored). The firmware loads NVS settings first and falls back to `local_secrets.h` when no network is stored; AI defaults can also be seeded there.

## Libraries

- `bblanchon/ArduinoJson` ^6.21.5 (chat / API JSON).
- Arduino ESP32 core: `WiFi`, `HardwareSerial`, NVS/Preferences, NTP.
- Bundled in-tree: `lib_minitel` (graphics/mosaic, Wi-Fi, SSH stub, screen manager), pulled in via `-Isrc/lib_minitel/include`; its `examples/` are excluded from the build.

## Notes

- Minitel keys (Telic 1B) map to navigation and editor actions: `REPETITION`/`SUITE` = up/down, `SOMMAIRE`/`GUIDE` = left/right, `ENVOI` = confirm / newline, `CORRECTION` = delete, `RETOUR` = back / exit editor, `CONNEXION/FIN` = save without exit, `ANNULATION` = editor command menu.
- The UART wrapper can drop the terminal's echo of our own bytes before it reaches the input parser. That filter is only on when the routing could not be confirmed, or when a probe still sees our bytes come back (`Minitel: ..., filtre` in `SYSTEME`); the serial log shows the link state at boot and after each check (`minitel` tag).
- `partitions.csv` (one 2 MB factory app, no update slot) is kept for reference but unused: updates need two app slots.
- Timezone setting: IANA names from a built-in table (`Europe/Paris`, overseas territories such as `Indian/Reunion`, a few major zones; any case) or any POSIX TZ rule typed directly (e.g. `CET-1CEST,M3.5.0,M10.5.0/3`). newlib cannot parse IANA names itself.
- `Reset settings` asks for a second `ENVOI` before wiping NVS.
- The server lives in `server/` (Bibliotek, Node/Fastify, outside the PlatformIO build); see also `docs/` (`ARCHITECTURE.md`, `API_CONTRACT.md`, `COOLIFY.md`, `SECRETS.md`), written in French like the on-screen text.
- `build_flags` define `ARDUINO_WRAPPER_NO_SKETCH`; the actual `setup()`/`loop()` live in `src/main.cpp` and delegate to `app_setup()`/`app_loop()`.

## Contributing

Issues and pull requests are welcome: see [CONTRIBUTING.md](CONTRIBUTING.md). Report security issues as described in [SECURITY.md](SECURITY.md).

## License

[MIT](LICENSE)
