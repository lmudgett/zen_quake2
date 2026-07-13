// gl3_surf.c -- world (BSP brush) surface rendering. Builds one static VBO
// from all world polygons at registration time and draws visible surfaces
// with the 3D shader. (Lightmaps are added in a follow-up; this first cut
// renders diffuse-only so geometry can be verified.)

#include "gl3_local.h"

// r_worldmodel is defined in gl3_model.c (declared extern in gl3_model.h)

static GLuint	world_vao, world_vbo;
static int		world_numverts;

int		c_brush_polys;
int		r_framecount = 1;
int		r_visframecount;

static cvar_t	*gl_modulate;

// ------------------------------------------------------------------ lightmaps

#define	BLOCK_WIDTH		128
#define	BLOCK_HEIGHT	128
#define	LIGHTMAP_BYTES	4
#define	MAX_LIGHTMAPS	128

typedef struct
{
	int		current_lightmap_texture;			// 1..N (0 reserved for dynamic)
	int		allocated[BLOCK_WIDTH];
	byte	lightmap_buffer[4 * BLOCK_WIDTH * BLOCK_HEIGHT];
} gllightmapstate_t;

static gllightmapstate_t	gl_lms;
static GLuint				gl3_lightmap_tex[MAX_LIGHTMAPS];
static float				s_blocklights[BLOCK_WIDTH * BLOCK_HEIGHT * 3];
static lightstyle_t			gl3_lightstyles[MAX_LIGHTSTYLES];

static void LM_InitBlock (void)
{
	memset (gl_lms.allocated, 0, sizeof(gl_lms.allocated));
}

// returns a page position; false if the block is full
static qboolean LM_AllocBlock (int w, int h, int *x, int *y)
{
	int	i, j, best, best2;

	best = BLOCK_HEIGHT;
	for (i = 0; i < BLOCK_WIDTH - w; i++)
	{
		best2 = 0;
		for (j = 0; j < w; j++)
		{
			if (gl_lms.allocated[i + j] >= best)
				break;
			if (gl_lms.allocated[i + j] > best2)
				best2 = gl_lms.allocated[i + j];
		}
		if (j == w)
		{
			*x = i;
			*y = best = best2;
		}
	}

	if (best + h > BLOCK_HEIGHT)
		return false;

	for (i = 0; i < w; i++)
		gl_lms.allocated[*x + i] = best + h;

	return true;
}

// upload the current page as a new GL texture
static void LM_UploadBlock (void)
{
	int	tex = gl_lms.current_lightmap_texture;

	glGenTextures (1, &gl3_lightmap_tex[tex]);
	glBindTexture (GL_TEXTURE_2D, gl3_lightmap_tex[tex]);
	gl3state.currenttexture = -1;
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, BLOCK_WIDTH, BLOCK_HEIGHT, 0,
		GL_RGBA, GL_UNSIGNED_BYTE, gl_lms.lightmap_buffer);

	if (++gl_lms.current_lightmap_texture == MAX_LIGHTMAPS)
		ri.Sys_Error (ERR_DROP, "LM_UploadBlock: MAX_LIGHTMAPS exceeded");
}

// decode a surface's stored light samples (+ lightstyles) into RGBA texels
static void R_BuildLightMap (msurface_t *surf, byte *dest, int stride)
{
	int		smax, tmax, size, i, j, maps, nummaps;
	byte	*lightmap;
	float	*bl, scale[3];
	float	modulate = gl_modulate ? gl_modulate->value : 1.0f;

	smax = (surf->extents[0] >> 4) + 1;
	tmax = (surf->extents[1] >> 4) + 1;
	size = smax * tmax;

	if (size * 3 > (int)(sizeof(s_blocklights) / sizeof(s_blocklights[0])))
		size = (int)(sizeof(s_blocklights) / sizeof(s_blocklights[0])) / 3;

	if (!surf->samples)
	{
		for (i = 0; i < size * 3; i++)
			s_blocklights[i] = 255;
	}
	else
	{
		for (nummaps = 0; nummaps < MAXLIGHTMAPS && surf->styles[nummaps] != 255; nummaps++)
			;
		lightmap = surf->samples;
		memset (s_blocklights, 0, sizeof(s_blocklights[0]) * size * 3);

		for (maps = 0; maps < MAXLIGHTMAPS && surf->styles[maps] != 255; maps++)
		{
			bl = s_blocklights;
			for (i = 0; i < 3; i++)
				scale[i] = modulate * gl3_lightstyles[surf->styles[maps]].rgb[i];
			for (i = 0; i < size; i++, bl += 3)
			{
				bl[0] += lightmap[i * 3 + 0] * scale[0];
				bl[1] += lightmap[i * 3 + 1] * scale[1];
				bl[2] += lightmap[i * 3 + 2] * scale[2];
			}
			lightmap += size * 3;
		}
	}

	// store as RGBA, rescaling if the brightest channel exceeds 255
	stride -= smax * 4;
	bl = s_blocklights;
	for (i = 0; i < tmax; i++, dest += stride)
	{
		for (j = 0; j < smax; j++, bl += 3, dest += 4)
		{
			int r = (int)bl[0], g = (int)bl[1], b = (int)bl[2], max;
			if (r < 0) r = 0;
			if (g < 0) g = 0;
			if (b < 0) b = 0;
			max = r > g ? r : g;
			if (b > max) max = b;
			if (max > 255)
			{
				float t = 255.0f / max;
				r = (int)(r * t); g = (int)(g * t); b = (int)(b * t);
			}
			dest[0] = r; dest[1] = g; dest[2] = b; dest[3] = 255;
		}
	}
}

