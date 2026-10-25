# Architecture

## Vue d’ensemble

Le firmware est organisé en couches :

- `platform` : adaptation Arduino/UART.
- `ui` : rendu bufferisé alpha/mosaïque, top bar, footer et curseur.
- `services` : Wi-Fi, NVS/settings, horloge NTP et fuseaux, client chat IA et choix du modèle, mises à jour (`ota_service`), racines TLS.
- `storage` : client de l'API Bibliotek (documents, projets, tâches, recherche) et ses parseurs testés nativement.
- `app` :
  - `app.cpp` : état global, navigation, parser clavier, explorateur, éditeur, chat, réglages ;
  - `app_shell.h` : l'API que les écrans modulaires utilisent (statut, navigation, saisie avec rappel, ouverture de document, services) ;
  - `net_requests.*` : toutes les requêtes réseau (paramètres, code exécuté sur la tâche réseau, rappel sur la boucle) ;
  - `screens/` : un fichier par écran (projets, tâches, recherche, système, modèle IA, verrou) ;
  - `workspace.*` : tâches du jour, rappels, chrono, documents récents.

## Boot

Au démarrage, rien n'attend le Wi-Fi :

1. Initialisation série debug.
2. Initialisation Minitel `1200 / 7E1`, puis liaison à 4800 bauds et aiguillages (voir « Liaison Minitel »).
3. Chargement des settings depuis NVS ; réseau Wi-Fi de `local_secrets.h` si aucun réseau persistant n’existe.
4. Chargement du chrono et des récents, démarrage de la tâche réseau.
5. Connexion au réseau mémorisé, en arrière-plan sur la tâche réseau. Le fuseau s'applique tout de suite.
6. Logo de boot (« Connexion WiFi en arriere-plan », ou « Mode hors ligne » sans réseau mémorisé), dessiné pendant la connexion.
7. Affichage de l'accueil, ou de l'écran `VERROU` si un code PIN est réglé. La top bar affiche `offline` jusqu'au lien.
8. Quand le lien monte, la boucle le voit (`app_loop`) et lance NTP, la mise à jour réseau local (ArduinoOTA, si un token API existe) et le test de l'API Bibliotek (`/health`) en arrière-plan. Réseau absent : après 15 s d'essai, le test de l'API fait afficher « WiFi hors ligne » aux écrans (tout de suite sans réseau mémorisé) et la radio continue d'essayer ; le lien monte dès que le réseau revient.

## Écrans

- `ACCUEIL` : date, tâches du jour, chrono, menu `1-9` et documents récents.
- `PROJETS` / `PROJET` : liste des projets ; fiche, tâches ouvertes, temps de la semaine et chrono d'un projet.
- `TACHES` : tâches du jour et en retard, ou toutes les tâches ouvertes.
- `RECHERCHE` : résultats de `/search`, ouverture sur la ligne trouvée.
- `SYSTEME` : santé de l'appareil, mise à jour depuis le serveur, redémarrage.
- `MODELE` : modèles de chat DigitalOcean.
- `VERROU` : code PIN au démarrage.
- `FILES` : navigation dans les sous-dossiers, documents et tags.
- `WIFI`: scan et connexion Wi-Fi.
- `SETTINGS` : API docs, configuration IA, modèle, outils de l'assistant, timezone, code PIN, reset.
- `CHAT`: conversation IA bornée en RAM avec scroll ; avec les outils, l'assistant lit et modifie les notes (voir « Assistant avec outils »).
- `EDITOR` : édition texte dense d'un document avec retour visuel, recherche simple et goto line. En sortie, on revient à l'écran qui l'a ouvert.
- `INPUT`: saisie texte pour mots de passe, métadonnées et prompts d’éditeur.

## Pipeline graphique

