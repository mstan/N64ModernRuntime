// N64ModernRuntime — compatibility shim (mstan fork).
//
// The libultra-call ring moved from librecomp to ultramodern
// (2026-06) so the runtime's own event-delivery paths (VI retrace,
// AI, SP/DP/SI done, timer fires) can record into the same ring
// without an inverted librecomp include. Existing consumers that
// include <librecomp/ultra_trace.hpp> keep working through this
// shim; new code should include <ultramodern/ultra_trace.hpp>.
//
// Copyright (c) 2026 Matthew Stanley — GPL-3.0 (see COPYING).

#ifndef LIBRECOMP_ULTRA_TRACE_HPP
#define LIBRECOMP_ULTRA_TRACE_HPP

#include "ultramodern/ultra_trace.hpp"

#endif /* LIBRECOMP_ULTRA_TRACE_HPP */
