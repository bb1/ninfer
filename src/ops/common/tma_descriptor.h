#pragma once

// Alignment for structs of CUtensorMap kernel parameters. TMA requires 64-byte descriptor
// addresses. cuda.h aligns CUtensorMap to 128 bytes only when __cplusplus reports C++11, which
// MSVC does not by default, and MSVC cannot pass kernel parameters aligned beyond 64 bytes by
// value (C2719). Elsewhere the CUtensorMap members already carry their declared alignment.
#if defined(_MSC_VER)
#define NINFER_TMA_DESCRIPTORS alignas(64)
#else
#define NINFER_TMA_DESCRIPTORS
#endif
