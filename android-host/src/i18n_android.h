// Android adapter for upstream dm/host/dm_i18n.h; the shared table remains upstream.
#pragma once
#include "../../d2r-mobile/dm/host/dm_i18n.h"
uint64_t dm_android_desired_language(void);
void dm_android_i18n_observe(const uint64_t *regs);
