/* ps4_compat.h - incluido a forca (-include) em todos os fontes da engine.
   Cobre o que o codigo Windows/Mac espera e o toolchain OpenOrbis nao puxa sozinho. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <sys/time.h>
#include <unistd.h>
#ifndef RBX_PLATFORM_PS4
#define RBX_PLATFORM_PS4 1
#endif
