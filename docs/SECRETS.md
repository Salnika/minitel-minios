# Secrets locaux

## Fichiers

- Template versionné: [local_secrets.example.h](../src/app/local_secrets.example.h)
- Fichier local ignoré par git: `src/app/local_secrets.h`

## Usage

Le firmware lit:

- `LOCAL_DEFAULT_WIFI_SSID`
- `LOCAL_DEFAULT_WIFI_PASSWORD`
- `LOCAL_DEFAULT_API_URL`
- `LOCAL_DEFAULT_API_TOKEN`
- `LOCAL_DEFAULT_AI_BASE_URL`
- `LOCAL_DEFAULT_AI_API_KEY`
- `LOCAL_DEFAULT_AI_MODEL`

Ces valeurs servent uniquement de seed locale. Une fois un réseau ou des réglages enregistrés, la source de vérité devient la NVS de l’ESP32.

## Règle

Ne pas committer `src/app/local_secrets.h`.
