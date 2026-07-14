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
cvar_t	*gl_wateralpha;
cvar_t	*gl_slimealpha;
cvar_t	*gl_2dscale;
cvar_t	*gl_anisotropy;
cvar_t	*gl_shadows;
cvar_t	*gl_retexture;
cvar_t	*cache_assets;
cvar_t	*gl_bump;
cvar_t	*r_lefthand;
cvar_t	*gl_flashblend;
cvar_t	*r_fullbright;
cvar_t	*gl_lightmap;
cvar_t	*r_speeds;
cvar_t	*gl_texturemode;
cvar_t	*r_drawentities;
cvar_t	*r_lightlevel;	// HACK: server reads this for monster sight (FindTarget)

void R_LightPoint (vec3_t p, vec3_t color);				// gl3_mesh.c
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

static void GL3_DrawEntity (entity_t *e)
{
	if (e->flags & RF_BEAM)
	{
		GL3_DrawBeam (e, gl3_viewproj);
		return;
	}
	if (!e->model)
	{
		GL3_DrawNullModel (e, gl3_viewproj);
		return;
	}

	switch (e->model->type)
	{
	case mod_alias:
		GL3_DrawAliasModel (e, gl3_viewproj);
		break;
	case mod_brush:
		GL3_DrawBrushModel (e, gl3_viewproj);
		break;
	case mod_sprite:
		GL3_DrawSpriteModel (e, gl3_viewproj);
		break;
	default:
		break;
	}
}

static void GL3_DrawEntities (void)
{
	int	i;

	if (!r_drawentities || !r_drawentities->value)
		return;

	// solid entities first, then translucent ones without depth writes
	// (matches R_DrawEntitiesOnList)
	for (i = 0; i < r_newrefdef.num_entities; i++)
	{
		entity_t	*e = &r_newrefdef.entities[i];
		if (!(e->flags & RF_TRANSLUCENT))
			GL3_DrawEntity (e);
	}

	glDepthMask (GL_FALSE);
	for (i = 0; i < r_newrefdef.num_entities; i++)
	{
		entity_t	*e = &r_newrefdef.entities[i];
		if (e->flags & RF_TRANSLUCENT)
			GL3_DrawEntity (e);
	}
	glDepthMask (GL_TRUE);
}

// Sample the world light at the view origin into the r_lightlevel cvar.
// The client copies it into usercmd_t.lightlevel each frame and the game's
// FindTarget treats light_level <= 5 as "too dark to be seen" -- without
// this monsters never sight the player (id's gl_rmain.c R_SetLightLevel,
// "save off light value for server to look at (BIG HACK!)").
static void GL3_SetLightLevel (void)
{
	vec3_t	shadelight;

	if (r_newrefdef.rdflags & RDF_NOWORLDMODEL)
		return;

	R_LightPoint (r_newrefdef.vieworg, shadelight);

	// pick the greatest component, which should be the same
	// as the mono value returned by software
	if (shadelight[0] > shadelight[1])
	{
		if (shadelight[0] > shadelight[2])
			r_lightlevel->value = 150 * shadelight[0];
		else
			r_lightlevel->value = 150 * shadelight[2];
	}
	else
	{
		if (shadelight[1] > shadelight[2])
			r_lightlevel->value = 150 * shadelight[1];
		else
			r_lightlevel->value = 150 * shadelight[2];
	}
}

static void GL3_RenderFrame (refdef_t *fd)
{
	float	mvp[16];

	r_newrefdef = *fd;
	r_framecount++;
	c_brush_polys = 0;
	c_alias_polys = 0;

	if (!r_worldmodel && !(fd->rdflags & RDF_NOWORLDMODEL))
		ri.Sys_Error (ERR_DROP, "GL3_RenderFrame: NULL worldmodel");

	// the 3D view renders into the offscreen scene target
	GL3_Post_BeginScene ();

	// viewport (GL y is bottom-up); the refdef is in the client's virtual
	// 2D resolution, so scale it to scene-FBO pixels
	float fbs = GL3_Post_FrameScale ();
	glViewport ((int)(r_newrefdef.x * fbs),
		GL3_Post_Height () - (int)((r_newrefdef.y + r_newrefdef.height) * fbs),
		(int)(r_newrefdef.width * fbs), (int)(r_newrefdef.height * fbs));

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
	glUniform1i (gl3_prog3d.u_lightmode,
		r_fullbright->value ? 1 : gl_lightmap->value ? 2 : 0);
	glActiveTexture (GL_TEXTURE0);

	GL3_SetFrustum ();

	if (r_newrefdef.rdflags & RDF_NOWORLDMODEL)
	{
		// no-world scene (player-setup preview): id clears the sub-view to
		// grey and draws no sky or world, just the entities
		glEnable (GL_SCISSOR_TEST);
		glScissor ((int)(r_newrefdef.x * fbs),
			GL3_Post_Height () - (int)((r_newrefdef.y + r_newrefdef.height) * fbs),
			(int)(r_newrefdef.width * fbs), (int)(r_newrefdef.height * fbs));
		glClearColor (0.3f, 0.3f, 0.3f, 1.0f);
		glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		glClearColor (0.0f, 0.0f, 0.0f, 1.0f);
		glDisable (GL_SCISSOR_TEST);

		GL3_DrawEntities ();
	}
	else
	{
		GL3_MarkLeaves ();
		GL3_PushDlights ();
		GL3_DrawSkyBox (gl3_viewproj, r_newrefdef.vieworg);	// background, before the world
		GL3_DrawWorld ();
		GL3_DrawWater (gl3_viewproj, r_newrefdef.time);
		GL3_DrawEntities ();
		GL3_RenderDlights (gl3_viewproj);	// gl_flashblend glow balls
		GL3_DrawParticles (gl3_viewproj);	// id: particles BEFORE alpha
											// surfaces, so glass tints them
		GL3_DrawWorldTranslucent ();		// glass / force fields, blended
	}

	GL3_SetLightLevel ();

	if (r_speeds->value)
		ri.Con_Printf (PRINT_ALL, "%4i wpoly %4i epoly %2i dlights\n",
			c_brush_polys, c_alias_polys, r_newrefdef.num_dlights);

	// resolve + post-process (gamma, underwater warp, bloom) to the window
	glDisable (GL_DEPTH_TEST);
	glDisable (GL_CULL_FACE);
	GL3_Post_EndScene ();

	// restore 2D state for the HUD/console the client draws next
	GL3_Draw_SetOrtho ();

	// full-screen damage/pickup/underwater colour wash
	if (r_newrefdef.blend[3] != 0)
		GL3_Draw_PolyBlend (r_newrefdef.blend[0], r_newrefdef.blend[1],
			r_newrefdef.blend[2], r_newrefdef.blend[3]);
}

