#include "audcore_fx.h"
#include "audcore_fx_editor.h"
extern "C" AUDCORE_FX_EXPORT const AudcoreFxEditor* audcore_fx_get_editor(uint32_t) { return nullptr; }
extern "C" AUDCORE_FX_EXPORT uint32_t audcore_fx_editor_flags(uint32_t) { return 0; }
