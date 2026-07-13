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
cvar_t	*r_drawentities;

void GL3_SetRawPalette (const unsigned char *palette);	// gl3_draw.c
void GL3_ScreenShot_f (void);							// gl3_screenshot.c
void GL3_ScreenShot_Capture (void);						// gl3_screenshot.c

image_t	*r_notexture;

// build a small magenta/black checkerboard used when a texture is missing
static void GL3_CreateNoTexture (void)
{
	byte	data[16 * 16 * 4];
	int		x, y;

	for (y = 0; y < 16; y++)
		for (x = 0; x < 16; x++)
		{
			byte	*p = data + (y * 16 + x) * 4;
			byte	c = ((x >> 3) ^ (y >> 3)) ? 255 : 0;
			p[0] = c; p[1] = 0; p[2] = c; p[3] = 255;
		}

	r_notexture = GL3_LoadPic ("***r_notexture***", data, 16, 16, it_wall, 32);
}

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
// GL3_BeginRegistration / RegisterModel / RegisterSkin / EndRegistration live
// in gl3_model.c. Pics come from the image manager.

refdef_t	r_newrefdef;
float		gl3_viewproj[16];		// proj * view for the current frame

static struct image_s *GL3_RegisterPic (char *name)
{
	return GL3_Draw_FindPic (name);
}

// GL3_SetSky lives in gl3_sky.c

// ------------------------------------------------------------------ 2D drawing

static void GL3_CinematicSetPalette (const unsigned char *palette)
{
	GL3_SetRawPalette (palette);
}

// ------------------------------------------------------------------ frame

// Build the view's model-view-projection from the refdef, matching the
// original R_SetupGL orientation conversion.
static void GL3_SetupWorldMatrix (float *mvp)
{
	float	proj[16], mv[16], tmp[16], r[16];
	float	aspect = (float)r_newrefdef.width / (float)r_newrefdef.height;

	GL3_MatPerspective (proj, r_newrefdef.fov_y, aspect, 4.0f, 4096.0f);

	// modelview: convert Quake axes to GL, then inverse camera transform
	GL3_MatIdentity (mv);
	GL3_MatRotate (tmp, -90, 1, 0, 0);  GL3_MatMul (mv, mv, tmp);
	GL3_MatRotate (tmp,  90, 0, 0, 1);  GL3_MatMul (mv, mv, tmp);
	GL3_MatRotate (tmp, -r_newrefdef.viewangles[2], 1, 0, 0); GL3_MatMul (mv, mv, tmp);
	GL3_MatRotate (tmp, -r_newrefdef.viewangles[0], 0, 1, 0); GL3_MatMul (mv, mv, tmp);
	GL3_MatRotate (tmp, -r_newrefdef.viewangles[1], 0, 0, 1); GL3_MatMul (mv, mv, tmp);
	GL3_MatTranslate (tmp, -r_newrefdef.vieworg[0], -r_newrefdef.vieworg[1], -r_newrefdef.vieworg[2]);
	GL3_MatMul (mv, mv, tmp);

	GL3_MatMul (mvp, proj, mv);
	memcpy (gl3_viewproj, mvp, sizeof(gl3_viewproj));	// entities post-multiply their model matrix
	(void)r;
}

static void GL3_DrawEntities (void)
{
	int	i;

	if (!r_drawentities || !r_drawentities->value)
		return;

	for (i = 0; i < r_newrefdef.num_entities; i++)
	{
		entity_t	*e = &r_newrefdef.entities[i];

		if (e->flags & RF_BEAM)
			continue;			// beams drawn later
		if (!e->model)
			continue;			// null-model boxes later

		switch (e->model->type)
		{
		case mod_alias:
			GL3_DrawAliasModel (e, gl3_viewproj);
			break;
		case mod_brush:
			GL3_DrawBrushModel (e, gl3_viewproj);
			break;
		case mod_sprite:
		default:
			break;				// sprites in a later sub-stage
		}
	}
}

static void GL3_RenderFrame (refdef_t *fd)
{
	float	mvp[16];

	r_newrefdef = *fd;
	r_framecount++;

	if (!r_worldmodel && !(fd->rdflags & RDF_NOWORLDMODEL))
		ri.Sys_Error (ERR_DROP, "GL3_RenderFrame: NULL worldmodel");

	// viewport (GL y is bottom-up)
	glViewport (r_newrefdef.x,
		gl3state.height - (r_newrefdef.y + r_newrefdef.height),
		r_newrefdef.width, r_newrefdef.height);

	glEnable (GL_DEPTH_TEST);
	glDepthFunc (GL_LEQUAL);
	glEnable (GL_CULL_FACE);
	glCullFace (GL_FRONT);			// Quake winding
	glDisable (GL_BLEND);

	GL3_SetupWorldMatrix (mvp);

	glUseProgram (gl3_prog3d.program);
	glUniformMatrix4fv (gl3_prog3d.u_mvp, 1, GL_FALSE, mvp);
	glUniform1f (gl3_prog3d.u_gamma, vid_gamma->value < 0.5f ? 0.5f : vid_gamma->value);
	glUniform1f (gl3_prog3d.u_intensity, gl_intensity->value);
	glUniform1i (gl3_prog3d.u_lm_enabled, 0);	// diffuse-only for now
	glActiveTexture (GL_TEXTURE0);

	GL3_MarkLeaves ();
	GL3_DrawSkyBox (gl3_viewproj, r_newrefdef.vieworg);	// background, before the world
	GL3_DrawWorld ();
	GL3_DrawEntities ();
	GL3_DrawParticles (gl3_viewproj);

	// restore 2D state for the HUD/console the client draws next
	glDisable (GL_DEPTH_TEST);
	glDisable (GL_CULL_FACE);
	GL3_Draw_SetOrtho ();
}

static void GL3_BeginFrame (float camera_separation)
{
	GL3_StartFrame ();

	glViewport (0, 0, gl3state.width, gl3state.height);
	glClearColor (0.0f, 0.0f, 0.0f, 1.0f);
	glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	// prepare for 2D drawing (menu/console when no 3D view is rendered)
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
	r_drawentities = ri.Cvar_Get ("r_drawentities", "1", 0);

	Swap_Init ();	// the renderer has its own copy of q_shared's byte-swap pointers

	if (!GL3_SetMode ((int)gl_mode->value, vid_fullscreen->value != 0))
	{
		ri.Con_Printf (PRINT_ALL, "ref_gl3: failed to set video mode\n");
		return -1;
	}

	GL3_InitImages ();
	GL3_CreateNoTexture ();
	GL3_InitShaders ();
	GL3_Draw_Init ();
	GL3_InitMesh ();
	GL3_InitParticles ();
	GL3_InitSky ();
	GL3_Mod_Init ();

	ri.Cmd_AddCommand ("imagelist", GL3_ImageList_f);
	ri.Cmd_AddCommand ("modellist", GL3_Mod_Modellist_f);
	ri.Cmd_AddCommand ("screenshot", GL3_ScreenShot_f);

	ri.Con_Printf (PRINT_ALL, "----------------------------------------\n");
	return 1;	// success (client only treats -1 as failure)
}

static void GL3_Shutdown (void)
{
	ri.Cmd_RemoveCommand ("imagelist");
	ri.Cmd_RemoveCommand ("modellist");
	ri.Cmd_RemoveCommand ("screenshot");
	GL3_Mod_FreeAll ();
	GL3_ShutdownMesh ();
	GL3_ShutdownParticles ();
	GL3_ShutdownSky ();
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
