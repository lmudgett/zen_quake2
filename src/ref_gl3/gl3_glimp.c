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

	GL3_UpdateViddef ();

	return true;
}

/*
=================
GL3_UpdateViddef

Compute the 2D virtual resolution from the drawable size and gl_2dscale and
report it to the client. All 2D drawing (HUD/console/menus/cinematics)
happens in vw x vh coordinates and is scaled up by the ortho projection, so
the UI stays readable at high resolutions.
=================
*/
void GL3_UpdateViddef (void)
{
	int	scale = (int)gl_2dscale->value;

	if (scale < 1)
	{	// auto: integer scale that keeps the UI near a 600-line layout
		scale = (int)(gl3state.height / 600.0f + 0.5f);
		if (scale < 1)
			scale = 1;
	}
	gl3state.scale = scale;
	gl3state.vw = (gl3state.width + scale - 1) / scale;
	gl3state.vh = (gl3state.height + scale - 1) / scale;

	glViewport (0, 0, gl3state.width, gl3state.height);
	ri.Vid_NewWindow (gl3state.vw, gl3state.vh);
}

/*
=================
GL3_CheckWindowChanges

Apply vid_fullscreen / gl_mode / gl_2dscale changes at the top of the frame.
SDL3 toggles fullscreen and resizes live, so the GL context -- and with it
every texture, lightmap and VBO -- survives; no vid_restart needed.
=================
*/
void GL3_CheckWindowChanges (void)
{
	int	width, height;

	if (!gl3state.window)
		return;

	if (vid_fullscreen->modified || gl_mode->modified)
	{
		qboolean fullscreen = vid_fullscreen->value != 0;

		vid_fullscreen->modified = false;
		gl_mode->modified = false;

		SDL_SetWindowFullscreen (gl3state.window, fullscreen ? true : false);
		if (!fullscreen)
		{
			if (ri.Vid_GetModeInfo (&width, &height, (int)gl_mode->value))
				SDL_SetWindowSize (gl3state.window, width, height);
			else
				ri.Con_Printf (PRINT_ALL, "GL3_CheckWindowChanges: invalid mode %d\n",
					(int)gl_mode->value);
		}
		SDL_SyncWindow (gl3state.window);

		SDL_GetWindowSizeInPixels (gl3state.window, &gl3state.width, &gl3state.height);
		gl3state.fullscreen = fullscreen;
		GL3_UpdateViddef ();
	}
	else if (gl_2dscale->modified)
	{
		GL3_UpdateViddef ();
	}
	gl_2dscale->modified = false;
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
