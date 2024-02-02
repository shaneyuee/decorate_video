/*
 * Compile with: g++ -o test2 test2.cpp -lglfw3 -lGLU -lGL -lEGL -lX11
 * */
#ifdef USE_OFF_SCREEN
#include <GL/glew.h>
#include <EGL/egl.h>
#include <GL/gl.h>
#include <iostream>
#include <stdio.h>

static const EGLint configAttribs[] = {
    EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
    EGL_BLUE_SIZE, 8,
    EGL_GREEN_SIZE, 8,
    EGL_RED_SIZE, 8,
    EGL_DEPTH_SIZE, 8,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
    EGL_NONE
};

#define pbufferWidth 9
#define pbufferHeight 9

static const EGLint pbufferAttribs[] = {
    EGL_WIDTH, pbufferWidth,
    EGL_HEIGHT, pbufferHeight,
    EGL_NONE
};

static EGLDisplay eglDpy = EGL_NO_DISPLAY;

int initEGL()
{
    // 1. Initialize EGL
    eglDpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (eglDpy == EGL_NO_DISPLAY)
    {
        auto err = eglGetError();
        auto errstr = eglQueryString(eglDpy, err);
        fprintf(stderr, "eglGetDisplay failed, err=%d:%s.\n", err, errstr);
        return -1;
    }
    EGLint major = 0, minor = 0;

    auto ret = eglInitialize(eglDpy, &major, &minor);
    if (ret != EGL_TRUE)
    {
        auto err = eglGetError();
        auto errstr = eglQueryString(eglDpy, err);
        fprintf(stderr, "eglInitialize failed, major=%d, minor=%d, err=%d:%s.\n", major, minor, err, errstr);
        return -2;
    }

    // 2. Select an appropriate configuration
    EGLint numConfigs;
    EGLConfig eglCfg;

    ret = eglChooseConfig(eglDpy, configAttribs, &eglCfg, 1, &numConfigs);
    if (ret != EGL_TRUE)
    {
        fprintf(stderr, "eglChooseConfig failed.\n");
        return -3;
    }

    // 3. Create a surface
    EGLSurface eglSurf = eglCreatePbufferSurface(eglDpy, eglCfg,
                                                pbufferAttribs);
    if (eglSurf == EGL_NO_SURFACE)
    {
        fprintf(stderr, "eglCreatePbufferSurface failed.\n");
        return -4;
    }
    // 4. Bind the API
    ret = eglBindAPI(EGL_OPENGL_API);
    if (ret == EGL_FALSE)
    {
        fprintf(stderr, "eglBindAPI failed.\n");
        return -5;
    }
    // 5. Create a context and make it current
    EGLContext eglCtx = eglCreateContext(eglDpy, eglCfg, EGL_NO_CONTEXT,
                                        NULL);
    if (eglCtx == EGL_NO_CONTEXT)
    {
        fprintf(stderr, "eglCreateContext failed.\n");
        return -6;
    }
    ret = eglMakeCurrent(eglDpy, eglSurf, eglSurf, eglCtx);
    if (ret == EGL_FALSE)
    {
        fprintf(stderr, "eglMakeCurrent failed.\n");
        return -7;
    }

    auto i = glewInit();
    if (i != GLEW_OK)
    {
        fprintf(stderr, "glewInit failed, msg: %s\n", glewGetErrorString(i));
        return -8;
    }

    // from now on use your OpenGL context
    printf("EGL initialized successfully.\n");
    return 0;
}

int uninitEGL()
{
    // Terminate EGL when finished
    eglTerminate(eglDpy);
    return 0;
}

#else

int initEGL()
{
    return 0;
}

int uninitEGL()
{
    return 0;
}

#endif

