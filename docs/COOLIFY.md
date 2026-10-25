# Déploiement Coolify

Le serveur du miniOS est **Bibliotek**, dans `server/` : une petite API Fastify qui range les notes et projets en fichiers texte. Il n'y a pas de base de données : un volume suffit.

## Bibliotek sur Coolify

1. **+ New → Application**, depuis ce dépôt Git (`https://github.com/Salnika/minitel-minios`, ou ton fork), branche `main`.
2. **Build Pack** : `Dockerfile`, **Base Directory** : `/server`, **Port exposé** : `3000`. Pour ne redéployer que quand le serveur change, mets `server/**` dans les **Watch Paths**.
3. **Variables d'environnement** :
   - `API_TOKEN` (secret) : le même que `Docs token` sur le Minitel. Il sert aussi de mot de passe pour les mises à jour par le réseau local.
   - optionnel : `TZ=Europe/Paris`, `MAX_FILE_SIZE_BYTES`, `FIRMWARE_MAX_BYTES`.
4. **Stockage persistant** : un volume monté sur `/data`. Les notes vont dans `/data/files`, le firmware publié dans `/data/firmware`. Sans ce volume, tout est perdu au redéploiement.
5. **Domaine** : en `https://`. Coolify génère le certificat Let's Encrypt, que le firmware vérifie avec ISRG Root X1/X2.
6. **Santé** : l'image déclare un `HEALTHCHECK` Docker (`GET /health`). Le health check propre à Coolify a besoin de `curl` ou `wget`, absents de l'image : laisse-le désactivé.

### Depuis l'ancien dépôt `bibliotek`

Le dépôt `bibliotek` publié n'a que `/folders` et `/files` : ni projets, ni tâches, ni recherche, ni `/firmware`. Le Minitel y répond « Pas de /firmware: serveur a redeployer » (« Aucune version publiee » avant la 1.2.1) et `publish_firmware.sh` s'arrête sur un 404. Plutôt que de créer une deuxième app, change la source de l'app existante : ce dépôt, branche `main`, Base Directory `/server`. Garde le même volume sur `/data` (les notes restent dans `/data/files`), le même `API_TOKEN` et le même domaine, puis redéploie.

Pour savoir quel serveur répond, avec le token :

```sh
curl -i -H "Authorization: Bearer <token>" https://bibliotek.example.fr/firmware/manifest
```

`{"error":"Not found"}` : ancien serveur, à redéployer. `{"error":"No firmware published"}` : bon serveur, rien n'est encore publié.

## Côté Minitel

- `Reglages > Docs URL` : l'URL **https** du domaine. Une URL `http://` redirigée par Coolify s'affiche comme « API redirigee : utilise https:// ».
- `Reglages > Docs token` : la valeur de `API_TOKEN`.
- Avant la 1.2.2, un `/` final dans la `Docs URL` cassait la vérification de mise à jour : les notes marchaient, mais le Minitel demandait `//firmware/manifest` et affichait « Aucune version publiee ». Sur un firmware plus ancien, retire ce `/`.

## Publier un firmware

```sh
MINIOS_API_URL=https://bibliotek.example.fr MINIOS_API_TOKEN=<token> scripts/publish_firmware.sh
```

Pense à augmenter `MINIOS_FIRMWARE_VERSION` dans `src/app/version.h` avant. `MINIOS_API_URL` doit être la `Docs URL` du Minitel. Le script relit ensuite `/firmware/manifest` et s'arrête si le serveur ne renvoie pas la nouvelle version. Ensuite, sur le Minitel : `Systeme > Verifier la mise a jour`, puis `ENVOI` pour installer.

## Sauvegardes

Tout est dans le volume `/data` : des fichiers Markdown lisibles, plus un `.meta.json` par fichier pour le titre et les tags. Une sauvegarde de volume Coolify, ou un `rsync` du dossier, suffit.
