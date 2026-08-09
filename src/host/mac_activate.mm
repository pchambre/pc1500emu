// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
//
// Launching pc1500emu as a plain Unix executable from a terminal (rather
// than as a proper .app bundle, which this project deliberately isn't --
// see mac_activate.h's own comment / the README's macOS note) means macOS
// doesn't automatically grant it normal foreground-app treatment the way
// Finder/LaunchServices would for a bundled app: its window can open
// behind whatever's currently focused and rank oddly in Cmd+Tab/Mission
// Control ordering. activateIgnoringOtherApps: is the standard fix --
// what a bundled app effectively gets "for free" from the Dock at launch.
//
// Call this after SDL_Init(SDL_INIT_VIDEO) (SDL's own macOS video backend
// is what sets up the shared NSApplication instance NSApp refers to).
#import <Cocoa/Cocoa.h>

void macActivateApp() {
  [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
  [NSApp activateIgnoringOtherApps:YES];
}
