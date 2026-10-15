/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 Gramine contributors */

/* Exceptions of the vbs PAL proper: not delivered yet (phase 3: the SEH filter of W hands
 * synchronous faults to the PAL). An unexpected fault is reported by W's filter around the boot. */

#include "api.h"
#include "pal.h"
#include "pal_internal.h"
