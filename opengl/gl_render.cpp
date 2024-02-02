//
// remove green matting implementation
//
#include "shader_util.h" // MUST be included before glew/egl

#ifdef USE_OFF_SCREEN
#  include <EGL/egl.h>
#else
#  include <GLFW/glfw3.h>
#endif

#include <iostream>
#include <vector>
#include <unistd.h>
#include "egl.h"
#include "../decorateVideo.h"
#include "../AutoTime.h"
#include "gl_render.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>


void print_ave_sdk_version()
{
	printf("Not using green matting SDK\n");
}


static int is_initialized = 0;
static GLuint framebuffer_id = 0;
static GLuint frame_rbo = 0;
static GLuint colorBufferTexture = 0;
// display window size
static int display_width = 0;
static int display_height = 0;
static int enable_debug = 0;
static int enable_alpha = 0;
static GLShader* shaders[32] = {nullptr};
static GLuint display_vao = 0;
static GLuint display_vbo = 0;
static GLuint repeat_vao = 0;
static GLuint repeat_vbo = 0;
static GLuint display_flip_vao = 0;
static GLuint display_flip_vbo = 0;
static GLuint xfm_vao = 0;
static GLuint xfm_vbo = 0;

// VAO for display
static GLfloat display_vertices[] = { -1.0, -1.0, 0.0, 0.0, 0.0,   // bottom left
                                       1.0, -1.0, 0.0, 1.0, 0.0,   // bottom right 
                                      -1.0,  1.0, 0.0, 0.0, 1.0,   // top left
                                       1.0,  1.0, 0.0, 1.0, 1.0 }; // top right

#define TEX_VERTICE_ROW_1 14
#define TEX_VERTICE_ROW_2 19
#define TEX_VERTICE_COL_1 8
#define TEX_VERTICE_COL_2 18
static GLfloat repeat_vertices[] = {  -1.0, -1.0, 0.0, 0.0, 0.0,	// bottom left
                                       1.0, -1.0, 0.0, 8.0, 0.0,   // bottom right - idx 8
                                      -1.0,  1.0, 0.0, 0.0, 2.0,   // top left     -         14
                                       1.0,  1.0, 0.0, 8.0, 2.0 }; // top right    - idx 18  19

static GLfloat display_flip_vertices[] = { -1.0, -1.0, 0.0, 0.0, 0.0,
                                       1.0, -1.0, 0.0, 1.0, 0.0,
                                      -1.0,  1.0, 0.0, 0.0, 1.0,
                                       1.0,  1.0, 0.0, 1.0, 1.0 };

static GLfloat xfm_vertices[] = {      0.0,  0.0, 0.0, 0.0, 0.0,
                                       1.0,  0.0, 0.0, 1.0, 0.0,
                                       0.0,  1.0, 0.0, 0.0, 1.0,
                                       1.0,  1.0, 0.0, 1.0, 1.0 };

enum SCALE_PREFER
{
	SP_SPEED = 0, // bilinear
	SP_QUALITY = 1, // mipmap-linear
};
static SCALE_PREFER scaleprefer = SP_SPEED;

static vector<GLuint> allTextures;

#define DEBUG_PRINTF

#ifdef DEBUG_PRINTF
    void clearGLError()
    {
        while (glGetError() != GL_NO_ERROR)
            ;
    }

    bool logGLCall(const char *function, const char *file, int line)
    {
        while (GLenum error = glGetError())
        {
            std::cerr << "[OpenGL Error] (" << error << ")" << function << " " << file << ":" << line << std::endl;
            return false;
        }
        return true;
    }
#else
	void clearGLError() {}
	bool logGLCall(const char *function, const char *file, int line) {}
#endif

#ifdef DEBUG_PRINTF
    void clearGLError();
    bool logGLCall(const char *function, const char *file, int line);