// Anaglyph stereo: with cl_stereo the client runs the whole draw path twice
// per frame with opposite camera offsets (id targeted quad-buffered hardware
// stereo, long dead). Composite in the window's channels instead -- left eye
// in red, right eye in green+blue, for standard red/cyan glasses. Scene-FBO
// rendering runs unmasked; the mask applies only to window-target drawing.
void GL3_ApplyStereoMask (void)
{
	if (gl3state.stereo_eye < 0)
		glColorMask (GL_TRUE, GL_FALSE, GL_FALSE, GL_TRUE);
	else if (gl3state.stereo_eye > 0)
		glColorMask (GL_FALSE, GL_TRUE, GL_TRUE, GL_TRUE);
	else
		glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
}

static void GL3_BeginFrame (float camera_separation)
{
	GL3_StartFrame ();

	GL3_CheckWindowChanges ();	// vid_fullscreen / gl_mode / gl_2dscale

	gl3state.stereo_eye = (camera_separation < 0) ? -1
		: (camera_separation > 0) ? 1 : 0;

	glViewport (0, 0, gl3state.width, gl3state.height);
	glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glClearColor (0.0f, 0.0f, 0.0f, 1.0f);
	if (gl3state.stereo_eye > 0)
		glClear (GL_DEPTH_BUFFER_BIT);	// right pass: keep the left eye's channels
	else
		glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	GL3_ApplyStereoMask ();

	// prepare for 2D drawing (menu/console when no 3D view is rendered)
	GL3_Draw_SetOrtho ();
}

static void GL3_EndFrame (void)
{
	glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
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
	gl_wateralpha = ri.Cvar_Get ("gl_wateralpha", "0.75", CVAR_ARCHIVE);
	gl_slimealpha = ri.Cvar_Get ("gl_slimealpha", "1", CVAR_ARCHIVE);	// acid: opaque like vanilla
	gl_2dscale = ri.Cvar_Get ("gl_2dscale", "0", CVAR_ARCHIVE);	// 0 = auto
	gl_anisotropy = ri.Cvar_Get ("gl_anisotropy", "8", CVAR_ARCHIVE);
	gl_shadows = ri.Cvar_Get ("gl_shadows", "1", CVAR_ARCHIVE);	// soft blob shadows
	gl_retexture = ri.Cvar_Get ("gl_retexture", "1", CVAR_ARCHIVE);	// hi-res texture packs
	cache_assets = ri.Cvar_Get ("cache_assets", "1", CVAR_ARCHIVE);	// assets persist across maps
	gl_bump = ri.Cvar_Get ("gl_bump", "2", CVAR_ARCHIVE);	// bump under dlights; 2 = auto-generate maps
	r_lefthand = ri.Cvar_Get ("hand", "0", CVAR_USERINFO | CVAR_ARCHIVE);
	gl_flashblend = ri.Cvar_Get ("gl_flashblend", "0", 0);
	r_fullbright = ri.Cvar_Get ("r_fullbright", "0", 0);
	gl_lightmap = ri.Cvar_Get ("gl_lightmap", "0", 0);
	r_speeds = ri.Cvar_Get ("r_speeds", "0", 0);
	gl_texturemode = ri.Cvar_Get ("gl_texturemode", "GL_LINEAR_MIPMAP_LINEAR", CVAR_ARCHIVE);
	r_drawentities = ri.Cvar_Get ("r_drawentities", "1", 0);
	r_lightlevel = ri.Cvar_Get ("r_lightlevel", "0", 0);

	Swap_Init ();	// the renderer has its own copy of q_shared's byte-swap pointers

	if (!GL3_SetMode ((int)gl_mode->value, vid_fullscreen->value != 0))
	{
		ri.Con_Printf (PRINT_ALL, "ref_gl3: failed to set video mode\n");
		return -1;
	}

	GL3_InitImages ();
	GL3_CreateNoTexture ();
	GL3_InitShaders ();
	GL3_Post_Init ();
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
	GL3_ShutdownSurf ();
	GL3_ShutdownMesh ();
	GL3_ShutdownParticles ();
	GL3_ShutdownSky ();
	GL3_Draw_Shutdown ();
	GL3_Post_Shutdown ();
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
