#ifndef __SHADER_UTIL_H__
#define __SHADER_UTIL_H__

#include <string.h>

const static char* VERTEX_SHADER_STRING = 
"#version 330 core \n \
layout (location = 0) in vec3 position; \n \
layout (location = 1) in vec2 texCoord; \n \
out vec2 TexCoord; \n \
void main() \n \
{ \n \
    gl_Position = vec4(position, 1.0f); \n \
    TexCoord = texCoord; \n \
}";

const static char* BGR_FRAGMENT_SHADER_STRING = 
"#version 330 core \n \
in vec2 TexCoord; \n \
out vec4 color; \n \
uniform sampler2D ourTexture; \n \
void main() \n \
{ \n \
  color = texture(ourTexture, TexCoord); \n \
}";

const static char* XFM_VERTEX_SHADER_STRING = 
"#version 330 core \n \
layout (location = 0) in vec3 position; \n \
layout (location = 1) in vec2 texCoord; \n \
out vec2 TexCoord; \n \
uniform mat4 transform; \n \
uniform mat4 projection; \n \
void main() \n \
{ \n \
    gl_Position = projection * transform * vec4(position, 1.0f); \n \
    TexCoord = texCoord; \n \
}";

const static char* XFM_FRAGMENT_SHADER_STRING = 
"#version 330 core \n \
in vec2 TexCoord; \n \
out vec4 color; \n \
uniform sampler2D ourTexture; \n \
uniform lowp float qt_Opacity; \n \
void main() \n \
{ \n \
  vec4 texel = texture(ourTexture, TexCoord); \n \
  color = vec4(texel.rgb, texel.a * qt_Opacity); \n \
}";

#define ALPHA_FRAGMENT_GLSL(cond, xa, ya, xi, yi, factordef, mulfactor) \
"#version 330 core \n \
in vec2 TexCoord; \n \
out vec4 color; \n \
uniform sampler2D ourTexture; \n \
" factordef " \n \
void main() \n \
{ \n \
  if(TexCoord." cond "){ \n \
    vec2 uva = vec2(TexCoord." xa ", TexCoord." ya "); \n \
    vec2 uvi = vec2(TexCoord." xi ", TexCoord." yi "); \n \
    float a = texture(ourTexture, uva).r " mulfactor "; \n \
    vec3 rgb = texture(ourTexture, uvi).rgb; \n \
    color = vec4(rgb, a); \n \
  } \n \
  else \n \
  { \n \
    color = vec4(0.0, 0.0, 0.0, 0.0); \n \
  } \n \
}"

#define NOR_ALPHA_FRAGMENT_GLSL(cond, xa, ya, xi, yi) ALPHA_FRAGMENT_GLSL(cond, xa, ya, xi, yi, "", "")
#define XFM_ALPHA_FRAGMENT_GLSL(cond, xa, ya, xi, yi) ALPHA_FRAGMENT_GLSL(cond, xa, ya, xi, yi, "uniform lowp float qt_Opacity = 1.0;", "* qt_Opacity")

const static char* ALPHA_LEFT_FRAGMENT_SHADER_STRING =   NOR_ALPHA_FRAGMENT_GLSL("x >= 0.25 && TexCoord.x < 0.75", "x-0.25", "y", "x+0.25", "y");
const static char* ALPHA_RIGHT_FRAGMENT_SHADER_STRING =  NOR_ALPHA_FRAGMENT_GLSL("x >= 0.25 && TexCoord.x < 0.75", "x+0.25", "y", "x-0.25", "y");
const static char* ALPHA_TOP_FRAGMENT_SHADER_STRING =    NOR_ALPHA_FRAGMENT_GLSL("y >= 0.25 && TexCoord.y < 0.75", "x", "y-0.25", "x", "y+0.25");
const static char* ALPHA_BOTTOM_FRAGMENT_SHADER_STRING = NOR_ALPHA_FRAGMENT_GLSL("y >= 0.25 && TexCoord.y < 0.75", "x", "y+0.25", "x", "y-0.25");

