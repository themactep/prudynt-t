#ifndef FAAC_COMPAT_HPP
#define FAAC_COMPAT_HPP

#include <faac.h>

// faac_params_init() gained a caller_size argument in SONAME 2 so the library
// can reconcile a faac_params that has grown since the caller was built.
// Pass sizeof(*p) on 2.x, fall back to the one-argument form on 1.x.
inline faac_status faac_params_init_compat(faac_params *p) {
#if defined(FAAC_VERSION_MAJOR) && (FAAC_VERSION_MAJOR >= 2)
  return faac_params_init(p, (uint32_t)sizeof(*p));
#else
  return faac_params_init(p);
#endif
}

#endif // FAAC_COMPAT_HPP