#define GLCall(x)   \
    clearGLError(); \
    x;              \
    logGLCall(#x, __FILE__, __LINE__)
#else
#define GLCall(x) x
#endif

GLuint create_texture(int width, int height, GLint format, bool repeat = false, void *buffer = NULL) {
	GLuint texture = 0;
	glActiveTexture(GL_TEXTURE0);
	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D, texture);
	if (repeat)
	{
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	}
	else
	{
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, scaleprefer==SP_SPEED? GL_LINEAR : GL_NEAREST_MIPMAP_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		if (scaleprefer==SP_QUALITY)
			glGenerateMipmap(GL_TEXTURE_2D);
	}
	GLint fmt = (format==GL_BGRA? GL_RGBA : (format==GL_BGR? GL_RGB : format));
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, fmt, width, height, 0, format, GL_UNSIGNED_BYTE, buffer);
	glBindTexture(GL_TEXTURE_2D, 0);
	return texture;
}

FILE* gLogFile = nullptr;
int gLogCounter = 0;

void external_log(const char* strLog) {
	// ave_vp will hold lock when calling this callback
	if (!gLogFile) {
		gLogFile = fopen("ave_vp_log.txt", "w");
		if (!gLogFile) {
			return;
		}
	}
	fprintf(gLogFile, "%s", strLog);
	if (++gLogCounter > 50) {
		gLogCounter = 0;
		fflush(gLogFile);
	}
}

int create_framebuffer(unsigned int &framebuffer, unsigned int &colorBufferTexture, unsigned int &rbo, int width, int height)
{
	if (framebuffer != 0)
	{
		glDeleteFramebuffers(1, &framebuffer);
	}
	glGenFramebuffers(1, &framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);

	// 创建渲染纹理
	if (colorBufferTexture != 0)
	{
		glDeleteTextures(1, &colorBufferTexture);
	}
	glGenTextures(1, &colorBufferTexture);

	// "Bind" the newly created texture : all future texture functions will modify this texture
	glBindTexture(GL_TEXTURE_2D, colorBufferTexture);

	// Give an empty image to OpenGL ( the last "0" )
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);

	// Poor filtering. Needed !
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

	// 创建渲染缓冲
	if (rbo != 0)
	{
		glDeleteRenderbuffers(1, &rbo);
	}
	glGenRenderbuffers(1, &rbo);
	glBindRenderbuffer(GL_RENDERBUFFER, rbo);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT, width, height);            // use a single renderbuffer object for both a depth AND stencil buffer.
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rbo); // now actually attach it

	// 帧缓冲绑定纹理
	glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, colorBufferTexture, 0);

	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
	{
		fprintf(stderr, "ERROR::FRAMEBUFFER:: Framebuffer is not complete!\n");
		return -1;
	}
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, 0);
	return 0;
}

int alpha_index(int alpha_mode)
{
	switch(alpha_mode)
	{
	case 'l':
	case 'L':
		return 0;
	case 'r':
	case 'R':
		return 1;
	case 't':
	case 'T':
		return 2;
	case 'b':
	case 'B':
		return 3;
	}
	return -1;
}

GLShader *getShader(int rotation, int opacity, int alpha_mode = 0)
{
	bool has_rotation = ((rotation%360) != 0);
	bool has_opacity = (opacity > 0 && opacity < 100);
	int idx = (has_rotation? 1 : 0) * 2 + (has_opacity? 1 : 0);
	int alpha_idx = alpha_index(alpha_mode);
	static const char *alpha_vertexs[] = { ALPHA_LEFT_FRAGMENT_SHADER_STRING, ALPHA_RIGHT_FRAGMENT_SHADER_STRING, ALPHA_TOP_FRAGMENT_SHADER_STRING, ALPHA_BOTTOM_FRAGMENT_SHADER_STRING }; 
	static const char *xfm_alpha_vertexs[] = { XFM_ALPHA_LEFT_FRAGMENT_SHADER_STRING, XFM_ALPHA_RIGHT_FRAGMENT_SHADER_STRING, XFM_ALPHA_TOP_FRAGMENT_SHADER_STRING, XFM_ALPHA_BOTTOM_FRAGMENT_SHADER_STRING }; 
	if (alpha_idx >= 0)
		idx = (has_rotation? 2 : 1) * 8 + (has_opacity? 1:0) * 4 + alpha_idx;
	if (shaders[idx] == NULL)
	{
		shaders[idx] = new GLShader(has_rotation? XFM_VERTEX_SHADER_STRING : VERTEX_SHADER_STRING, \
									alpha_idx >= 0? \
									(has_opacity? xfm_alpha_vertexs[alpha_idx] : alpha_vertexs[alpha_idx]) : \
									(has_opacity? XFM_FRAGMENT_SHADER_STRING : BGR_FRAGMENT_SHADER_STRING));
	}
	return shaders[idx];
}

