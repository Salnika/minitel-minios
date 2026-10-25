#!/usr/bin/env bash
# Builds the firmware and publishes it on the Bibliotek server; the Minitel then installs
# it from Systeme > Verifier la mise a jour.
#
#   MINIOS_API_URL=https://bibliotek.example.fr MINIOS_API_TOKEN=... scripts/publish_firmware.sh
#
# MINIOS_API_URL must be the Docs URL set on the Minitel (Reglages): updates are looked for
# there. Bump MINIOS_FIRMWARE_VERSION in src/app/version.h first: the device only installs a
# newer version.
set -euo pipefail
cd "$(dirname "$0")/.."

: "${MINIOS_API_URL:?set MINIOS_API_URL to the https URL of Bibliotek}"
: "${MINIOS_API_TOKEN:?set MINIOS_API_TOKEN to its API_TOKEN}"
api_url="${MINIOS_API_URL%/}"

version=$(sed -n 's/^#define MINIOS_FIRMWARE_VERSION "\(.*\)"$/\1/p' src/app/version.h)
if [ -z "$version" ]; then
  echo "MINIOS_FIRMWARE_VERSION not found in src/app/version.h" >&2
  exit 1
fi

pio run -e esp32dev

response=$(mktemp)
trap 'rm -f "$response"' EXIT

status=$(curl -sS -o "$response" -w '%{http_code}' -X PUT "${api_url}/firmware?version=${version}" \
  -H "Authorization: Bearer ${MINIOS_API_TOKEN}" \
  -H "Content-Type: application/octet-stream" \
  --data-binary @.pio/build/esp32dev/firmware.bin)
if [ "$status" != "201" ]; then
  echo "Publication refusee (HTTP ${status}) : $(cat "$response")" >&2
  case "$status" in
    404) echo "Ce serveur n'a pas la route /firmware : c'est un ancien Bibliotek. Deploie server/ (docs/COOLIFY.md)." >&2 ;;
    401 | 403) echo "Token refuse : MINIOS_API_TOKEN doit valoir l'API_TOKEN du serveur." >&2 ;;
    413) echo "Firmware trop gros pour le serveur : augmente FIRMWARE_MAX_BYTES." >&2 ;;
  esac
  exit 1
fi

# Read back what the Minitel will get.
status=$(curl -sS -o "$response" -w '%{http_code}' "${api_url}/firmware/manifest" \
  -H "Authorization: Bearer ${MINIOS_API_TOKEN}")
manifest=$(cat "$response")
if [ "$status" != "200" ] || ! printf '%s' "$manifest" | grep -Eq "\"version\": ?\"${version}\""; then
  echo "Le serveur ne renvoie pas la version ${version} (HTTP ${status}) : ${manifest}" >&2
  exit 1
fi

echo "Version ${version} publiee sur ${api_url} : ${manifest}"
echo "Sur le Minitel (Docs URL = ${api_url}) : Systeme > Verifier la mise a jour"