void GL3_BeginBuildingLightmaps (model_t *m)
{
	int	i;

	memset (gl_lms.allocated, 0, sizeof(gl_lms.allocated));
	r_framecount = 1;

	if (!gl_modulate)
		gl_modulate = ri.Cvar_Get ("gl_modulate", "1", CVAR_ARCHIVE);

	// default lightstyles so surfaces build correctly before the game sends any
	for (i = 0; i < MAX_LIGHTSTYLES; i++)
	{
		gl3_lightstyles[i].rgb[0] = 1;
		gl3_lightstyles[i].rgb[1] = 1;
		gl3_lightstyles[i].rgb[2] = 1;
		gl3_lightstyles[i].white = 3;
	}

	gl_lms.current_lightmap_texture = 1;
}

void GL3_CreateSurfaceLightmap (msurface_t *surf)
{
	int		smax, tmax;
	byte	*base;

	if (surf->flags & (SURF_DRAWSKY | SURF_DRAWTURB))
	{
		surf->lightmaptexturenum = 0;
		return;
	}

	smax = (surf->extents[0] >> 4) + 1;
	tmax = (surf->extents[1] >> 4) + 1;

	if (!LM_AllocBlock (smax, tmax, &surf->light_s, &surf->light_t))
	{
		LM_UploadBlock ();
		LM_InitBlock ();
		if (!LM_AllocBlock (smax, tmax, &surf->light_s, &surf->light_t))
			ri.Sys_Error (ERR_FATAL, "LM_AllocBlock(%d,%d) failed", smax, tmax);
	}

	surf->lightmaptexturenum = gl_lms.current_lightmap_texture;

	base = gl_lms.lightmap_buffer;
	base += (surf->light_t * BLOCK_WIDTH + surf->light_s) * LIGHTMAP_BYTES;
	R_BuildLightMap (surf, base, BLOCK_WIDTH * LIGHTMAP_BYTES);
}

void GL3_EndBuildingLightmaps (void)
{
	LM_UploadBlock ();	// flush the final page
}

// ------------------------------------------------------------------ world VBO

// count polygon verts across every world surface
static int GL3_CountWorldVerts (void)
{
	int	i, total = 0;
	msurface_t	*surf = r_worldmodel->surfaces;

	for (i = 0; i < r_worldmodel->numsurfaces; i++, surf++)
	{
		glpoly_t *p;
		for (p = surf->polys; p; p = p->next)
			total += p->numverts;
	}
	return total;
}

void GL3_BuildWorldVBO (void)
{
	int		i, v = 0;
	float	*data, *dst;
	msurface_t	*surf;

	if (!r_worldmodel)
		return;

	world_numverts = GL3_CountWorldVerts ();
	if (!world_numverts)
		return;

	data = malloc (world_numverts * VERTEXSIZE * sizeof(float));
	dst = data;

	surf = r_worldmodel->surfaces;
	for (i = 0; i < r_worldmodel->numsurfaces; i++, surf++)
	{
		glpoly_t *p;
		for (p = surf->polys; p; p = p->next)
		{
			p->vbo_firstvert = v;
			memcpy (dst, p->verts, p->numverts * VERTEXSIZE * sizeof(float));
			dst += p->numverts * VERTEXSIZE;
			v += p->numverts;
		}
	}

	glGenVertexArrays (1, &world_vao);
	glGenBuffers (1, &world_vbo);
	glBindVertexArray (world_vao);
	glBindBuffer (GL_ARRAY_BUFFER, world_vbo);
	glBufferData (GL_ARRAY_BUFFER, world_numverts * VERTEXSIZE * sizeof(float), data, GL_STATIC_DRAW);

	// layout: pos(3) uv(2) lmuv(2), stride VERTEXSIZE floats
	glEnableVertexAttribArray (0);
	glVertexAttribPointer (0, 3, GL_FLOAT, GL_FALSE, VERTEXSIZE * sizeof(float), (void *)0);
	glEnableVertexAttribArray (1);
	glVertexAttribPointer (1, 2, GL_FLOAT, GL_FALSE, VERTEXSIZE * sizeof(float), (void *)(3 * sizeof(float)));
	glEnableVertexAttribArray (2);
	glVertexAttribPointer (2, 2, GL_FLOAT, GL_FALSE, VERTEXSIZE * sizeof(float), (void *)(5 * sizeof(float)));

	free (data);

	ri.Con_Printf (PRINT_ALL, "world VBO: %d verts, %d surfaces\n",
		world_numverts, r_worldmodel->numsurfaces);
}