#ifndef USE_OFF_SCREEN
static bool initGLFW()
{
	printf("OpenglWorkThread::init init gl render thread\n");
	glfwInit();
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
	glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
	glfwWindowHint(GLFW_RESIZABLE, GL_FALSE);
	glfwWindowHint(GLFW_VISIBLE, GL_FALSE);
#ifdef MacOS
	fprintf(stderr, "Running on MacOS\n");
	glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE); // Mac
#endif
	// glfw window creation
	// --------------------
	auto m_renderWindow = glfwCreateWindow(100, 100, "ai-live-sdk", NULL, NULL);
	if (m_renderWindow == NULL)
	{
		fprintf(stderr, "OpenglWorkThread::init Failed to create GLFW window\n");
		glfwTerminate();
		return false;
	}
	glfwHideWindow(m_renderWindow);
	glfwMakeContextCurrent(m_renderWindow);

	// glad: load all OpenGL function pointers
	if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress))
	{
		std::cout << "Failed to initialize GLAD" << std::endl;
		return -1;
	}

	//glfwMakeContextCurrent(NULL); // 切换线程处理，分离上下文
	return true;
}
#endif


int gl_init_render(int dispwidth, int dispheight, int debug, const char *scale_prefer, bool output_alpha)
{
	if (debug)
		enable_debug = 1;
	if (output_alpha)
		enable_alpha = 1;
	if (scale_prefer && strncasecmp(scale_prefer, "quality", 8)==0)
	{
		scaleprefer = SP_QUALITY;
	}
	else if(scale_prefer && strncasecmp(scale_prefer, "quality", 8)==0)
	{
		scaleprefer = SP_SPEED;
	}

	if(is_initialized && 
		dispwidth==display_width && dispheight==display_height)
		return 0;

	display_width = dispwidth;
	display_height = dispheight;

	/* Initialize the library */
	if (!is_initialized) // only init gl for the first time
	{
#ifdef USE_OFF_SCREEN
		if (initEGL()) // off screen version of glfw
#else
		if (initGLFW() == false)
#endif
		{
			std::cerr << "initEGL failed." << std::endl;
			return -1;
		}
#ifdef USE_OFF_SCREEN
		if (!gladLoadGLLoader((GLADloadproc)eglGetProcAddress))
		{
			std::cerr << "Failed to initialize GLAD" << std::endl;
			return -1;
		}
#endif
		is_initialized = 1;
	}

	// create shader for display
	shaders[0] = new GLShader(VERTEX_SHADER_STRING, BGR_FRAGMENT_SHADER_STRING);

	glGenVertexArrays(1, &display_vao);
	glBindVertexArray(display_vao);
	glGenBuffers(1, &display_vbo);
	glBindBuffer(GL_ARRAY_BUFFER, display_vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(display_vertices), display_vertices, GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, 0, 5 * sizeof(GL_FLOAT), 0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, 0, 5 * sizeof(GL_FLOAT), (void*)(3 * sizeof(GL_FLOAT)));
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindVertexArray(0);

	glGenVertexArrays(1, &display_flip_vao);
	glBindVertexArray(display_flip_vao);
	glGenBuffers(1, &display_flip_vbo);
	glBindBuffer(GL_ARRAY_BUFFER, display_flip_vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(display_flip_vertices), display_flip_vertices, GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, 0, 5 * sizeof(GL_FLOAT), 0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, 0, 5 * sizeof(GL_FLOAT), (void*)(3 * sizeof(GL_FLOAT)));
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindVertexArray(0);

	glGenVertexArrays(1, &xfm_vao);
	glBindVertexArray(xfm_vao);
	glGenBuffers(1, &xfm_vbo);
	glBindBuffer(GL_ARRAY_BUFFER, xfm_vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(xfm_vertices), xfm_vertices, GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, 0, 5 * sizeof(GL_FLOAT), 0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, 0, 5 * sizeof(GL_FLOAT), (void*)(3 * sizeof(GL_FLOAT)));
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindVertexArray(0);

	// create framebuffer
	int i = create_framebuffer(framebuffer_id, colorBufferTexture, frame_rbo, display_width, display_height);
	if (i)
	{
		std::cerr << "create_framebuffer failed." << std::endl;
		uninitEGL();
		return -1;
	}

	return 0;
}

