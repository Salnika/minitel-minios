# Security

## Reporting a vulnerability

Please report vulnerabilities privately through [GitHub's private vulnerability reporting](https://github.com/Salnika/minitel-minios/security/advisories/new), not in a public issue. Only the latest `master` is maintained.

## Before deploying

miniOS is built for one person and their own Bibliotek server:

- Every Bibliotek route except `GET /health` takes the same bearer token (`API_TOKEN`). Whoever has it can read, change and delete every note and publish firmware for the Minitel, so serve Bibliotek over HTTPS only and use a long random token.
- The same token is the ArduinoOTA password for updates from the local network.
- The device keeps the Wi-Fi password, API token, AI key and PIN in NVS with an XOR obfuscation, which is not encryption: anyone holding the board can read them back.
- With the assistant tools on, the Minitel sends its AI key to Bibliotek with each question, and refuses to do so over plain `http`.
