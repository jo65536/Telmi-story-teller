# Revue adversariale batterie — Miyoo Mini / Mini Plus

Périmètre initial : commit `ef3e19bb` (`fix: stop draining the battery when a
story plays`), comparé à son parent `3ee56a6b`.

## Verdict

Le commit allait dans la bonne direction en remplaçant la boucle de polling à
vide et en éteignant l'écran pendant une histoire. Il restait toutefois plusieurs
chemins capables d'annuler le gain ou de consommer inutilement.

## Défauts importants trouvés

1. Le délai écran était réarmé à chaque stage illustré. Une histoire composée de
   stages courts pouvait donc garder l'écran allumé indéfiniment. Un appui POWER
   manuel était également annulé par le stage suivant.
2. `Mix_PlayMusic(music, 1)` joue une piste puis une répétition supplémentaire.
   La valeur correcte pour une lecture unique est `0`, conformément à la
   [documentation SDL_mixer](https://wiki.libsdl.org/SDL2_mixer/Mix_PlayMusic).
3. Sur Mini Plus, `batmon` lançait le binaire vendeur `axp_test` chaque seconde,
   plus une seconde fois toutes les 15 secondes : environ 3 840 processus par
   heure.
4. Même après le commit, la boucle principale se réveillait 20 fois par seconde
   pendant toute lecture autoplay pour détecter la fin d'une piste.
5. `file_write()` interprétait mal le résultat d'`open()`, pouvait écrire sur un
   descripteur invalide et omettait de fermer le FD après une erreur.
6. Un thread de calcul de durée terminé n'était plus joint avant d'être remplacé.
   Le handle pthread fuyait à chaque piste et `musicDuration` était lu sans
   synchronisation sur ARM 32 bits.
7. `chargingState` pouvait lancer `axp_test` environ 67 fois par seconde pendant
   l'animation de charge et utilisait les fonctions framebuffer sans
   `display_init()`.
8. Les arbres cJSON et les buffers retournés par `file_read()` étaient souvent
   libérés avec la mauvaise primitive, ou pas libérés.

## Corrections intégrées

- Un seul budget écran par segment autonome, extinction POWER persistante entre
  les stages, overlays limités à cinq secondes et aucun réveil pour un écran
  noir sans contenu.
- Extinction du GPIO et du PWM, transitions idempotentes, nouvelle tentative si
  une écriture matérielle échoue et restauration framebuffer avant le réveil.
- Notification de fin audio SDL vers le thread principal par self-pipe. Le
  callback ne fait qu'un `write()` non bloquant, car SDL peut l'appeler depuis
  un thread audio arbitraire
  ([contrat SDL_mixer](https://wiki.libsdl.org/SDL2_mixer/Mix_HookMusicFinished)).
- Lecture audio unique (`loops=0`), gestion correcte des joins et mutex.
- Mini Plus : un seul `axp_test` par minute, soit environ 60 par heure. Mini :
  lecture GPIO/ADC maintenue toutes les 15 secondes.
- Animation de charge : contrôle à 1 Hz.
- Logger opt-in à 60 secondes sous `/tmp`, puis une seule copie atomique vers la
  SD à l'arrêt. Aucun appel additionnel à `axp_test`.
- Build release en `-Os`, sections séparées et `--gc-sections`; dépendances
  Make automatiques et nettoyées.
- Nettoyage des FD, buffers fichier et arbres cJSON.

## Changements volontairement différés

- Wi-Fi : la valeur JSON ne prouve pas que `wlan0`, `wpa_supplicant` ou `udhcpc`
  sont réellement arrêtés sur chaque firmware.
- Gouverneur CPU : `powersave` peut réduire la consommation, mais doit être testé
  contre les coupures audio avant activation pendant les histoires.
- `FBIOBLANK` et réglages PMIC : aucun déploiement sans preuve v2/v4.
- Calcul de durée MP3 : il ouvre encore une seconde fois la piste. Le rendre
  paresseux peut créer un délai au changement de piste si le calcul démarre
  tard ; mesurer d'abord son coût CPU/SD.

## Validation effectuée

- Compilation/link ARM EABI5 des cinq binaires avec le toolchain Miyoo.
- `storyTeller` : 101 908 à 52 648 octets ; `batmon` : 26 368 à 9 768 octets.
- Vérifications syntaxiques hôte Mini/Mini Plus.
- Syntaxe BusyBox et `shellcheck` du logger.
- Test du CSV : 24 colonnes, arrêt propre, copie atomique et propagation d'un
  échec de persistance.
- `git diff --check` sans erreur.

Le test historique `make test` reste cassé avant ces changements : son Makefile
référence l'ancien composant `infoPanel`, absent du dépôt. Le test autonome
ajouté se lance avec `make test-powerlog`.

La validation GPIO/PWM, la continuité audio et le gain énergétique réel doivent
encore être mesurés sur les deux consoles selon `docs/power-ab-testing.md`.