int gl_delete_texture(int texture)
{
	if (texture <= 0)
		return 0;

	glDeleteTextures(1, (GLuint*)&texture);
	for (int i=0; i<allTextures.size(); i++)
	{
		if (allTextures[i] == texture)
		{
			allTextures.erase(allTextures.begin()+i);
			break;
		}
	}
	return 0;
}


int gl_reset_screen()
{
	// clear display window color
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_id);
	glViewport(0, 0, display_width, display_height);
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	return 0;
}

// upload bgr image and render to framebuffer
int gl_render_texture_bgr(int textureId, uint8_t *buffer, int imgw, int imgh, int x, int y, int w, int h, int rotation, int opacity)
{
	AUTOTIMED("OpenGL draw BGR image Run", enable_debug);

	if (opacity <= 0 || opacity >= 100)
		opacity = 0;

	if (textureId == 0)
	{
		textureId = create_texture(imgw, imgh, GL_BGR, false, buffer);
		allTextures.push_back(textureId);
		buffer = NULL; // already loaded, dont load again later
	}
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_id);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, textureId);
	if (buffer)
	{
		// set alignment to 1, so that data can be processed by opencv
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, imgw, imgh, GL_BGR, GL_UNSIGNED_BYTE, buffer);
		if (scaleprefer==SP_QUALITY)
			glGenerateMipmap(GL_TEXTURE_2D);
	}
	GLShader *shader = getShader(rotation, opacity);
	shader->Use();
	bool has_rotation = (rotation % 360) != 0;
	if (has_rotation)
	{
		glViewport(0, 0, display_width, display_height);
		glm::mat4 transform = glm::mat4(1.0f);
		// make sure to initialize matrix to identity matrix first
		transform = glm::translate(transform, glm::vec3((float)x, (float)y, 0.0f));
		transform = glm::translate(transform, glm::vec3(0.5f * w, 0.5f * h, 0.0f)); // set rotation point at center
		transform = glm::rotate(transform, glm::radians((float)rotation), glm::vec3(0.0f, 0.0f, 1.0f));
		transform = glm::translate(transform, glm::vec3(-0.5f * w, -0.5f * h, 0.0f)); // move origin back
		transform = glm::scale(transform, glm::vec3((float)w, (float)h, 1.0f)); // last scale
		glUniformMatrix4fv(shader->GetUniform(1), 1, GL_FALSE, glm::value_ptr(transform));

		glm::mat4 projection = glm::ortho(0.0f, (float)display_width, 0.0f, (float)display_height, -1.0f, 1.0f);
		glUniformMatrix4fv(shader->GetUniform(3), 1, GL_FALSE, glm::value_ptr(projection));
		glBindVertexArray(xfm_vao);
	}
	else // no rotation, use viewport to move and resize
	{
		glViewport(x, y, w, h);
		glBindVertexArray(display_flip_vao); // display_vao
	}

	if (opacity)
	{
		glUniform1f(shader->GetUniform(2), ((float)opacity) / 100);
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	}
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glBindVertexArray(0);
	glBindTexture(GL_TEXTURE_2D, 0);
	if (opacity)
		glDisable(GL_BLEND);

	return textureId;
}

