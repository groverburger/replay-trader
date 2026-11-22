// macOS renderer: Metal through ImGui's Metal backend, on a CAMetalLayer in the GLFW window. (OpenGL on macOS is
// deprecated and its swaps don't pace to ProMotion's 120 Hz; a Metal layer presents at the display's rate.)
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <Cocoa/Cocoa.h>
#define GLFW_INCLUDE_NONE
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include "imgui.h"
#include "imgui_impl_metal.h"
#include "mac_metal.h"

static id<MTLDevice> g_device;
static id<MTLCommandQueue> g_queue;
static CAMetalLayer* g_layer;
static MTLRenderPassDescriptor* g_pass;

bool metal_init(GLFWwindow* win) {
    g_device = MTLCreateSystemDefaultDevice();
    if (!g_device) return false;
    g_queue = [g_device newCommandQueue];
    if (!ImGui_ImplMetal_Init(g_device)) return false;
    NSWindow* nswin = glfwGetCocoaWindow(win);
    g_layer = [CAMetalLayer layer];
    g_layer.device = g_device;
    g_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    g_layer.displaySyncEnabled = YES;  // present on the display's refresh (vsync)
    g_layer.opaque = YES;
    nswin.contentView.layer = g_layer;
    nswin.contentView.wantsLayer = YES;
    g_pass = [MTLRenderPassDescriptor new];
    g_pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    g_pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    return true;
}

void metal_shutdown() {
    ImGui_ImplMetal_Shutdown();
    g_layer = nil, g_pass = nil, g_queue = nil, g_device = nil;
}

void metal_rebuild_fonts() {
    ImGui_ImplMetal_DestroyFontsTexture();
    ImGui_ImplMetal_CreateFontsTexture(g_device);
}

static id<CAMetalDrawable> g_drawable;

double metal_new_frame(int fb_w, int fb_h) {
    double wait = 0;
    @autoreleasepool {
        if (fb_w > 0 && fb_h > 0) {
            if (g_layer.drawableSize.width != fb_w || g_layer.drawableSize.height != fb_h) g_layer.drawableSize = CGSizeMake(fb_w, fb_h);
            CFTimeInterval t0 = CACurrentMediaTime();
            g_drawable = [g_layer nextDrawable];  // waits while all drawables are queued: paces the loop to vsync
            wait = (CACurrentMediaTime() - t0) * 1000.0;
        }
        g_pass.colorAttachments[0].texture = g_drawable ? g_drawable.texture : nil;
        if (g_drawable) ImGui_ImplMetal_NewFrame(g_pass);  // the backend reads the target's format from the pass
    }
    return wait;
}

void metal_render(float r, float g, float b) {
    @autoreleasepool {
        if (!g_drawable) return;
        id<MTLCommandBuffer> cb = [g_queue commandBuffer];
        g_pass.colorAttachments[0].clearColor = MTLClearColorMake(r, g, b, 1);
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:g_pass];
        ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), cb, enc);
        [enc endEncoding];
        [cb presentDrawable:g_drawable];
        [cb commit];
        g_drawable = nil;
    }
}
