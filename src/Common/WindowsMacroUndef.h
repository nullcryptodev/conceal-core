// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Undefine Windows min/max macros that break std::min/std::max.
#pragma once

#ifdef min
#undef min
#endif

#ifdef max
#undef max
#endif