int gl_render_texture_bgra(int textureId, uint8_t *buffer, int imgw, int imgh, int x, int y, int w, int h, int rotation, int opacity)
{
	AUTOTIMED("OpenGL draw BGRA image Run", enable_debug);

	if (textureId == 0)
	{
		textureId = create_texture(imgw, imgh, GL_BGRA, false, buffer);
		allTextures.push_back(textureId);
		buffer = NULL; // already loaded, dont load again later
	}
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_id);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, textureId);
	if (buffer)
	{
		// set alignment to 1, so that data can be processed by opencv
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, imgw, imgh, GL_BGRA, GL_UNSIGNED_BYTE, buffer);
		if (scaleprefer==SP_QUALITY)
			glGenerateMipmap(GL_TEXTURE_2D);
	}
	GLShader *shader = getShader(rotation, opacity);
	shader->Use();
	bool has_rotation = (rotation % 360) != 0;

	if (has_rotation)
	{
		glViewport(0, 0, display_width, display_height);
		glm::mat4 transform = glm::mat4(1.0f);
		// make sure to initialize matrix to identity matrix first
		transform = glm::translate(transform, glm::vec3((float)x, (float)y, 0.0f));
		transform = glm::translate(transform, glm::vec3(0.5f * w, 0.5f * h, 0.0f)); // set rotation point at center
		transform = glm::rotate(transform, glm::radians((float)rotation), glm::vec3(0.0f, 0.0f, 1.0f));
		transform = glm::translate(transform, glm::vec3(-0.5f * w, -0.5f * h, 0.0f)); // move origin back
		transform = glm::scale(transform, glm::vec3((float)w, (float)h, 1.0f)); // last scale
		glUniformMatrix4fv(shader->GetUniform(1), 1, GL_FALSE, glm::value_ptr(transform));

		glm::mat4 projection = glm::ortho(0.0f, (float)display_width, 0.0f, (float)display_height, -1.0f, 1.0f);
		glUniformMatrix4fv(shader->GetUniform(3), 1, GL_FALSE, glm::value_ptr(projection));

		glBindVertexArray(xfm_vao);
	}
	else
	{
		glViewport(x, y, w, h);
		glBindVertexArray(display_flip_vao); // display_vao
	}
	if ((opacity > 0 && opacity < 100))
	{
		glUniform1f(shader->GetUniform(2), ((float)opacity) / 100); // opacity
	}
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glBindVertexArray(0);
	glDisable(GL_BLEND);
	glBindTexture(GL_TEXTURE_2D, 0);

	return textureId;
}

// buffer must be bgra format
int gl_render_texture(int textureId, uint8_t *buffer, int channels, int imgw, int imgh, int x, int y, int w, int h, int rotation, int opacity)
{
	if (channels == 3)
		return gl_render_texture_bgr(textureId, buffer, imgw, imgh, x, y, w, h, rotation, opacity);
	else
		return gl_render_texture_bgra(textureId, buffer, imgw, imgh, x, y, w, h, rotation, opacity);
}