const static char* XFM_ALPHA_LEFT_FRAGMENT_SHADER_STRING =   XFM_ALPHA_FRAGMENT_GLSL("x >= 0.25 && TexCoord.x < 0.75", "x-0.25", "y", "x+0.25", "y");
const static char* XFM_ALPHA_RIGHT_FRAGMENT_SHADER_STRING =  XFM_ALPHA_FRAGMENT_GLSL("x >= 0.25 && TexCoord.x < 0.75", "x+0.25", "y", "x-0.25", "y");
const static char* XFM_ALPHA_TOP_FRAGMENT_SHADER_STRING =    XFM_ALPHA_FRAGMENT_GLSL("y >= 0.25 && TexCoord.y < 0.75", "x", "y-0.25", "x", "y+0.25");
const static char* XFM_ALPHA_BOTTOM_FRAGMENT_SHADER_STRING = XFM_ALPHA_FRAGMENT_GLSL("y >= 0.25 && TexCoord.y < 0.75", "x", "y+0.25", "x", "y-0.25");

#include "glad/glad.h"
#include <iostream>

using namespace std;

class GLShader
{
public:
	GLShader(const char* vShaderCode,const char* fShaderCode) {
        GLuint vertex, fragment;
        GLint success;
        GLchar infoLog[512];
        vertex = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(vertex, 1, &vShaderCode, NULL);
        glCompileShader(vertex);
        glGetShaderiv(vertex, GL_COMPILE_STATUS, &success);
        if (!success)
        {
            glGetShaderInfoLog(vertex, 512, NULL, infoLog);
            std::cout << "ERROR::SHADER::VERTEX::COMPILATION_FAILED\n" << infoLog << std::endl;
        }
        fragment = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(fragment, 1, &fShaderCode, NULL);
        glCompileShader(fragment);
        glGetShaderiv(fragment, GL_COMPILE_STATUS, &success);
        if (!success)
        {
            glGetShaderInfoLog(fragment, 512, NULL, infoLog);
            std::cout << "ERROR::SHADER::FRAGMENT::COMPILATION_FAILED\n" << infoLog << std::endl;
        }

        program_ = glCreateProgram();
        glAttachShader(program_, vertex);
        glAttachShader(program_, fragment);
        glLinkProgram(program_);

        glGetProgramiv(program_, GL_LINK_STATUS, &success);
        if (!success)
        {
            glGetProgramInfoLog(program_, 512, NULL, infoLog);
            std::cout << "ERROR::SHADER::PROGRAM::LINKING_FAILED\n" << infoLog << std::endl;
        }
        glDeleteShader(vertex);
        glDeleteShader(fragment);

        uniform_[0] = glGetUniformLocation(program_, "ourTexture");
        if (vShaderCode == XFM_VERTEX_SHADER_STRING)
        {
            uniform_[1] = glGetUniformLocation(program_, "transform");
            uniform_[3] = glGetUniformLocation(program_, "projection");
        }
        if (strstr(fShaderCode, "qt_Opacity") != NULL)
        {
            uniform_[2] = glGetUniformLocation(program_, "qt_Opacity");
            std::cout << "Using opacity shader." << std::endl;
        }
        attrib_[0] = glGetAttribLocation(program_, "position");
        attrib_[1] = glGetAttribLocation(program_, "texCoord");
	}

    ~GLShader() {
        if (program_) {
            glDeleteProgram(program_);
            program_ = 0;
        }
    }

	void Use() {
        glUseProgram(program_);
	}

    GLint GetAttrib(int idx) {
        return attrib_[idx];
    }

    GLint GetUniform(int idx) {
        return uniform_[idx];
    }
private:
	GLuint program_ = 0;
    GLint uniform_[4] = { 0 };
    GLint attrib_[2] = { 0 };
};

#endif