- Le viewport produit reste aligné sur 40 colonnes x 24 rangées (rangées 1 à 24; la rangée 0 du Minitel, ligne de service, n'est pas utilisée et `US` n'adresse pas de rangée 25).
- La couche UI maintient un buffer mosaïque par cellule avec masques 2x3 et couleur logique.
- Le flush compare le buffer à ce que le terminal affiche et n'envoie que les cellules modifiées: deux changements séparés par 2 cellules au plus sont fusionnés (moins cher qu'un `US` de 3 octets), une fin de rangée périmée est effacée par un seul `CAN` (`0x18`), et un rafraîchissement complet après `FF` n'envoie que les cellules non vides.
- Un changement d'écran repart d'une page vide (`FF`), comme au démarrage : rien de l'écran précédent ne reste. Dans un même écran, seul le diff est envoyé.
- Rien n'est envoyé quand rien n'a changé. Chaque rangée commence par un `US`, qui remet les attributs et le jeu G0 : aucune séquence de mode n'est envoyée avant. Le curseur n'est caché (`0x14`) ou montré (`0x11`) que s'il change d'état.
- Dans la boucle, le flush s'arrête entre deux rangées quand une touche attend (`minitel_ui_flush_until_input`) : elle est traitée d'abord, le flush suivant envoie le reste. Jamais deux fois de suite, pour que le bas de l'écran suive même touche maintenue. Seulement quand l'écho est coupé : avec le filtre d'écho, nos propres octets reviennent pendant le dessin.
- `HOME`, `FILES`, `WIFI` et `SETTINGS` utilisent une micro-police dérivée des glyphes bitmap.
- `INPUT`, `CHAT` et `EDITOR` utilisent un rendu texte dense en 40 colonnes avec curseur géré côté UI si nécessaire.

## Liaison Minitel

`src/ui/minitel_link.*` règle la prise péri-informatique avec les commandes Protocole (STUM 1B, chapitre 6). Chaque commande attend son acquittement (250 ms au plus) :

| Séquence | Rôle | Réponse |
|---|---|---|
| `ESC 0x39 0x74` | PRO1 STATUS VITESSE | `ESC 0x3A 0x75 <vitesse>` (`0x64` 1200, `0x76` 4800) |
| `ESC 0x3A 0x6B 0x76` | PRO2 PROG : 4800 bauds | idem, à la nouvelle vitesse |
| `ESC 0x3B 0x61/0x60 <récepteur> <émetteur>` | PRO3 aiguillage ON/OFF | `ESC 0x3B 0x63 <récepteur> <status>` |

