// gl3_glimp.c -- SDL3 window and OpenGL 3.3 core context management.
// The renderer owns the window; the client reaches it via
// SDL_GL_GetCurrentWindow() for input.

#include "gl3_local.h"

gl3state_t	gl3state;

static GLADapiproc GL3_GetProcAddress (const char *name)
{
	return (GLADapiproc)SDL_GL_GetProcAddress (name);
}

qboolean GL3_SetMode (int mode, qboolean fullscreen)
{
	int	width, height;

	if (!ri.Vid_GetModeInfo (&width, &height, mode))
	{
		ri.Con_Printf (PRINT_ALL, "GL3_SetMode: invalid mode %d\n", mode);
		return false;
	}

	// tear down an existing window on a mode change
	GL3_ShutdownWindow ();

	SDL_GL_SetAttribute (SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute (SDL_GL_CONTEXT_MINOR_VERSION, 3);
	SDL_GL_SetAttribute (SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
	SDL_GL_SetAttribute (SDL_GL_DOUBLEBUFFER, 1);
	SDL_GL_SetAttribute (SDL_GL_DEPTH_SIZE, 24);
	SDL_GL_SetAttribute (SDL_GL_STENCIL_SIZE, 8);
	SDL_GL_SetAttribute (SDL_GL_RED_SIZE, 8);
	SDL_GL_SetAttribute (SDL_GL_GREEN_SIZE, 8);
	SDL_GL_SetAttribute (SDL_GL_BLUE_SIZE, 8);

	Uint32 flags = SDL_WINDOW_OPENGL;
	if (fullscreen)
		flags |= SDL_WINDOW_FULLSCREEN;

	gl3state.window = SDL_CreateWindow ("Quake 2", width, height, flags);
	if (!gl3state.window)
	{
		ri.Con_Printf (PRINT_ALL, "SDL_CreateWindow failed: %s\n", SDL_GetError());
		return false;
	}

	gl3state.context = SDL_GL_CreateContext (gl3state.window);
	if (!gl3state.context)
	{
		ri.Con_Printf (PRINT_ALL, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
		SDL_DestroyWindow (gl3state.window);
		gl3state.window = NULL;
		return false;
	}

	SDL_GL_MakeCurrent (gl3state.window, gl3state.context);

	if (!gladLoadGL (GL3_GetProcAddress))
	{
		ri.Con_Printf (PRINT_ALL, "gladLoadGL failed to load OpenGL 3.3\n");
		return false;
	}

	SDL_GL_SetSwapInterval (1);		// vsync on by default

	// use the real drawable size (respects high-DPI)
	SDL_GetWindowSizeInPixels (gl3state.window, &gl3state.width, &gl3state.height);
	gl3state.fullscreen = fullscreen;

	ri.Con_Printf (PRINT_ALL, "OpenGL %s\n", (const char *)glGetString (GL_VERSION));
	ri.Con_Printf (PRINT_ALL, "  vendor:   %s\n", (const char *)glGetString (GL_VENDOR));
	ri.Con_Printf (PRINT_ALL, "  renderer: %s\n", (const char *)glGetString (GL_RENDERER));

	glViewport (0, 0, gl3state.width, gl3state.height);

	// tell the client the real drawable size
	ri.Vid_NewWindow (gl3state.width, gl3state.height);

	return true;
}

void GL3_ShutdownWindow (void)
{
	if (gl3state.context)
	{
		SDL_GL_DestroyContext (gl3state.context);
		gl3state.context = NULL;
	}
	if (gl3state.window)
	{
		SDL_DestroyWindow (gl3state.window);
		gl3state.window = NULL;
	}
}

void GL3_StartFrame (void)
{
	SDL_GL_MakeCurrent (gl3state.window, gl3state.context);
}

void GL3_SwapBuffers (void)
{
	SDL_GL_SwapWindow (gl3state.window);
}
