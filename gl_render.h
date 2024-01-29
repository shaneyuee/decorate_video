//
// opengl rendering functions
//
#ifndef __OPENGL_RENDERING_HEADER__
#define __OPENGL_RENDERING_HEADER__
#include <vector>

void print_ave_sdk_version();

// initialize opengl rendering engine
// parameters:
//   - dispwidth/dispheight - display window size
//   - debug - set to 1 to display statistics
//   - scale_prefer - must be one of "speed", "quality"
//   - output_alpha - keep alpha channel, download image in BGRA format
int gl_init_render(int dispwidth, int dispheight, int debug, const char *scale_prefer, bool output_alpha);

// reset the screen window
int gl_reset_screen();

int gl_delete_texture(int texture);

// upload bgr image and render to framebuffer
int gl_render_texture_bgr(int textureId, uint8_t *buffer, int imgw, int imgh, int x, int y, int w, int h, int rotation, int opacity);

// upload bgra image and render to framebuffer
int gl_render_texture_bgra(int textureId, uint8_t *buffer, int imgw, int imgh, int x, int y, int w, int h, int rotation, int opacity);

// upload bgr or bgra image and do render to framebuffer
int gl_render_texture(int textureId, uint8_t *buffer, int channels, int imgw, int imgh, int x, int y, int w, int h, int rotation, int opacity);

// alpha_mode:
//   'l' - alpha video at left side
//   'r' - right
//   't' - top
//   'b' - bottom
int gl_render_texture_alpha(int textureId, uint8_t *buffer, int channels, int imgw, int imgh, int x, int y, int w, int h, int rotation, int opacity, int alpha_mode);

enum MARK_RENDER_MODE
{
    MARK_BOTTOM = 1,
    MARK_TOP,
    MARK_SCALE,
    MARK_REPEAT,
};

// upload bgra image and render to framebuffer, mode controls how to draw the image
//   mode 
//      - 1, subtitle, bottom center
//      - 2, subtitle, top center
//      - 3, watermark, fill by scaling
//      - 4, watermark, fill by duplication
int gl_render_texture_mark(int textureId, uint8_t *buffer, int imgw, int imgh, int rotation, int opacity, MARK_RENDER_MODE mode, int repeat_rows, int repeat_cols);

// fill buffer in BGR format
// buffer size must be >= disp_width * disp_height * 3
int gl_download_image(uint8_t *buffer);

// uninitialize the rendering engine
int gl_uninit_render();

#endif

