#ifndef LANGUAGE_MANAGER_H
#define LANGUAGE_MANAGER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Callback type: called when language changes. en == true => English, false => Chinese */
typedef void (*language_change_cb_t)(bool en, void *ctx);

/* Initialize language manager (call once at startup). */
void language_init(bool default_en);

/* Get / set language */
bool language_get_en(void);
void language_set_en(bool en);
void language_toggle(void);

/* Register/unregister change callback.
   Returns positive id (>=1) on success, or -1 on failure.
*/
int language_register_callback(language_change_cb_t cb, void *ctx);
void language_unregister_callback(int id);

#ifdef __cplusplus
}
#endif

#endif /* LANGUAGE_MANAGER_H */