// ------------------------------------------------------------------ draw

void GL3_MarkLeaves (void)
{
	// PVS culling comes later; for now everything is considered visible
}

static image_t *GL3_TextureAnimation (mtexinfo_t *tex)
{
	if (!tex->next)
		return tex->image;
	// animated textures step by (int)(time*2); base frame is fine for a still
	return tex->image;
}

void GL3_DrawWorld (void)
{
	int		i;
	int		cur_lm = -1, cur_lm_enabled = -1;
	msurface_t	*surf;

	if (!r_worldmodel || !world_vao)
		return;

	glUniform1f (gl3_prog3d.u_alpha, 1.0f);
	glBindVertexArray (world_vao);

	surf = r_worldmodel->surfaces;
	for (i = 0; i < r_worldmodel->numsurfaces; i++, surf++)
	{
		image_t	*img;
		glpoly_t *p;
		int		lit;

		if (surf->flags & (SURF_DRAWSKY | SURF_DRAWTURB))
			continue;			// sky and water drawn in their own passes
		if (surf->texinfo && (surf->texinfo->flags & (SURF_TRANS33 | SURF_TRANS66)))
			continue;			// translucent surfaces drawn in a later pass

		img = surf->texinfo ? GL3_TextureAnimation (surf->texinfo) : NULL;
		if (!img)
			img = r_notexture;

		// lit surfaces sample their lightmap page on unit 1
		lit = !(surf->flags & SURF_DRAWTURB) && surf->lightmaptexturenum > 0;
		if (lit != cur_lm_enabled)
		{
			glUniform1i (gl3_prog3d.u_lm_enabled, lit);
			cur_lm_enabled = lit;
		}
		if (lit && surf->lightmaptexturenum != cur_lm)
		{
			glActiveTexture (GL_TEXTURE1);
			glBindTexture (GL_TEXTURE_2D, gl3_lightmap_tex[surf->lightmaptexturenum]);
			glActiveTexture (GL_TEXTURE0);
			cur_lm = surf->lightmaptexturenum;
		}

		GL3_Bind (img->texnum);		// diffuse on unit 0

		for (p = surf->polys; p; p = p->next)
		{
			glDrawArrays (GL_TRIANGLE_FAN, p->vbo_firstvert, p->numverts);
			c_brush_polys++;
		}
	}
}

/*
=================
GL3_DrawWater

Turb (water/lava/slime) surfaces: raw texcoords warped per-fragment by the
warp shader. Opaque unless the texinfo is also flagged translucent.
=================
*/
void GL3_DrawWater (const float *viewproj, float time)
{
	int			i;
	msurface_t	*surf;

	if (!r_worldmodel || !world_vao)
		return;

	glUseProgram (gl3_prog_warp.program);
	glUniformMatrix4fv (gl3_prog_warp.u_mvp, 1, GL_FALSE, viewproj);
	glUniform1f (gl3_prog_warp.u_time, time);
	glUniform1f (gl3_prog_warp.u_gamma, vid_gamma->value < 0.5f ? 0.5f : vid_gamma->value);
	glUniform1f (gl3_prog_warp.u_intensity, gl_intensity ? gl_intensity->value : 1.0f);
	glActiveTexture (GL_TEXTURE0);
	glBindVertexArray (world_vao);

	surf = r_worldmodel->surfaces;
	for (i = 0; i < r_worldmodel->numsurfaces; i++, surf++)
	{
		image_t		*img;
		glpoly_t	*p;

		if (!(surf->flags & SURF_DRAWTURB))
			continue;

		img = surf->texinfo ? surf->texinfo->image : NULL;
		if (!img)
			img = r_notexture;
		GL3_Bind (img->texnum);

		for (p = surf->polys; p; p = p->next)
			glDrawArrays (GL_TRIANGLE_FAN, p->vbo_firstvert, p->numverts);
	}
}

