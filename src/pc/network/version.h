#ifndef VERSION_H
#define VERSION_H

#define SM64COOPDX_VERSION "v1.5.1"

#ifdef SMNET
/* fork de 1024 jugadores: get_version() da "v1.5.1-1024b1"; así la lista de partidas y el Join no
   mezclan este cliente con el oficial (los mods siguen viendo SM64COOPDX_VERSION "v1.5.1").
   Con cada versión publicada: subir SMNET_FORK_BUILD (lo compara el aviso de versión nueva). */
#define SMNET_FORK_BUILD 1
#define SMNET_FORK_TAG   "1024b1"
#define SMNET_FORK_LABEL "1024 beta 1"
#endif

// internal version
#define VERSION_TEXT "v"
#define VERSION_NUMBER 42
#define MINOR_VERSION_NUMBER 1

#define VERSION_OFFSET 37 // difference from old versioning system

#if defined(VERSION_JP)
#define VERSION_REGION "JP"
#elif defined(VERSION_EU)
#define VERSION_REGION "EU"
#elif defined(VERSION_SH)
#define VERSION_REGION "SH"
#else
#define VERSION_REGION "US"
#endif

#ifdef DEVELOPMENT
#define GAME_NAME "sm64coopdx-dev"
#define WINDOW_NAME "Super Mario 64 Coop Deluxe (DEV)"
#elif !defined(VERSION_US)
#define GAME_NAME "sm64coopdx-intl"
#define WINDOW_NAME "Super Mario 64 Coop Deluxe (INTL)"
#else
#define GAME_NAME "sm64coopdx"
#define WINDOW_NAME "Super Mario 64 Coop Deluxe"
#endif

#define MAX_VERSION_LENGTH 128

const char* get_version(void);
#ifdef COMPILE_TIME
const char* get_version_with_build_date(void);
#endif

#endif
