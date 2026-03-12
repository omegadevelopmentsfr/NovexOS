/*
 * NovexOS - lang.c
 * Simple Localization System (French/English)
 */

#include "lang.h"
#include "string.h"

static lang_id_t current_lang = LANG_EN;

/* English strings */
static const char *strings_en[STR_MAX] = {
    "\n=== Boot Complete ===\n",
    "Press 1 for QWERTY, 2 for AZERTY\n",
    "Select Display: 1) Normal (1024x768) 2) HD (1280x720) 3) FHD "
    "(1920x1080)\n",
    "NovexOS",
    "Terminal",
    "Restart",
    "Shutdown",
    "PIT OK\n",
    "Keyboard OK\n",
    "Mouse OK\n"};

/* French strings */
static const char *strings_fr[STR_MAX] = {
    "\n=== Demarrage Termine ===\n",
    "Appuyez sur 1 pour QWERTY, 2 pour AZERTY\n",
    "Selectionnez l'ecran: 1) Normal (1024x768) 2) HD (1280x720) 3) FHD "
    "(1920x1080)\n",
    "Menu",
    "Console",
    "Redemarrer",
    "Eteindre",
    "PIT OK\n",
    "Clavier OK\n",
    "Souris OK\n"};

void lang_set(lang_id_t lang) {
  if (lang == LANG_EN || lang == LANG_FR) {
    current_lang = lang;
  }
}

lang_id_t lang_get(void) { return current_lang; }

const char *get_string(string_id_t id) {
  if (id >= STR_MAX)
    return "";

  if (current_lang == LANG_FR) {
    return strings_fr[id];
  } else {
    return strings_en[id];
  }
}
