# Protocole A/B batterie Miyoo Mini / Mini Plus

Deux paquets prêts à extraire à la racine des cartes SD sont disponibles :

| Variante | Paquet | SHA-256 |
| --- | --- | --- |
| témoin `3ee56a6b` + télémétrie uniquement | `release/TelmiOS_v1.10.1-baseline-telemetry-update.zip` | `70d7332396cf5ebdb52af803d135577da8adb2984239a558cf291d1fc9ddceaf` |
| optimisée + même télémétrie | `release/TelmiOS_v1.10.1-battery-optimized-telemetry-update.zip` | `6fcf3cdf4df2a8a88aa7f2b35992357abb25b64539d9fe5b4db98cbe7e954a25` |

La baseline ne contient aucune correction énergie : seuls le logger, ses appels
dans `runtime.sh` et la publication de l'état écran dans `/tmp` ont été ajoutés.
Faire une sauvegarde des cartes avant de remplacer les fichiers système.

Le logger est désactivé par défaut. Quand il est activé, il échantillonne toutes
les 60 secondes sous `/tmp` et ne copie le CSV sur la carte SD qu'une fois, de
façon atomique, à l'arrêt propre de `storyTeller`. Le type de filesystem qui
porte `/tmp` est enregistré dans l'en-tête du CSV : le vérifier sur les deux
firmwares plutôt que de supposer qu'il s'agit d'un `tmpfs`.

## Activer une console

Sur chaque carte SD :

1. Créer `Saves/.powerTelemetry` avec le nom de la variante : `baseline` ou
   `patched`.
2. Créer `Saves/.powerDevice` avec un identifiant stable de la console, par
   exemple `miyoo-a` ou `miyoo-b`.
3. Facultatif : créer `Saves/.powerTelemetryInterval` avec une période comprise
   entre 30 et 3600 secondes. Garder `60` pour les comparaisons.

Les résultats sont écrits à l'arrêt dans
`Saves/Diagnostics/power/power-<console>-<variante>-*.csv`.

Le CSV ne contient aucun titre, chemin ou nom d'histoire. Il mesure les ticks CPU
de `storyTeller` et `batmon`, les créations de processus, les changements de
contexte, la fréquence/gouverneur CPU, la charge, l'état écran/PWM et les caches
batterie déjà produits sous `/tmp`. Le logger ne relance jamais `axp_test`.
Un échec de copie est signalé dans `Saves/telmi-power-flush-error.txt`; le journal
RAM reste alors dans `/tmp/telmi-power.csv` tant que la console ne s'éteint pas.

Après la campagne, supprimer `Saves/.powerTelemetry`. Supprimer également les
anciens CSV manuellement : la rétention n'est volontairement pas automatisée
pour ne jamais effacer des résultats sans décision de l'opérateur.

## Éviter une fausse conclusion

Deux consoles différentes ne constituent pas à elles seules un A/B fiable :
capacité et âge de batterie, écran, PMIC et révision matérielle peuvent expliquer
un écart. Faire un test croisé :

| Session | Console A | Console B |
| --- | --- | --- |
| 1 | patched | baseline |
| 2 | baseline | patched |

Répéter idéalement chaque session trois fois. Utiliser la même histoire, le même
volume, la même luminosité, la même température ambiante et le même état de
charge initial. Ne pas tester pendant la charge. Inverser l'ordre réduit aussi
l'effet du vieillissement et des conditions du jour.

Les deux paquets ci-dessus contiennent la même version de `powerlog.sh`. La jauge
Mini Plus de la variante optimisée est rafraîchie au plus toutes les 60 secondes :
ne pas interpréter une valeur isolée comme une mesure instantanée.

## Indicateurs principaux

- pente de tension ou de pourcentage par heure ;
- ticks CPU `storyTeller` par seconde ;
- ticks CPU `batmon` et créations de processus par seconde ;
- proportion des échantillons avec écran et PWM éteints ;
- fréquence CPU et charge globale ;
- absence de répétition des pistes et de silence entre deux pistes.

Pour calculer des taux, utiliser les deltas d'`uptime_s` entre lignes plutôt que
la période théorique : la dernière ligne est volontairement écrite au moment de
l'arrêt. Ne jamais soustraire les ticks d'un processus si son PID a changé.

Avant d'ajouter une politique Wi-Fi ou un gouverneur CPU, relever aussi sur les
deux révisions l'état de `wlan0`, `wpa_supplicant`, `udhcpc`, les gouverneurs
disponibles et `time_in_state`. Ces éléments dépendent du firmware installé et
ne sont pas prouvés par la configuration du dépôt.