// alpha_mode:
//   'l' - alpha video at left side
//   'r' - right
//   't' - top
//   'b' - bottom
int gl_render_texture_alpha(int textureId, uint8_t *buffer, int channels, int imgw, int imgh, int x, int y, int w, int h, int rotation, int opacity, int alpha_mode)
{
	AUTOTIMED("OpenGL draw half-alpha image Run", enable_debug);

	if (textureId == 0)
	{
		textureId = create_texture(imgw, imgh, channels==3? GL_BGR : GL_BGRA, false, buffer);
		allTextures.push_back(textureId);
		buffer = NULL; // already loaded, dont load again later
	}
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_id);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, textureId);
	if (buffer)
	{
		// set alignment to 1, so that data can be processed by opencv
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, imgw, imgh, channels==3? GL_BGR : GL_BGRA, GL_UNSIGNED_BYTE, buffer);
		if (scaleprefer==SP_QUALITY)
			glGenerateMipmap(GL_TEXTURE_2D);
	}

	alpha_mode = tolower(alpha_mode);
	if (alpha_mode == 'l' || alpha_mode == 'r')
	{
		x -= w / 2;
		w *= 2;
	}
	else if (alpha_mode == 't' || alpha_mode == 'b')
	{
		y -= h / 2;
		h *= 2;
	}

	GLShader *shader = getShader(rotation, opacity, alpha_mode);
	shader->Use();

	bool has_rotation = (rotation % 360) != 0;
	if (has_rotation)
	{
		glViewport(0, 0, display_width, display_height);
		glm::mat4 transform = glm::mat4(1.0f);
		// make sure to initialize matrix to identity matrix first
		transform = glm::translate(transform, glm::vec3((float)x, (float)y, 0.0f));
		transform = glm::translate(transform, glm::vec3(0.5f * w, 0.5f * h, 0.0f)); // set rotation point at center
		transform = glm::rotate(transform, glm::radians((float)rotation), glm::vec3(0.0f, 0.0f, 1.0f));
		transform = glm::translate(transform, glm::vec3(-0.5f * w, -0.5f * h, 0.0f)); // move origin back
		transform = glm::scale(transform, glm::vec3((float)w, (float)h, 1.0f)); // last scale
		glUniformMatrix4fv(shader->GetUniform(1), 1, GL_FALSE, glm::value_ptr(transform));

		glm::mat4 projection = glm::ortho(0.0f, (float)display_width, 0.0f, (float)display_height, -1.0f, 1.0f);
		glUniformMatrix4fv(shader->GetUniform(3), 1, GL_FALSE, glm::value_ptr(projection));

		glBindVertexArray(xfm_vao);
	}
	else
	{
		glViewport(x, y, w, h);
		glBindVertexArray(display_vao); // display_flip_vao
	}
	if (opacity > 0 && opacity < 100)
	{
		glUniform1f(shader->GetUniform(2), ((float)opacity) / 100); // opacity
	}
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glBindVertexArray(0);
	glDisable(GL_BLEND);
	glBindTexture(GL_TEXTURE_2D, 0);

	return textureId;
}

void init_repeat_vao(float rows, float cols)
{
	if (rows == 0.0)
		rows = display_height > display_width? 8 : 6;
	if (cols == 0.0)
		cols = display_height > display_width? 4 : 6;
	repeat_vertices[TEX_VERTICE_ROW_1] = repeat_vertices[TEX_VERTICE_ROW_2] = rows;
	repeat_vertices[TEX_VERTICE_COL_1] = repeat_vertices[TEX_VERTICE_COL_2] = cols;

	glGenVertexArrays(1, &repeat_vao);
	glBindVertexArray(repeat_vao);
	glGenBuffers(1, &repeat_vbo);
	glBindBuffer(GL_ARRAY_BUFFER, repeat_vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(repeat_vertices), repeat_vertices, GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, 0, 5 * sizeof(GL_FLOAT), 0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, 0, 5 * sizeof(GL_FLOAT), (void*)(3 * sizeof(GL_FLOAT)));
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindVertexArray(0);
}

