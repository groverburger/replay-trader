// macOS: tells trackpad scrolling (precise pixel deltas) from mouse-wheel clicks. GLFW passes both to the scroll
// callback as plain numbers; a local event monitor sees each scroll event first and records which kind it is.
#import <Cocoa/Cocoa.h>

#include <stdbool.h>

static bool g_precise = false;

void mac_input_init() {
    [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskScrollWheel
                                          handler:^NSEvent*(NSEvent* e) {
                                              g_precise = e.hasPreciseScrollingDeltas;
                                              return e;
                                          }];
}

bool mac_scroll_precise() { return g_precise; }
