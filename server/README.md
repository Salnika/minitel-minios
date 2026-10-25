# Bibliotek API

API REST minimaliste pour stocker des fichiers texte `.txt` et `.md` sur le filesystem, avec métadonnées optionnelles via sidecars `.meta.json`. C'est le serveur du miniOS Minitel : elle vit dans le dossier `server/` du dépôt `minitel-minios`.

## Lancement local

1. Copier `.env.example` vers `.env` et définir `API_TOKEN`.
2. Installer les dépendances avec `vp install`.
3. Démarrer le serveur avec `vp run dev`.

Le service écoute par défaut sur `http://localhost:3000`.

Variables d’environnement prises en charge :

- `API_TOKEN` obligatoire
- `DATA_PATH` défaut `./data/files`
- `PORT` défaut `3000`
- `MAX_FILE_SIZE_BYTES` défaut `1048576`
- `REQUEST_TIMEOUT_MS` défaut `10000`

`GET /health` est public. Toutes les autres routes exigent `Authorization: Bearer <API_TOKEN>`.

## Validation

- `vp check`
- `vp test`
- `vp run build`

## Bruno

Une collection Bruno est disponible dans `./bruno`.

1. Ouvrir le dossier `bruno` dans Bruno.
2. Sélectionner l’environnement `Local` si tu veux surcharger les valeurs par défaut.
3. Sinon, la collection a déjà des variables par défaut dans `./bruno/collection.bru`.
4. Ajuster `bibliotekBaseUrl` et `bibliotekApiToken` dans `./bruno/environments/Local.bru` ou `./bruno/collection.bru` si besoin.

Les requêtes couvrent le workflow principal de l’API : health check, dossiers, création/lecture/mise à jour/rename/move/suppression de fichiers.

## Docker

Construire l’image :

```sh
docker build -t bibliotek .
```

Lancer le conteneur avec un volume monté sur `/data` :

```sh
docker run \
  --rm \
  -p 3000:3000 \
  -e API_TOKEN=supersecrettoken \
  -e DATA_PATH=/data/files \
  -v "$(pwd)/data:/data" \
  bibliotek
```

## Déploiement Coolify

1. Dans Coolify : **+ New → Application**, choisis ce dépôt Git (`minitel-minios`, ou ton fork) et la branche `main`.
2. **Build Pack** : `Dockerfile`, **Base Directory** : `/server`, **Port exposé** : `3000`. Les **Watch Paths** `server/**` évitent un redéploiement à chaque commit du firmware.
3. **Variables d'environnement** :
   - `API_TOKEN` (secret) : le même que le « Docs token » réglé sur le Minitel.
   - optionnel : `TZ=Europe/Paris`, `MAX_FILE_SIZE_BYTES`, `FIRMWARE_MAX_BYTES` (4 Mo par défaut).
4. **Stockage persistant** : un volume monté sur `/data`. Les notes vont dans `/data/files`, le firmware dans `/data/firmware`. Sans ce volume, tout est perdu au redéploiement.
5. **Domaine** : par exemple `https://bibliotek.ton-domaine.fr`. Coolify génère le certificat Let's Encrypt, que le Minitel reconnaît (ISRG Root X1/X2).
6. **Santé** : l'image déclare un `HEALTHCHECK` Docker sur `GET /health`. Le health check propre à Coolify a besoin de `curl` ou `wget` dans l'image, absents de `node:slim` : laisse-le désactivé.
7. Sur le Minitel, dans **Réglages** : `Docs URL` = l'URL **https** du domaine, `Docs token` = `API_TOKEN`.

## Extensions pour le Minitel

Ces routes servent le gestionnaire de projets du miniOS. Elles exigent toutes le token.