// upload bgra image and render to framebuffer, mode controls how to draw the image
//   mode 
//      - 1, subtitle, bottom center
//      - 2, subtitle, top center
//      - 3, watermark, fill by scaling
//      - 4, watermark, fill by duplication
int gl_render_texture_mark(int textureId, uint8_t *buffer, int imgw, int imgh, int rotation, int opacity, MARK_RENDER_MODE mode, int repeat_rows, int repeat_cols)
{
	AUTOTIMED("OpenGL draw MARK image Run", enable_debug);

	if (textureId == 0)
	{
		if (mode == MARK_REPEAT)
			init_repeat_vao(repeat_rows * 2, repeat_cols * 2);
		textureId = create_texture(imgw, imgh, GL_BGRA, mode==MARK_REPEAT, buffer);
		allTextures.push_back(textureId);
	}
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_id);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, textureId);
	GLShader *shader = getShader(rotation, opacity);
	shader->Use();
	if (rotation%360)
	{
		glm::mat4 transform = glm::mat4(1.0f);
		// make sure to initialize matrix to identity matrix first
		transform = glm::rotate(transform, (float)glm::radians((float)rotation), glm::vec3(0.0f, 0.0f, 1.0f));
		glUniformMatrix4fv(shader->GetUniform(1), 1, GL_FALSE, glm::value_ptr(transform)); // transform

		glm::mat4 projection = glm::mat4(1.0f);
		glUniformMatrix4fv(shader->GetUniform(3), 1, GL_FALSE, glm::value_ptr(projection));
	} 
	if ((opacity > 0 && opacity < 100))
	{
		glUniform1f(shader->GetUniform(2), ((float)opacity) / 100); // opacity
	}
	glBindVertexArray(mode==MARK_REPEAT? repeat_vao : display_flip_vao);
	// calculate x, y, w, h
	int x = 0, y = 0, w = display_width, h = display_height;
	if (mode==MARK_REPEAT) // make sure every corner has marks
	{
		int max = display_width > display_height? display_width : display_height;
		x = y = -max/2;
		w = h =  max*2;
	}
	else if(mode==MARK_BOTTOM)
	{
		x = (display_width - imgw) / 2;
		y = display_height - imgh;
	}
	else if(mode==MARK_TOP)
	{
		x = (display_width - imgw) / 2;
		y = display_height - imgh;
	}
	if(mode==MARK_BOTTOM || mode==MARK_TOP)
	{
		w = imgw;
		h = imgh;
	}
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glViewport(x, y, w, h);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glBindVertexArray(0);
	glDisable(GL_BLEND);
	glBindTexture(GL_TEXTURE_2D, 0);
	return textureId;
}


// fill buffer in BGR format
// buffer size must be >= disp_width * disp_height * 3
int gl_download_image(uint8_t *buffer)
{
	AUTOTIMED("OpenGL download image Run", enable_debug);
	// set alignment to 1, so that data can be processed by opencv
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
#if 0
	glGetTexImage(GL_TEXTURE_2D, 0, enable_alpha? GL_BGRA : GL_BGR, GL_UNSIGNED_BYTE, buffer);
#else
	glReadPixels(0, 0, display_width, display_height, enable_alpha? GL_BGRA : GL_BGR, GL_UNSIGNED_BYTE, buffer);
#endif
	return 0;
}

int gl_uninit_render()
{
	if (gLogFile) {
		fclose(gLogFile);
		gLogFile = nullptr;
	}

	if (frame_rbo != 0)
		glDeleteRenderbuffers(1, &frame_rbo);
	if (framebuffer_id != 0)
		glDeleteFramebuffers(1, &framebuffer_id);
	if (display_vao != 0)
		glDeleteVertexArrays(1, &display_vao);
	if (display_vbo != 0)
		glDeleteBuffers(1, &display_vbo);
	if (display_flip_vao != 0)
		glDeleteVertexArrays(1, &display_flip_vao);
	if (display_flip_vbo != 0)
		glDeleteBuffers(1, &display_flip_vbo);
	if (xfm_vao != 0)
		glDeleteVertexArrays(1, &xfm_vao);
	if (xfm_vbo != 0)
		glDeleteBuffers(1, &xfm_vbo);
	if (colorBufferTexture != 0)
		glDeleteTextures(1, &colorBufferTexture);
	
	for (auto &t : allTextures)
	{
		if (t != 0)
			glDeleteTextures(1, &t);
	}

	uninitEGL();
	return 0;
}