- Vitesse : le Minitel démarre à 1200 bauds ; s'il répond à 1200 (ou à 4800, quand il a gardé notre réglage pendant un redémarrage de l'ESP32), il passe à 4800. S'il refuse, la liaison reste à 1200.
- Aiguillages : dans l'état local standard, le clavier va au modem, qui le reboucle sur l'écran (l'écho local) : chaque touche s'affichait là où le firmware était en train d'écrire, parfois au milieu d'une séquence `US`. Le firmware établit écran ← prise et prise ← clavier, puis coupe modem ← clavier, modem ← prise, écran ← modem et prise ← modem. Rien n'est coupé si les deux liens de remplacement ne sont pas confirmés par leur acquittement.
- Écho : une dernière question de vitesse vérifie que nos octets ne reviennent plus. Le filtre d'écho de `arduino_wrapper.cpp` ne reste actif que si les aiguillages n'ont pas été confirmés ou si ce test voit encore l'écho (câblage).
- Connexion/fin : une connexion ou une déconnexion remet les aiguillages standard. Les codes `SEP 0x50`-`0x5F` (dont `SEP 0x59`, la touche `CONNEXION/FIN`) déclenchent une vérification 500 ms après le dernier code ; l'affichage attend jusque-là, pour ne rien envoyer à une vitesse que le Minitel aurait quittée.
- Redémarrage du Minitel : il repart à 1200 bauds, ses octets arrivent à 4800 avec des erreurs de trame ou de parité (`onReceiveError`). La boucle repose alors la question de vitesse (au plus toutes les 2 s) ; sans réponse, la liaison est refaite et l'écran entièrement redessiné.

## Top bar

La ligne 1 est réservée à la top bar:

`ECRAN | WIFI | HH:MM`

- `ECRAN`: nom logique de l’écran courant.
- `WIFI`: SSID courant ou `offline`.
- `HH:MM`: heure locale issue de NTP, sinon `--:--`.

La ligne 2 est un séparateur. Les lignes 3 à 22 portent le contenu. La ligne 23 sert de status. La ligne 24 sert de footer.

Dans `EDITOR`:

- la ligne 3 affiche le nom du document et l’état dirty;
- les lignes 4 à 22 affichent 19 lignes visuelles repliées à 40 colonnes;
- `ANNULATION` ouvre un mini menu de commandes;
- `RETOUR` demande la sortie, avec confirmation si le document est modifié;
- `CONNEXION/FIN` sauvegarde sans quitter.

## Persistance

Les settings sont stockés en NVS:

- `wifi_ssid`
- `wifi_pass` (obfuscation XOR + hex)
- `api_url`
- `api_token` (obfuscation XOR + hex)
- `ai_url`
- `ai_key` (obfuscation XOR + hex)
- `ai_model`
- `timezone`
- `last_doc`

## Requêtes réseau

Aucune requête réseau ne bloque l'écran, ni les appels HTTP (Bibliotek, IA, mises à jour) ni le scan et la connexion Wi-Fi, celle du démarrage comprise : elles passent par une tâche FreeRTOS dédiée (`services/net_worker`, pile de 16 Ko, même cœur et même priorité que la boucle Arduino), une à la fois.

- La boucle soumet une requête (`net::submit`, `app/net_requests.h`) et reçoit son résultat dans un rappel, au `net::poll()` suivant. Elle continue de dessiner et de lire le clavier entre-temps.
- File (`services/net_queue`, testée en natif) : 4 places, une requête en vol. Les requêtes de premier plan (celles de l'utilisateur) passent avant celles d'arrière-plan (connexion Wi-Fi du démarrage, tâches du jour, `/health`) ; une seule à la fois, une seconde est refusée (« Requete en cours »). Une requête d'arrière-plan déjà en attente est fusionnée avec la nouvelle. Celle qui est en vol n'est jamais interrompue : une requête faite pendant la connexion du démarrage l'attend, 15 s au plus si le réseau est absent (« Attente du WiFi... » sur la ligne de statut).
- La ligne de statut montre la requête de premier plan de l'écran avec un compteur de secondes. `ANNULATION` l'annule, quitter son écran aussi : en attente, elle disparaît ; en vol, son résultat est ignoré. Le flux du chat, le téléchargement d'une mise à jour et l'attente d'une connexion Wi-Fi s'arrêtent dès que la tâche réseau le voit.
- Wi-Fi : le scan laisse la liste affichée jusqu'à l'arrivée de la nouvelle. Une connexion qui échoue ou qu'on annule revient au réseau enregistré s'il y en a un autre, plutôt que de laisser la radio réessayer ; sans réseau enregistré, une annulation arrête la tentative (`services/wifi_utils`, testé en natif). Le réseau n'est écrit en NVS que dans le rappel, sur la boucle, une fois connecté. Au démarrage, la connexion au réseau enregistré est une requête d'arrière-plan : silencieuse, aucun changement d'écran ne l'annule (l'écran `VERROU` non plus, et `net::poll()` tourne aussi sous le verrou), et un échec laisse la radio réessayer ce réseau. Si le lien est déjà monté quand elle part (une connexion demandée par l'utilisateur est passée avant), elle n'y touche pas.
- Partage : les paramètres d'une requête, le résultat en vol (`net_reply_t`) et un instantané des réglages passent de la boucle à la tâche réseau puis reviennent, par deux files FreeRTOS d'un élément ; seuls l'indicateur d'annulation et la progression (stade, octets, message) sont des atomiques lus pendant la requête. La tâche réseau a ses propres instances d'`ApiDocumentStore`, `ChatService`, `OtaService` et `WifiService`, et n'écrit jamais ni sur l'UART ni en NVS : l'écho du Minitel reste filtré sur la boucle.
- Ce qui reste bloquant : les mises à jour poussées depuis un ordinateur (ArduinoOTA), reçues sur la boucle par `ArduinoOTA.handle()` le temps du transfert.

## Chat IA

Le firmware utilise un `ChatService` distinct du `DocumentStore`:

- endpoint OpenAI-compatible en HTTPS;
- authentification `Bearer`;
- mémoire bornée aux 4 derniers échanges en RAM;
- réponses en streaming (`"stream": true`) : le corps (chunked si besoin), les événements SSE puis les deltas JSON sont décodés au fil de l'eau (`services/sse_parser`, `chat_service_utils::stream_*`, testés en natif) ; le texte est translittéré et ne fait que s'allonger, la boucle l'affiche à mesure ;
- une réponse qui atteint le bas de l'écran saute d'une page plutôt que de défiler ligne à ligne, ce qui renverrait toute la conversation ;
- un modèle qui ne fait que raisonner jusqu'à la limite de tokens est relancé une fois, en lui demandant la réponse seule ;
- si le serveur ignore le streaming, la réponse complète est lue d'un bloc.

### Assistant avec outils

Avec `Outils IA` actif (réglage `ai_tools`, actif par défaut) et un Docs URL/token, la question part vers `POST /assistant` de Bibliotek (`server/src/assistant.ts`) au lieu du modèle :

- le Minitel envoie ses réglages IA (URL, clé, modèle), son heure locale, les 4 derniers échanges avec leurs actions et la question. La clé ne se règle qu'au Minitel ; Bibliotek doit donc être en `https`, sinon le Minitel refuse d'envoyer la question ;
- Bibliotek fait tourner la boucle modèle → outils → modèle (8 tours au plus) sur ses fichiers : lister, lire, chercher, créer, ajouter, modifier, déplacer, supprimer, projets et tâches. La RAM et la limite de 2 Ko du Minitel ne comptent plus : un long fichier se lit par morceaux, une modification ne réécrit que le passage visé ;
- la réponse revient dans le même format que celle du modèle (chunks SSE), décodée par le même `chat_service_utils::stream_*`, avec deux événements de plus : `{"action": "lit notes/courses.md"}` pour chaque outil exécuté, affiché ` * ...` sous la question au fil de l'eau (`L'IA travaille...` sur la ligne de statut), et `{"confirm": {"id", "text"}}` ;
- une action qui remplace, modifie ou supprime un texte existant attend l'utilisateur : le flux s'arrête sur `confirm`, l'écran affiche ` ? <action>` et le pied `ENVOI oui  CORRECTION non  ANN abandon`. `ENVOI` ou `CORRECTION` envoient `POST /assistant/confirm {id, accept}` (`NET_CHAT_CONFIRM`), dont la réponse continue l'échange : la réponse et les actions reprennent après ce qui était arrivé (`stream_resume`). `ANNULATION` clôt l'échange sans l'action ; `RETOUR` laisse la question en attente (5 min côté serveur) ;
- Bibliotek envoie un commentaire SSE dès l'ouverture puis toutes les 10 s : le Minitel n'attend ni les en-têtes plus de 15 s ni un octet plus de 30 s. Quand le Minitel raccroche (`ANNULATION`, écran quitté), Bibliotek arrête l'appel au modèle et n'exécute plus d'outil ;
- pas de relance côté Minitel en mode assistant : une question relancée referait les actions. Bibliotek relance lui-même un modèle muet, et répond sans outils à un modèle qui les refuse (`modele sans outils`).

## DocumentStore

Le firmware utilise une abstraction `DocumentStore`:

- `MockDocumentStore`: stockage RAM, seed de documents de démonstration (non utilisé par l'app).
- `ApiDocumentStore`: client REST de l'API documents (`/health`, `/files`, `/folders`), utilisé par l'app.

Tous les formats sont traités comme texte brut. L’extension ne change pas l’éditeur.

Un document qui ne tient pas dans les tampons du device (contenu > 2 Ko, titre > 32 octets, plus de 4 tags ou tag > 16 octets) est marqué `truncated`: l'éditeur l'ouvre en lecture seule et `save_document` refuse de l'écrire, pour ne jamais renvoyer une copie tronquée au serveur.

## Mises à jour

- Table de partitions `min_spiffs.csv` : deux emplacements de 1,9 Mo, NVS inchangée.
- Réseau local : ArduinoOTA (`minitel-minios.local`, mot de passe = token API), `pio run -e ota -t upload`.
- Serveur : `Systeme > Verifier la mise a jour` lit `/firmware/manifest`, compare `MINIOS_FIRMWARE_VERSION`, puis télécharge `/firmware/download` dans l'autre emplacement. `Update` vérifie le MD5 avant de redémarrer.

## Clés NVS ajoutées

- `cfg_ver` : version du format des réglages. La v2 remplace l'ancien modèle par défaut une seule fois.
- `pin` : code PIN (obfuscation XOR + hex).
- `timer_proj`, `timer_start` : chrono en cours, conservé après un redémarrage.
- `recents` : 4 derniers documents ouverts.
- `ai_tools` : `0` coupe les outils de l'assistant (le chat parle alors directement au modèle), actif sinon.
