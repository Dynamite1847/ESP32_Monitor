#pragma once

#include "app_model/app_model.h"

#ifdef __cplusplus
extern "C" {
#endif

void desk_ui_init(const desk_app_state_t *initial_state);
void desk_ui_apply_state(const desk_app_state_t *state);
void desk_ui_show_home(void);

#ifdef __cplusplus
}
#endif
