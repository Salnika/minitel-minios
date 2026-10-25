# Séquences Minitel utiles

Référence courte pour le Telic Alcatel 1B utilisé par ce projet.

## Affichage texte

| Séquence | Rôle |
|----------|------|
| `0x0C` | Effacer l’écran |
| `0x1F <row> <col>` | Positionner le curseur en mode Vidéotex |
| `ESC 'X'` puis `SI (0x0F)` | Forcer le mode alphanumérique |

Notes:

- `row = 0x40 + ligne`
- `col = 0x40 + colonne`
- Le firmware cible une géométrie 40x20

## Graphiques mosaïque

| Séquence | Rôle |
|----------|------|
| `SO (0x0E)` | Entrer en mosaïque |
| `SI (0x0F)` | Revenir en texte |
| `ESC 'Z'` | Mosaïque contiguë |
| `ESC 'Y'` | Mosaïque séparée |

## Divers

| Séquence | Rôle |
|----------|------|
| `ESC 0x3B 0x61 0x58 0x52` | PRO3 Aiguillage ON |
| `ESC 0x3B 0x60 0x58 0x52` | PRO3 Aiguillage OFF |

Voir aussi [KEYMAP_TELIC_1B.md](docs/KEYMAP_TELIC_1B.md) pour les entrées clavier réellement exploitées par le miniOS.
