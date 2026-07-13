// gl3_main.c -- entry point and refexport table for the GL 3.3 renderer.
// Stage 1: brings up the window/context and clears the screen. 2D, world and
// model drawing are filled in incrementally; unimplemented entry points are
// safe no-ops so the client runs throughout.

#include "gl3_local.h"

refimport_t	ri;

cvar_t	*gl_mode;
cvar_t	*vid_fullscreen;
cvar_t	*vid_gamma;
cvar_t	*gl_clear;

// ------------------------------------------------------------------ registration

static void GL3_BeginRegistration (char *map)		{ }
static struct model_s *GL3_RegisterModel (char *name)	{ return NULL; }
static struct image_s *GL3_RegisterSkin (char *name)	{ return NULL; }
static struct image_s *GL3_RegisterPic (char *name)	{ return NULL; }
static void GL3_SetSky (char *name, float rotate, vec3_t axis) { }
static void GL3_EndRegistration (void)				{ }

// ------------------------------------------------------------------ 2D drawing (stubs for now)

static void GL3_DrawGetPicSize (int *w, int *h, char *name)	{ if (w) *w = 0; if (h) *h = 0; }
static void GL3_DrawPic (int x, int y, char *name)			{ }
static void GL3_DrawStretchPic (int x, int y, int w, int h, char *name) { }
static void GL3_DrawChar (int x, int y, int c)				{ }
static void GL3_DrawTileClear (int x, int y, int w, int h, char *name) { }
static void GL3_DrawFill (int x, int y, int w, int h, int c) { }
static void GL3_DrawFadeScreen (void)						{ }
static void GL3_DrawStretchRaw (int x, int y, int w, int h, int cols, int rows, byte *data) { }
static void GL3_CinematicSetPalette (const unsigned char *palette) { }

// ------------------------------------------------------------------ frame

static void GL3_RenderFrame (refdef_t *fd)
{
	// world/entity rendering lands here in a later stage
}

static void GL3_BeginFrame (float camera_separation)
{
	GL3_StartFrame ();

	glViewport (0, 0, gl3state.width, gl3state.height);
	glClearColor (0.15f, 0.15f, 0.20f, 1.0f);
	glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

static void GL3_EndFrame (void)
{
	GL3_SwapBuffers ();
}

static void GL3_AppActivate (qboolean active)
{
}

// ------------------------------------------------------------------ init / shutdown

static int GL3_Init (void *hinstance, void *wndproc)
{
	ri.Con_Printf (PRINT_ALL, "------- ref_gl3 (OpenGL 3.3 core) -------\n");

	gl_mode = ri.Cvar_Get ("gl_mode", "3", CVAR_ARCHIVE);
	vid_fullscreen = ri.Cvar_Get ("vid_fullscreen", "0", CVAR_ARCHIVE);
	vid_gamma = ri.Cvar_Get ("vid_gamma", "1", CVAR_ARCHIVE);
	gl_clear = ri.Cvar_Get ("gl_clear", "0", 0);

	if (!GL3_SetMode ((int)gl_mode->value, vid_fullscreen->value != 0))
	{
		ri.Con_Printf (PRINT_ALL, "ref_gl3: failed to set video mode\n");
		return -1;
	}

	ri.Con_Printf (PRINT_ALL, "----------------------------------------\n");
	return 1;	// success (client only treats -1 as failure)
}

static void GL3_Shutdown (void)
{
	GL3_ShutdownWindow ();
}

// ------------------------------------------------------------------ export

Q2_DLL_EXPORT refexport_t GetRefAPI (refimport_t rimp)
{
	refexport_t	re;

	ri = rimp;

	re.api_version = API_VERSION;

	re.Init = GL3_Init;
	re.Shutdown = GL3_Shutdown;

	re.BeginRegistration = GL3_BeginRegistration;
	re.RegisterModel = GL3_RegisterModel;
	re.RegisterSkin = GL3_RegisterSkin;
	re.RegisterPic = GL3_RegisterPic;
	re.SetSky = GL3_SetSky;
	re.EndRegistration = GL3_EndRegistration;

	re.RenderFrame = GL3_RenderFrame;

	re.DrawGetPicSize = GL3_DrawGetPicSize;
	re.DrawPic = GL3_DrawPic;
	re.DrawStretchPic = GL3_DrawStretchPic;
	re.DrawChar = GL3_DrawChar;
	re.DrawTileClear = GL3_DrawTileClear;
	re.DrawFill = GL3_DrawFill;
	re.DrawFadeScreen = GL3_DrawFadeScreen;
	re.DrawStretchRaw = GL3_DrawStretchRaw;

	re.CinematicSetPalette = GL3_CinematicSetPalette;
	re.BeginFrame = GL3_BeginFrame;
	re.EndFrame = GL3_EndFrame;
	re.AppActivate = GL3_AppActivate;

	return re;
}
