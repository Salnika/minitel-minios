# Contrat d'API (Bibliotek)

Le firmware parle à **Bibliotek**, dans `../server/`. Son README décrit toutes les routes. Ce fichier ne reprend que ce que le Minitel utilise.

## Authentification

- `Authorization: Bearer <API_TOKEN>` sur toutes les routes sauf `GET /health`.
- 401 : token absent. 403 : token invalide.

## Notes

| Usage sur le Minitel | Route |
|---|---|
| Test de connexion au démarrage | `GET /health` |
| Explorateur : dossiers | `GET /folders`, `POST /folders {name}` |
| Explorateur : documents d'un dossier | `GET /files?folder=<dossier>` |
| Ouvrir / enregistrer | `GET /file?id=<dossier>/<nom>`, `PUT /file?id=... {content, title, tags, sortOrder}` |
| Créer un document | `POST /files {folder, name, title, content}` |
| Capture rapide, journal, temps | `POST /files/append {id, text}` |
| Recherche | `GET /search?q=&limit=` |

L'identifiant d'un document passe dans le paramètre de requête `id`, pas dans le chemin : un `/` encodé (`%2F`) peut être refusé par le proxy de Coolify.

Un contenu vide est accepté.

## Projets et tâches

| Usage sur le Minitel | Route |
|---|---|
| Liste des projets | `GET /projects?today=YYYY-MM-DD` |
| Nouveau projet | `POST /projects {title}` → `projets/<slug>/projet.md` + `taches.md` |
| Tâches d'un projet, du jour, ou toutes | `GET /tasks?project=&due=today&today=&limit=` |
| Ajouter une tâche | `POST /tasks {project, text, due?, time?, priority?}` |
| Cocher / décocher | `POST /tasks/toggle {file, line, done}`, avec une 409 si la ligne a changé entre-temps |

Le Minitel envoie sa propre date locale (`today`). « Aujourd'hui » et « en retard » suivent donc son fuseau horaire.

Conventions des fichiers :

```md
projets/robot/projet.md   # Titre, puis "Statut:", "Objectif:", "Prochaine etape:"
projets/robot/taches.md   - [ ] Commander les pieces !! @2026-10-25 14:30
projets/robot/temps.md    - 2026-10-20 14:00 +90 min
inbox/inbox.md            - 2026-10-20 14:05 idee capturee
journal/2026-10-20.md     ## 14:05
```

## Assistant

| Usage sur le Minitel | Route |
|---|---|
| Question à l'assistant avec outils | `POST /assistant {ai: {baseUrl, apiKey, model}, lang?, now?, history?, prompt}` |
| Réponse à une confirmation | `POST /assistant/confirm {id, accept}`, 404 si elle a expiré (5 min) |

- `ai` : les réglages IA du Minitel ; `baseUrl` doit être en `https`. `lang` : la langue du firmware, `fr` ou `en` (français si absent ou inconnu). `now` : son heure locale `AAAA-MM-JJ HH:MM`. `history` : ses derniers échanges `{user, assistant, actions?}`, les actions une par ligne.
- Les deux routes répondent en `text/event-stream` : les chunks d'une chat completion (`choices[0].delta.content`, `reasoning_content`, `finish_reason`), plus `{"action": "lit notes/courses.md"}` pour chaque outil exécuté et `{"error": {"message"}}` en cas d'échec du modèle (message court). Réponse, actions, confirmations et erreurs suivent `lang`, que la confirmation garde.
- Une action qui remplace, modifie ou supprime du texte envoie `{"confirm": {"id", "text"}}` puis termine le flux ; `POST /assistant/confirm` reprend la conversation dans sa propre réponse.
- Un commentaire SSE part dès l'ouverture du flux, puis toutes les 10 s.

## Mises à jour

| Route | Rôle |
|---|---|
| `GET /firmware/manifest` | `{version, size, md5, publishedAt}`, 404 si rien n'est publié |
| `GET /firmware/download` | le binaire |
| `PUT /firmware?version=1.2.0` | publication (corps `application/octet-stream`) |

## Limites côté appareil

- Un document au-delà de 2 Ko (ou dont le titre fait plus de 32 octets, ou qui a plus de 4 tags) s'ouvre en lecture seule, pour ne jamais écraser le serveur avec une copie tronquée.
- Les listes sont bornées : 24 projets, 30 tâches, 20 résultats de recherche.
