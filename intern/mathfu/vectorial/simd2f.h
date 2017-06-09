/*
  Vectorial
  Copyright (c) 2014 Google, Inc.
  Licensed under the terms of the two-clause BSD License (see LICENSE)
*/

#ifndef VECTORIAL_SIMD2F_H
#define VECTORIAL_SIMD2F_H

#include "vectorial/config.h"

#if defined(VECTORIAL_NEON)
    #include "simd2f_neon.h"
#else
    #error No implementation defined
#endif

#include "simd2f_common.h"

#ifdef __cplusplus

    #ifdef VECTORIAL_OSTREAM
        #include <ostream>

        vectorial_inline std::ostream& operator<<(std::ostream& os, const simd2f& v) {
            os << "simd2f(" << simd2f_get_x(v) << ", "
                       << simd2f_get_y(v) << ")";
            return os;
        }
    #endif

#endif




#endif

