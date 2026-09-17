// [2026-09-04 NATIVE UI COMPOSITE] Fullscreen triangle for the editor-UI overlay.
//
// Native Vulkan presentation puts the swapchain in a NATIVE child window, which composites above
// every ordinary Qt sibling and therefore hid the whole tool palette. Making the Qt widgets native
// so they sit on top instead turned the viewport black (a QOpenGLWidget with native children stops
// compositing its children the way this needs, and native mode gates off every GL pass). The way
// out is to stop fighting the window manager: Qt renders the overlay to an image, and VULKAN draws
// it as the last thing in the frame. Input already worked -- the surface is WM_NCHITTEST
// transparent, so clicks fall through to the real widgets underneath.
//
// No vertex buffer: three vertices covering the screen.
#version 450

layout(location = 0) out vec2 v_uv;

void main()
{
  vec2 uv = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
  v_uv = uv;                                   // (0,0) (2,0) (0,2)
  gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0); // (-1,-1) (3,-1) (-1,3)
}