/*
=================
GL3_DrawWorldTranslucent

Second pass: TRANS33/TRANS66 surfaces (glass, force fields, some water)
drawn alpha-blended after the opaque world. Depth-tested but not written.
=================
*/
void GL3_DrawWorldTranslucent (void)
{
	int			i;
	msurface_t	*surf;

	if (!r_worldmodel || !world_vao)
		return;

	glUseProgram (gl3_prog3d.program);
	glUniform1i (gl3_prog3d.u_lm_enabled, 0);
	glEnable (GL_BLEND);
	glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDepthMask (GL_FALSE);
	glBindVertexArray (world_vao);

	surf = r_worldmodel->surfaces;
	for (i = 0; i < r_worldmodel->numsurfaces; i++, surf++)
	{
		image_t		*img;
		glpoly_t	*p;

		if (!surf->texinfo || !(surf->texinfo->flags & (SURF_TRANS33 | SURF_TRANS66)))
			continue;
		if (surf->flags & SURF_DRAWTURB)
			continue;			// translucent water handled by the water pass

		glUniform1f (gl3_prog3d.u_alpha,
			(surf->texinfo->flags & SURF_TRANS33) ? 0.33f : 0.66f);

		img = GL3_TextureAnimation (surf->texinfo);
		if (!img)
			img = r_notexture;
		GL3_Bind (img->texnum);

		for (p = surf->polys; p; p = p->next)
			glDrawArrays (GL_TRIANGLE_FAN, p->vbo_firstvert, p->numverts);
	}

	glUniform1f (gl3_prog3d.u_alpha, 1.0f);
	glDepthMask (GL_TRUE);
	glDisable (GL_BLEND);
}

// draw a single world surface (diffuse + lightmap) -- used for inline bmodels
static void GL3_DrawSurface (msurface_t *surf)
{
	image_t		*img;
	glpoly_t	*p;
	int			lit;

	if (surf->flags & SURF_DRAWSKY)
		return;

	img = surf->texinfo ? GL3_TextureAnimation (surf->texinfo) : NULL;
	if (!img)
		img = r_notexture;

	lit = !(surf->flags & SURF_DRAWTURB) && surf->lightmaptexturenum > 0;
	glUniform1i (gl3_prog3d.u_lm_enabled, lit);
	if (lit)
	{
		glActiveTexture (GL_TEXTURE1);
		glBindTexture (GL_TEXTURE_2D, gl3_lightmap_tex[surf->lightmaptexturenum]);
		glActiveTexture (GL_TEXTURE0);
	}
	GL3_Bind (img->texnum);

	for (p = surf->polys; p; p = p->next)
	{
		glDrawArrays (GL_TRIANGLE_FAN, p->vbo_firstvert, p->numverts);
		c_brush_polys++;
	}
}

/*
=================
GL3_DrawBrushModel

Inline brush models (doors, platforms, buttons) are submodels of the world:
their surfaces already live in the world VBO, so we just draw that surface
range with the entity's transform applied.
=================
*/
void GL3_DrawBrushModel (entity_t *e, const float *viewproj)
{
	model_t		*mod = (model_t *)e->model;
	float		model_mat[16], mvp[16], tmp[16];
	msurface_t	*surf;
	int			i;

	if (!mod || mod->nummodelsurfaces == 0)
		return;

	// model matrix: translate then yaw/pitch/roll (R_RotateForEntity)
	GL3_MatTranslate (model_mat, e->origin[0], e->origin[1], e->origin[2]);
	GL3_MatRotate (tmp, e->angles[1], 0, 0, 1); GL3_MatMul (model_mat, model_mat, tmp);
	GL3_MatRotate (tmp, e->angles[0], 0, 1, 0); GL3_MatMul (model_mat, model_mat, tmp);
	GL3_MatRotate (tmp, e->angles[2], 1, 0, 0); GL3_MatMul (model_mat, model_mat, tmp);
	GL3_MatMul (mvp, viewproj, model_mat);

	glUseProgram (gl3_prog3d.program);
	glUniformMatrix4fv (gl3_prog3d.u_mvp, 1, GL_FALSE, mvp);
	glBindVertexArray (world_vao);

	surf = r_worldmodel->surfaces + mod->firstmodelsurface;
	for (i = 0; i < mod->nummodelsurfaces; i++, surf++)
		GL3_DrawSurface (surf);
}
