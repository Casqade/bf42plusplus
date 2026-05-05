#pragma once

#if defined(TRACY_ENABLE)
  #include <Tracy.hpp>
  #include <TracyC.h>
#endif


void attach_profiler();
