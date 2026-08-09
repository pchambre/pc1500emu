// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
#pragma once

// Forces this process to the foreground on macOS -- a no-op everywhere
// else, so main.cpp can call it unconditionally with no #ifdef at the call
// site. See mac_activate.mm for why this exists.
#if defined(__APPLE__)
void macActivateApp();
#else
inline void macActivateApp() {}
#endif
