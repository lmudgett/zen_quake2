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
cvar_t	*gl_intensity;

void GL3_SetRawPalette (const unsigned char *palette);	// gl3_draw.c
void GL3_ScreenShot_f (void);							// gl3_screenshot.c
void GL3_ScreenShot_Capture (void);						// gl3_screenshot.c

// q_shared.c (compiled into the renderer) calls Com_Printf; forward it to
// the engine's console via the import table.
void Com_Printf (char *fmt, ...)
{
	va_list	argptr;
	char	msg[2048];

	va_start (argptr, fmt);
	vsnprintf (msg, sizeof(msg), fmt, argptr);
	va_end (argptr);

	ri.Con_Printf (PRINT_ALL, "%s", msg);
}

// ------------------------------------------------------------------ registration

static void GL3_BeginRegistration (char *map)		{ }
static struct model_s *GL3_RegisterModel (char *name)	{ return NULL; }
static struct image_s *GL3_RegisterSkin (char *name)	{ return NULL; }
static struct image_s *GL3_RegisterPic (char *name)	{ return NULL; }
static void GL3_SetSky (char *name, float rotate, vec3_t axis) { }
static void GL3_EndRegistration (void)				{ }

// ------------------------------------------------------------------ 2D drawing

static void GL3_CinematicSetPalette (const unsigned char *palette)
{
	GL3_SetRawPalette (palette);
}

// ------------------------------------------------------------------ frame

static void GL3_RenderFrame (refdef_t *fd)
{
	// world/entity rendering lands here in a later stage
}

static void GL3_BeginFrame (float camera_separation)
{
	GL3_StartFrame ();

	glViewport (0, 0, gl3state.width, gl3state.height);
	glClearColor (0.0f, 0.0f, 0.0f, 1.0f);
	glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	// prepare for the frame's 2D drawing (HUD/console/menu draw after RenderFrame)
	GL3_Draw_SetOrtho ();
}

static void GL3_EndFrame (void)
{
	GL3_ScreenShot_Capture ();	// grab the back buffer before it is swapped
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
	gl_intensity = ri.Cvar_Get ("intensity", "2", CVAR_ARCHIVE);

	Swap_Init ();	// the renderer has its own copy of q_shared's byte-swap pointers

	if (!GL3_SetMode ((int)gl_mode->value, vid_fullscreen->value != 0))
	{
		ri.Con_Printf (PRINT_ALL, "ref_gl3: failed to set video mode\n");
		return -1;
	}

	GL3_InitImages ();
	GL3_InitShaders ();
	GL3_Draw_Init ();

	ri.Cmd_AddCommand ("imagelist", GL3_ImageList_f);
	ri.Cmd_AddCommand ("screenshot", GL3_ScreenShot_f);

	ri.Con_Printf (PRINT_ALL, "----------------------------------------\n");
	return 1;	// success (client only treats -1 as failure)
}

static void GL3_Shutdown (void)
{
	ri.Cmd_RemoveCommand ("imagelist");
	GL3_Draw_Shutdown ();
	GL3_ShutdownShaders ();
	GL3_ShutdownImages ();
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

	re.DrawGetPicSize = GL3_Draw_GetPicSize;
	re.DrawPic = GL3_Draw_Pic;
	re.DrawStretchPic = GL3_Draw_StretchPic;
	re.DrawChar = GL3_Draw_Char;
	re.DrawTileClear = GL3_Draw_TileClear;
	re.DrawFill = GL3_Draw_Fill;
	re.DrawFadeScreen = GL3_Draw_FadeScreen;
	re.DrawStretchRaw = GL3_Draw_StretchRaw;

	re.CinematicSetPalette = GL3_CinematicSetPalette;
	re.BeginFrame = GL3_BeginFrame;
	re.EndFrame = GL3_EndFrame;
	re.AppActivate = GL3_AppActivate;

	return re;
}
