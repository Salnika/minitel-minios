# TEXT STORAGE API — SPECIFICATIONS (V1)

## 1. OBJECTIF

Créer une API REST ultra simple permettant de :

- stocker des fichiers texte
- lister et naviguer dans des dossiers
- lire, créer, modifier et supprimer des fichiers
- trier les fichiers
- sécuriser l’accès via un token

Le système est conçu pour être utilisé par un client très limité (ESP32 + Minitel).

---

## 2. CONTRAINTES

- Simplicité maximale (pas de sur-ingénierie)
- Aucune dépendance à une base de données en V1
- Stockage basé sur le filesystem
- API synchrone et stateless
- Authentification par token unique
- Compatible Docker / Coolify
- Tolérant aux erreurs côté client (écriture complète à chaque save)
- On doit pouvoir supporter plusieurs extensions, txt, json, md et autre (mais que du texte)

---

## 3. STACK TECHNIQUE

- vp (client vite-plus, tape vp help pour avoir la liste des commandes)
- Framework HTTP : Fastify (recommandé)
- TypeScript
- Docker
- Le projet est deja scaffold

---

## 4. STRUCTURE DU STOCKAGE

### 4.1 Dossier racine

```txt
/data/files/
```

### 4.2 Organisation

```txt
/data/files/
  notes/
    file1.txt
    file1.meta.json
  journal/
    2026-03-24.txt
  drafts/
    draft1.txt
```

### 4.3 Règles

- Tous les fichiers doivent être dans `/data/files`
- Aucun accès en dehors de ce dossier
- Interdire :
  - chemins absolus
  - `..`
  - caractères dangereux

---

## 5. FORMAT DES FICHIERS

### 5.1 Contenu

- fichiers `.txt` uniquement
- encodage UTF-8
- contenu brut

### 5.2 Métadonnées (optionnelles)

Fichier `.meta.json` associé :

```json
{
  "title": "Titre du fichier",
  "tags": ["tag1", "tag2"],
  "sortOrder": 10
}
```

Champs optionnels :

- `title` (string)
- `tags` (array)
- `sortOrder` (number)

---

## 6. AUTHENTIFICATION

### 6.1 Méthode

Header HTTP obligatoire :

```http
Authorization: Bearer <API_TOKEN>
```

### 6.2 Configuration

Variable d’environnement :

```env
API_TOKEN=supersecrettoken
```

### 6.3 Règles

- Si absent → 401
- Si incorrect → 403

---

## 7. API

### 7.1 Health check

#### GET `/health`

Réponse :

```json
{ "status": "ok" }
```

---

### 7.2 Dossiers

#### GET `/folders`

Retourne la liste des dossiers

```json
["notes", "journal", "drafts"]
```

---

#### POST `/folders`

Créer un dossier

```json
{
  "name": "nouveau-dossier"
}
```

---

### 7.3 Fichiers

#### GET `/files`

Query params :

- `folder` (optionnel)
- `sort` = `name | updatedAt | size | manual`
- `order` = `asc | desc`

Exemple :

```http
GET /files?folder=notes&sort=updatedAt&order=desc
```

Réponse :

```json
[
  {
    "id": "notes/file1.txt",
    "name": "file1.txt",
    "title": "Titre",
    "folder": "notes",
    "updatedAt": "2026-03-24T18:42:00Z",
    "size": 1200
  }
]
```

---

#### GET `/files/:id`

Retourne un fichier

```json
{
  "id": "notes/file1.txt",
  "name": "file1.txt",
  "title": "Titre",
  "folder": "notes",
  "content": "Contenu du fichier",
  "updatedAt": "2026-03-24T18:42:00Z"
}
```

---

#### POST `/files`

Créer un fichier

```json
{
  "folder": "notes",
  "name": "file1.txt",
  "content": "Texte initial",
  "title": "Titre optionnel"
}
```

---

#### PUT `/files/:id`

Met à jour complètement le contenu

```json
{
  "content": "Nouveau contenu"
}
```

⚠️ Remplacement complet du fichier (pas de patch)

---

#### DELETE `/files/:id`

Supprime le fichier + meta

---

#### POST `/files/:id/rename`

```json
{
  "newName": "nouveau-nom.txt"
}
```

---

#### POST `/files/:id/move`

```json
{
  "targetFolder": "journal"
}
```

---

## 8. IDENTIFIANT DE FICHIER

Format :

```txt
<folder>/<filename>.txt
```

Exemple :

```txt
notes/file1.txt
```

Règles :

- pas de slash en début
- pas de `..`
- validation stricte obligatoire

---

## 9. TRI

### 9.1 Modes

- `name`
- `updatedAt`
- `size`
- `manual`

### 9.2 Tri manuel

Utilise `sortOrder` dans `.meta.json`

Si absent → fallback sur nom

---

## 10. SÉCURITÉ

Obligatoire :

- validation stricte des chemins
- sandbox dans `/data/files`
- empêcher path traversal
- limiter taille fichier (ex: 1MB configurable)
- timeout sur requêtes longues

---

## 11. ERREURS

Format standard :

```json
{
  "error": "message"
}
```

Codes :

- 400 → requête invalide
- 401 → non authentifié
- 403 → token invalide
- 404 → fichier introuvable
- 500 → erreur serveur

---

## 12. DOCKER

### 12.1 Volume obligatoire

```yaml
volumes:
  - ./data:/data
```

### 12.2 Variables

```env
API_TOKEN=supersecrettoken
DATA_PATH=/data/files
PORT=3000
```

---

## 13. COMPORTEMENT CLIENT (MINITEL)

- navigation par dossier
- liste paginée si nécessaire
- édition complète du texte
- sauvegarde = envoi complet du contenu
- aucune logique complexe côté client

---

## 14. EXTENSIONS FUTURES (NON V1)

À ne pas implémenter maintenant :

- base SQLite
- recherche full-text
- multi-utilisateurs
- permissions avancées
- versioning
- historique

---

## 15. CRITÈRES DE SUCCÈS

- API fonctionnelle en moins de 1 seconde par requête
- aucune perte de données
- fichiers accessibles directement sur disque
- code simple, lisible, maintenable
- déployable en 1 commande via Docker

---

## 16. PRIORITÉS

1. sécurité filesystem
2. simplicité API
3. robustesse écriture/lecture
4. compatibilité client minimal
5. performance suffisante (pas d’optimisation prématurée)
