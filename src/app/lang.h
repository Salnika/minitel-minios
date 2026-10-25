#ifndef APP_LANG_H
#define APP_LANG_H

// Language of the on-screen texts, chosen at build time: French by default, English with
// -DMINIOS_LANG_EN (environments esp32dev_en, ota_en and native_en). Each text is written
// TR("francais", "English") where it is used, so both stay side by side and only one is
// compiled in. Both must take the same printf arguments and fit the same width.
// Data never changes with the language: file and folder names (projets/, taches.md),
// project fields, the task syntax, JSON fields and NVS keys.
#if defined(MINIOS_LANG_EN)
#define TR(fr, en) en
#define MINIOS_LANG "en"
#else
#define TR(fr, en) fr
#define MINIOS_LANG "fr"
#endif

#endif
