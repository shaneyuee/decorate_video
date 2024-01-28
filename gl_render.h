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
//   - init_chromakey - whether to initialize ave chromakey sdk
//   - debug - set to 1 to display statistics
//   - scale_prefer - must be one of "speed", "quality"
//   - protect_areas - protect green color areas not to be chromakeyed
//   - output_alpha - keep alpha channel, download image in BGRA format
//   - matting_scale - scale > 0, default 1.0, aggressive 1.2
//   - enable_green_suppress - enable green color suppression if needed
//   - matting_power - power > 0, default 3.0, aggressive 4.0
//   - matting_model - eff: prefer effect, perf: prefer performance
int gl_init_render(int dispwidth, int dispheight, bool init_chromakey, int debug,
                   const char *scale_prefer, std::vector<std::vector<int>> &protect_areas,
                   bool output_alpha, float matting_scale, bool enable_green_suppress,
                   float matting_power, const char *matting_model);

// reset the screen window
int gl_reset_screen();

int gl_delete_texture(int texture);

// upload bgr image and render to framebuffer
int gl_render_texture_bgr(int textureId, uint8_t *buffer, int imgw, int imgh, int x, int y, int w, int h, int rotation, int opacity);

// upload bgra image and render to framebuffer
int gl_render_texture_bgra(int textureId, uint8_t *buffer, int imgw, int imgh, int x, int y, int w, int h, int rotation, int opacity);

// upload bgra image and do chroma keying before rendering
// buffer must be bgra format
int gl_render_texture_chromakey(int textureId, uint8_t *buffer, int channels, int imgw, int imgh, int x, int y, int w, int h, int rotation, int opacity);

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

