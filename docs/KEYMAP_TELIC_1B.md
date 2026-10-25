# Keymap Telic Alcatel 1B

Le miniOS exploite les touches suivantes:

## Navigation

- flèche gauche: `0x08`
- flèche droite: `0x09`
- flèche bas: `0x0A`
- flèche haut: `0x0B`

## Fonctions Telic

- `ENVOI`: `^S A`
- `RETOUR`: `^S B`
- `REPETITION`: `^S C`
- `GUIDE`: `^S D`
- `ANNULATION`: `^S E`
- `SOMMAIRE`: `^S F`
- `CORRECTION`: `^S G`
- `SUITE`: `^S H`
- `CONNEXION/FIN`: `^S Y`

## Mapping firmware

- `REPETITION`: haut
- `SUITE`: bas
- `SOMMAIRE`: gauche
- `GUIDE`: droite
- `ENVOI`: confirmer / nouvelle ligne
- `RETOUR`: revenir / quitter l’éditeur si propre
- `CORRECTION`: effacer le caractère précédent
- `ANNULATION`: ouvrir le menu de commandes de l’éditeur
- `CONNEXION/FIN`: sauvegarder sans quitter

## Raccourcis éditeur

- `ANNULATION` ouvre le menu `Recherche / Aller ligne / Quitter / Redessiner`
- dans ce menu: `REPETITION` et `SUITE` naviguent, `ENVOI` valide
- `RETOUR` dans l’éditeur demande la sortie; si le document est dirty, `ENVOI` confirme et `RETOUR`/`ANNULATION` annulent

## Caractères spéciaux gérés

- `^Y A` => `` ` ``
- `^Y B` => `'`
- `^Y C` => `^`
- `^Y H` => `"`