| Route                                                                         | Rôle                                                                                                                                   |
| ----------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------- |
| `GET /projects?today=YYYY-MM-DD`                                              | Projets de `projets/` : titre, statut, objectif, prochaine étape, tâches ouvertes et en retard, minutes de la semaine                  |
| `POST /projects` `{title, goal?}`                                             | Crée `projets/<slug>/` avec `projet.md` et `taches.md`                                                                                 |
| `GET /tasks?project=&status=open\|done\|all&due=today\|overdue&today=&limit=` | Tâches triées : en retard d'abord, puis par échéance et priorité                                                                       |
| `POST /tasks` `{project, text, due?, time?, priority?}`                       | Ajoute une ligne à `projets/<project>/taches.md`                                                                                       |
| `POST /tasks/toggle` `{file, line, done?}`                                    | Coche ou décoche une tâche (409 si la ligne a changé entre-temps)                                                                      |
| `POST /files/append` `{id, text}`                                             | Ajoute une ligne à un fichier en créant dossiers et fichier si besoin (boîte de réception, journal, temps)                             |
| `GET /search?q=&folder=&limit=`                                               | Recherche insensible à la casse et aux accents                                                                                         |
| `GET` / `PUT` / `DELETE /file?id=`                                            | Comme `/files/:folder/:name`, mais l'id passe en paramètre de requête : pas de `/` encodé dans le chemin, que certains proxys refusent |
| `GET /firmware/manifest`, `GET /firmware/download`                            | Mise à jour du Minitel par le réseau                                                                                                   |
| `PUT /firmware?version=1.2.0` (binaire)                                       | Publie un firmware (calcule taille et MD5)                                                                                             |
| `POST /assistant` `{ai: {baseUrl, apiKey, model}, now?, history?, prompt}`    | Assistant avec outils : fait tourner le modèle sur les notes et diffuse sa réponse en SSE (voir plus bas)                              |
| `POST /assistant/confirm` `{id, accept}`                                      | Réponse de l'utilisateur à une action en attente ; la conversation continue dans cette réponse, 404 après 5 min                        |

Les contenus vides sont acceptés (une note vidée peut être enregistrée).

### Conventions des fichiers

Tout reste du Markdown lisible et modifiable à la main. Voici le contenu de chaque fichier (sans l'indentation) :

```text
projets/robot/projet.md
  # Robot Minitel
  Statut: en cours
  Objectif: Afficher l'heure
  Prochaine etape: Souder le connecteur

projets/robot/taches.md
  - [ ] Commander les pieces !! @2026-10-25 14:30
  - [x] Souder le connecteur

projets/robot/temps.md
  - 2026-10-20 14:00 +90 min Soudure
```

- Les tâches sont des cases `- [ ]` dans n'importe quel fichier sous `projets/`.
- `!` à `!!!` donne la priorité, `@AAAA-MM-JJ` l'échéance, suivie éventuellement d'une heure `HH:MM`.

### Assistant

`src/assistant.ts` fait tourner un modèle compatible OpenAI (chat completions avec `tools`, par défaut DigitalOcean Gradient) sur les fichiers, avec les outils de `src/assistant-tools.ts` : lister un dossier, lire (par morceaux pour un long fichier), chercher, créer ou remplacer, ajouter à la fin, modifier un passage, créer un dossier, déplacer, supprimer, lister les projets et les tâches, créer un projet, ajouter et cocher une tâche.

- La clé et le modèle viennent du Minitel avec chaque question (`ai`) : rien à configurer ici, mais Bibliotek doit être servi en `https`.
- Une action qui remplace, modifie ou supprime du texte existant attend l'accord de l'utilisateur : le flux se termine sur `{"confirm": {"id", "text"}}`, et `POST /assistant/confirm` donne la réponse. Les autres actions (créer, ajouter, déplacer, cocher) s'exécutent directement.
- La réponse suit le format des chunks d'une chat completion, plus `{"action": "..."}` à chaque outil exécuté. Un commentaire SSE part à l'ouverture puis toutes les 10 s ; quand le client raccroche, l'appel au modèle s'arrête et plus aucun outil ne tourne.
- 8 tours d'outils au plus par question. Un modèle qui refuse `tools` (400) répond sans outils ; un modèle qui ne fait que réfléchir est relancé une fois pour sa réponse.

### Publier un firmware

```sh
curl -X PUT "https://bibliotek.ton-domaine.fr/firmware?version=1.2.0" \
  -H "Authorization: Bearer $API_TOKEN" \
  -H "Content-Type: application/octet-stream" \
  --data-binary @firmware.bin
```

Le script `../scripts/publish_firmware.sh` (projet `minitel-minios`) fait la compilation et l'envoi.
