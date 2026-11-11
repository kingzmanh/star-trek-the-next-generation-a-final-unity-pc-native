# brender_hires - Argonaut BRender scenes at N x resolution

From Alien Odyssey (3DGAME.EXE). Wraps BrZbSceneRender and the game's end-of-frame copy:
the scene is rendered a second time into N x colour + depth pixelmaps seeded with the upscaled
background and its depth (so pre-rendered rooms still hide characters), the game's own pass runs
last, and the result is blended over the presented picture with a mask (Video > Internal Resolution).

Per game you need (tools/native_<tag>.txt, "wrap" lines):
- `<addr> N_g3d_scene_render wrap`  - BrZbSceneRender (eax world, edx camera, ebx colour pm, ecx depth pm)
- `<addr> N_g3d_frame_done wrap`    - the routine that copies the finished back buffer to the screen
- `G3D_BOUNDS_CB` in hires3d.c      - BRender's render-bounds callback global (BrZbSetRenderBoundsCallback
  stores it); zeroed during the extra pass or it overflows the game's dirty-tile grid at > 3x.
Find them with `tools/sigfind.py` against 3DGAME.EXE (BRender 1.x code is the same across games) or
via BRender's error strings. Pixelmap layout of that BRender version is at the top of hires3d.c.
Rename the N_g3d_* functions if you like; keep native names and hires3d.c in step.
