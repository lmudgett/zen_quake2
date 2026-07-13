// gl3_local.h -- internal header for the modern OpenGL 3.3 core renderer.
// Works with the original Quake 2 refresh ABI (ref.h) and file formats.

#ifndef GL3_LOCAL_H
#define GL3_LOCAL_H

#include <glad/gl.h>
#include <SDL3/SDL.h>

#include "../client/ref.h"

#ifdef _WIN32
#define Q2_DLL_EXPORT __declspec(dllexport)
#else
#define Q2_DLL_EXPORT __attribute__((visibility("default")))
#endif

// the import table the engine hands us (printing, cvars, files, ...)
extern refimport_t	ri;

typedef struct
{
	int			width, height;		// current drawable size in pixels
	qboolean	fullscreen;
	SDL_Window	*window;
	SDL_GLContext	context;
} gl3state_t;

extern gl3state_t	gl3state;

// cvars
extern cvar_t	*gl_mode;
extern cvar_t	*vid_fullscreen;
extern cvar_t	*vid_gamma;
extern cvar_t	*gl_clear;

// gl3_glimp.c -- window / context management
void     GL3_ShutdownWindow (void);
void     GL3_StartFrame (void);		// make context current
void     GL3_SwapBuffers (void);
qboolean GL3_SetMode (int mode, qboolean fullscreen);

#endif // GL3_LOCAL_H
