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

cvar_t	*gl_modulate;		// also read by R_LightPoint (gl3_mesh.c)
static cvar_t	*r_novis;
static cvar_t	*r_nocull;

// view frustum for box culling (built each frame from the refdef)
static cplane_t	frustum[4];
static vec3_t	modelorg;			// viewer origin, for node facing tests

// PVS cluster tracking; -2 forces a re-mark on the first frame of a map
static int		r_viewcluster = -1, r_viewcluster2 = -1;
static int		r_oldviewcluster = -2, r_oldviewcluster2 = -2;

// ------------------------------------------------------------------ lightmaps

#define	BLOCK_WIDTH		128
#define	BLOCK_HEIGHT	128
#define	LIGHTMAP_BYTES	4
#define	MAX_LIGHTMAPS	128

#define	DLIGHT_CUTOFF	64		// intensity below this adds nothing (id value)

typedef struct
{
	int		current_lightmap_texture;			// 1..N (0 reserved for dynamic)
	int		allocated[BLOCK_WIDTH];
	byte	lightmap_buffer[4 * BLOCK_WIDTH * BLOCK_HEIGHT];
} gllightmapstate_t;

static gllightmapstate_t	gl_lms;
static GLuint				gl3_lightmap_tex[MAX_LIGHTMAPS];
static GLuint				gl3_lightmap_dyn;	// page for surfaces lit by dlights this frame
static float				s_blocklights[BLOCK_WIDTH * BLOCK_HEIGHT * 3];
static lightstyle_t			gl3_lightstyles[MAX_LIGHTSTYLES];
static cvar_t				*gl_dynamic;

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

/*
=================
R_AddDynamicLights

Add the falloff of every dlight touching this surface into s_blocklights
(from gl_light.c). Marked surfaces carry per-frame dlightbits.
=================
*/
static void R_AddDynamicLights (msurface_t *surf)
{
	int			lnum, i, smax, tmax, s, t, sd, td;
	float		fdist, frad, fminlight;
	vec3_t		impact, local;
	dlight_t	*dl;
	float		*bl;
	mtexinfo_t	*tex = surf->texinfo;

	smax = (surf->extents[0] >> 4) + 1;
	tmax = (surf->extents[1] >> 4) + 1;

	for (lnum = 0; lnum < r_newrefdef.num_dlights; lnum++)
	{
		if (!(surf->dlightbits & (1 << lnum)))
			continue;

		dl = &r_newrefdef.dlights[lnum];
		frad = dl->intensity;
		fdist = DotProduct (dl->origin, surf->plane->normal) - surf->plane->dist;
		frad -= (float)fabs (fdist);	// radius of the circle on the plane
		if (frad < DLIGHT_CUTOFF)
			continue;
		fminlight = frad - DLIGHT_CUTOFF;

		for (i = 0; i < 3; i++)
			impact[i] = dl->origin[i] - surf->plane->normal[i] * fdist;

		local[0] = DotProduct (impact, tex->vecs[0]) + tex->vecs[0][3] - surf->texturemins[0];
		local[1] = DotProduct (impact, tex->vecs[1]) + tex->vecs[1][3] - surf->texturemins[1];

		bl = s_blocklights;
		for (t = 0; t < tmax; t++)
		{
			td = (int)local[1] - t * 16;
			if (td < 0)
				td = -td;
			for (s = 0; s < smax; s++, bl += 3)
			{
				sd = (int)(local[0] - s * 16);
				if (sd < 0)
					sd = -sd;
				if (sd > td)
					fdist = sd + (td >> 1);
				else
					fdist = td + (sd >> 1);
				if (fdist < fminlight)
				{
					bl[0] += (frad - fdist) * dl->color[0];
					bl[1] += (frad - fdist) * dl->color[1];
					bl[2] += (frad - fdist) * dl->color[2];
				}
			}
		}
	}
}

/*
=================
GL3_MarkLights / GL3_PushDlights

Flag every world surface a dlight touches with this frame's dlightframe
and the light's bit, walking only the side of each node the light reaches.
=================
*/
static void GL3_MarkLights (dlight_t *light, int bit, mnode_t *node)
{
	cplane_t	*splitplane;
	float		dist;
	msurface_t	*surf;
	int			i;

	if (node->contents != -1)
		return;

	splitplane = node->plane;
	dist = DotProduct (light->origin, splitplane->normal) - splitplane->dist;

	if (dist > light->intensity - DLIGHT_CUTOFF)
	{
		GL3_MarkLights (light, bit, node->children[0]);
		return;
	}
	if (dist < -light->intensity + DLIGHT_CUTOFF)
	{
		GL3_MarkLights (light, bit, node->children[1]);
		return;
	}

	surf = r_worldmodel->surfaces + node->firstsurface;
	for (i = 0; i < node->numsurfaces; i++, surf++)
	{
		if (surf->dlightframe != r_framecount)
		{
			surf->dlightbits = 0;
			surf->dlightframe = r_framecount;
		}
		surf->dlightbits |= bit;
	}

	GL3_MarkLights (light, bit, node->children[0]);
	GL3_MarkLights (light, bit, node->children[1]);
}

void GL3_PushDlights (void)
{
	int			i;
	dlight_t	*dl;

	if (!gl_dynamic)
		gl_dynamic = ri.Cvar_Get ("gl_dynamic", "2", 0);
	if (!gl_dynamic->value || !r_worldmodel)
		return;
	if (gl_flashblend && gl_flashblend->value)
		return;		// dlights drawn as additive glow balls instead
	if (gl_dynamic->value == 2)
		return;		// per-pixel mode: no CPU lightmap rebuilds needed

	dl = r_newrefdef.dlights;
	for (i = 0; i < r_newrefdef.num_dlights; i++, dl++)
		GL3_MarkLights (dl, 1 << i, r_worldmodel->nodes);
}

/*
=================
GL3_UploadDlights

gl_dynamic 2: feed this frame's dynamic lights to the world shader for
per-pixel accumulation. `move` shifts the light origins into an inline
bmodel's surface space (world surfaces pass vec3_origin).
=================
*/
void GL3_UploadDlights (const vec3_t move)
{
	float		pos[MAX_DLIGHTS * 4];
	float		col[MAX_DLIGHTS * 3];
	dlight_t	*dl;
	int			i, n;

	if (!gl_dynamic)
		gl_dynamic = ri.Cvar_Get ("gl_dynamic", "2", 0);

	n = (gl_dynamic->value == 2) ? r_newrefdef.num_dlights : 0;
	if (gl_flashblend && gl_flashblend->value)
		n = 0;		// dlights drawn as additive glow balls instead
	if (n > MAX_DLIGHTS)
		n = MAX_DLIGHTS;

	dl = r_newrefdef.dlights;
	for (i = 0; i < n; i++, dl++)
	{
		pos[i * 4 + 0] = dl->origin[0] - move[0];
		pos[i * 4 + 1] = dl->origin[1] - move[1];
		pos[i * 4 + 2] = dl->origin[2] - move[2];
		pos[i * 4 + 3] = dl->intensity;
		col[i * 3 + 0] = dl->color[0];
		col[i * 3 + 1] = dl->color[1];
		col[i * 3 + 2] = dl->color[2];
	}

	glUniform1i (gl3_prog3d.u_num_dlights, n);
	if (n)
	{
		glUniform4fv (gl3_prog3d.u_dlights, n, pos);
		glUniform3fv (gl3_prog3d.u_dlcolors, n, col);
	}
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
		// fullbright: id skips the dynamic-light add entirely (goto store)
		for (i = 0; i < size * 3; i++)
			s_blocklights[i] = 255;
		goto store;
	}
	else
	{
		for (nummaps = 0; nummaps < MAXLIGHTMAPS && surf->styles[nummaps] != 255; nummaps++)
			;
		lightmap = surf->samples;
		memset (s_blocklights, 0, sizeof(s_blocklights[0]) * size * 3);

		for (maps = 0; maps < MAXLIGHTMAPS && surf->styles[maps] != 255; maps++)
		{
			// current animated style values when rendering; defaults at load
			lightstyle_t	*style = r_newrefdef.lightstyles
				? &r_newrefdef.lightstyles[surf->styles[maps]]
				: &gl3_lightstyles[surf->styles[maps]];

			bl = s_blocklights;
			for (i = 0; i < 3; i++)
				scale[i] = modulate * style->rgb[i];
			surf->cached_light[maps] = style->white;	// change detection

			for (i = 0; i < size; i++, bl += 3)
			{
				bl[0] += lightmap[i * 3 + 0] * scale[0];
				bl[1] += lightmap[i * 3 + 1] * scale[1];
				bl[2] += lightmap[i * 3 + 2] * scale[2];
			}
			lightmap += size * 3;
		}
	}

	// dynamic lights touching this surface (marked by GL3_PushDlights)
	if (surf->dlightframe == r_framecount)
		R_AddDynamicLights (surf);

store:
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

	// release the previous map's lightmap pages before allocating new ones
	// (current_lightmap_texture still holds the prior map's page count here)
	for (i = 1; i < gl_lms.current_lightmap_texture; i++)
	{
		if (gl3_lightmap_tex[i])
		{
			glDeleteTextures (1, &gl3_lightmap_tex[i]);
			gl3_lightmap_tex[i] = 0;
		}
	}

	memset (gl_lms.allocated, 0, sizeof(gl_lms.allocated));
	r_framecount = 1;
	r_oldviewcluster = r_oldviewcluster2 = -2;	// force a PVS re-mark on the new map

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

	// (re)create the dynamic page: surfaces touched by dlights re-upload
	// their block (static + dynamic light) here each frame and draw from it
	if (!gl3_lightmap_dyn)
		glGenTextures (1, &gl3_lightmap_dyn);
	glBindTexture (GL_TEXTURE_2D, gl3_lightmap_dyn);
	gl3state.currenttexture = -1;
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, BLOCK_WIDTH, BLOCK_HEIGHT, 0,
		GL_RGBA, GL_UNSIGNED_BYTE, NULL);
}

// build this dlit surface's block (static + dynamic) and upload it into the
// dynamic page at the surface's own page position, so its baked lm texcoords
// stay valid; binds the dynamic page on unit 1
static void GL3_UpdateDynamicLightmap (msurface_t *surf)
{
	static byte	scratch[34 * 34 * LIGHTMAP_BYTES];	// id tolerates up to 33x33 blocks
	byte		cached[MAXLIGHTMAPS];
	int			smax = (surf->extents[0] >> 4) + 1;
	int			tmax = (surf->extents[1] >> 4) + 1;

	// a dlight-frame rebuild must not update the style cache: id only caches
	// on the persistent path, else a style toggle landing on a dlit frame is
	// absorbed and the static page never refreshes
	memcpy (cached, surf->cached_light, sizeof(cached));
	R_BuildLightMap (surf, scratch, smax * LIGHTMAP_BYTES);
	memcpy (surf->cached_light, cached, sizeof(cached));

	glActiveTexture (GL_TEXTURE1);
	glBindTexture (GL_TEXTURE_2D, gl3_lightmap_dyn);
	glTexSubImage2D (GL_TEXTURE_2D, 0, surf->light_s, surf->light_t, smax, tmax,
		GL_RGBA, GL_UNSIGNED_BYTE, scratch);
	glActiveTexture (GL_TEXTURE0);
}

// true if any of the surface's lightstyles animated since its block was built
static qboolean GL3_LightstylesChanged (msurface_t *surf)
{
	int	map;

	if (!r_newrefdef.lightstyles || !gl_dynamic || !gl_dynamic->value)
		return false;

	for (map = 0; map < MAXLIGHTMAPS && surf->styles[map] != 255; map++)
		if (r_newrefdef.lightstyles[surf->styles[map]].white != surf->cached_light[map])
			return true;
	return false;
}

// rebuild an animated-style surface's block into its OWN page (it stays
// valid until the style changes again); leaves that page bound on unit 1
static void GL3_UpdateStaticLightmap (msurface_t *surf)
{
	static byte	scratch[34 * 34 * LIGHTMAP_BYTES];
	int			smax = (surf->extents[0] >> 4) + 1;
	int			tmax = (surf->extents[1] >> 4) + 1;

	R_BuildLightMap (surf, scratch, smax * LIGHTMAP_BYTES);

	glActiveTexture (GL_TEXTURE1);
	glBindTexture (GL_TEXTURE_2D, gl3_lightmap_tex[surf->lightmaptexturenum]);
	glTexSubImage2D (GL_TEXTURE_2D, 0, surf->light_s, surf->light_t, smax, tmax,
		GL_RGBA, GL_UNSIGNED_BYTE, scratch);
	glActiveTexture (GL_TEXTURE0);
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

	// free the previous map's world buffers before building (or bailing)
	if (world_vbo) { glDeleteBuffers (1, &world_vbo); world_vbo = 0; }
	if (world_vao) { glDeleteVertexArrays (1, &world_vao); world_vao = 0; }

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

// Release every GL object owned by this module (world geometry and all
// lightmap pages, static and dynamic). Called at renderer shutdown; the
// per-map paths free the previous map's objects on their own.
void GL3_ShutdownSurf (void)
{
	int	i;

	if (world_vbo) { glDeleteBuffers (1, &world_vbo); world_vbo = 0; }
	if (world_vao) { glDeleteVertexArrays (1, &world_vao); world_vao = 0; }

	for (i = 1; i < gl_lms.current_lightmap_texture; i++)
	{
		if (gl3_lightmap_tex[i])
		{
			glDeleteTextures (1, &gl3_lightmap_tex[i]);
			gl3_lightmap_tex[i] = 0;
		}
	}
	gl_lms.current_lightmap_texture = 1;

	if (gl3_lightmap_dyn) { glDeleteTextures (1, &gl3_lightmap_dyn); gl3_lightmap_dyn = 0; }
}

// ------------------------------------------------------------------ culling

static int SignbitsForPlane (cplane_t *p)
{
	int	bits = 0, j;

	for (j = 0; j < 3; j++)
		if (p->normal[j] < 0)
			bits |= 1 << j;
	return bits;
}

// build the four side planes of the view frustum from the current refdef
void GL3_SetFrustum (void)
{
	vec3_t	vpn, vright, vup;
	int		i;

	AngleVectors (r_newrefdef.viewangles, vpn, vright, vup);

	RotatePointAroundVector (frustum[0].normal, vup, vpn, -(90 - r_newrefdef.fov_x / 2));
	RotatePointAroundVector (frustum[1].normal, vup, vpn, 90 - r_newrefdef.fov_x / 2);
	RotatePointAroundVector (frustum[2].normal, vright, vpn, 90 - r_newrefdef.fov_y / 2);
	RotatePointAroundVector (frustum[3].normal, vright, vpn, -(90 - r_newrefdef.fov_y / 2));

	for (i = 0; i < 4; i++)
	{
		frustum[i].type = PLANE_ANYZ;
		frustum[i].dist = DotProduct (r_newrefdef.vieworg, frustum[i].normal);
		frustum[i].signbits = SignbitsForPlane (&frustum[i]);
	}
}

// true if the box is completely outside the frustum
qboolean GL3_CullBox (vec3_t mins, vec3_t maxs)
{
	int	i;

	if (r_nocull && r_nocull->value)
		return false;

	for (i = 0; i < 4; i++)
		if (BoxOnPlaneSide (mins, maxs, &frustum[i]) == 2)
			return true;
	return false;
}

/*
=================
GL3_MarkLeaves

Find the cluster the view origin is in and flag every leaf (and its parent
nodes) in that cluster's PVS with the current visframecount. Uses a second
cluster 16 units above/below the eye so surfaces don't drop out while
crossing a water boundary.
=================
*/
void GL3_MarkLeaves (void)
{
	static byte	fatvis[MAX_MAP_LEAFS / 8];
	byte		*vis;
	mnode_t		*node;
	mleaf_t		*leaf;
	int			i, c, cluster;

	if (!r_worldmodel || (r_newrefdef.rdflags & RDF_NOWORLDMODEL))
		return;

	if (!r_novis)
	{
		r_novis = ri.Cvar_Get ("r_novis", "0", 0);
		r_nocull = ri.Cvar_Get ("r_nocull", "0", 0);
	}

	// current view cluster (and a second one just above/below the eye)
	leaf = GL3_Mod_PointInLeaf (r_newrefdef.vieworg, r_worldmodel);
	r_viewcluster = r_viewcluster2 = leaf->cluster;

	if (!leaf->contents)
	{	// look down a bit
		vec3_t	temp;
		VectorCopy (r_newrefdef.vieworg, temp);
		temp[2] -= 16;
		leaf = GL3_Mod_PointInLeaf (temp, r_worldmodel);
		if (!(leaf->contents & CONTENTS_SOLID) && leaf->cluster != r_viewcluster2)
			r_viewcluster2 = leaf->cluster;
	}
	else
	{	// look up a bit
		vec3_t	temp;
		VectorCopy (r_newrefdef.vieworg, temp);
		temp[2] += 16;
		leaf = GL3_Mod_PointInLeaf (temp, r_worldmodel);
		if (!(leaf->contents & CONTENTS_SOLID) && leaf->cluster != r_viewcluster2)
			r_viewcluster2 = leaf->cluster;
	}

	if (r_oldviewcluster == r_viewcluster && r_oldviewcluster2 == r_viewcluster2
		&& !r_novis->value && r_viewcluster != -1)
		return;					// same PVS as last frame

	r_visframecount++;
	r_oldviewcluster = r_viewcluster;
	r_oldviewcluster2 = r_viewcluster2;

	if (r_novis->value || r_viewcluster == -1 || !r_worldmodel->vis)
	{
		// no vis data (or disabled): everything visible
		for (i = 0; i < r_worldmodel->numleafs; i++)
			r_worldmodel->leafs[i].visframe = r_visframecount;
		for (i = 0; i < r_worldmodel->numnodes; i++)
			r_worldmodel->nodes[i].visframe = r_visframecount;
		return;
	}

	vis = GL3_Mod_ClusterPVS (r_viewcluster, r_worldmodel);

	// may need to combine two clusters because of solid water boundaries
	if (r_viewcluster2 != r_viewcluster)
	{
		memcpy (fatvis, vis, (r_worldmodel->numleafs + 7) / 8);
		vis = GL3_Mod_ClusterPVS (r_viewcluster2, r_worldmodel);
		c = (r_worldmodel->numleafs + 31) / 32;
		for (i = 0; i < c; i++)
			((int *)fatvis)[i] |= ((int *)vis)[i];
		vis = fatvis;
	}

	for (i = 0, leaf = r_worldmodel->leafs; i < r_worldmodel->numleafs; i++, leaf++)
	{
		cluster = leaf->cluster;
		if (cluster == -1)
			continue;
		if (vis[cluster >> 3] & (1 << (cluster & 7)))
		{
			node = (mnode_t *)leaf;
			do
			{
				if (node->visframe == r_visframecount)
					break;
				node->visframe = r_visframecount;
				node = node->parent;
			} while (node);
		}
	}
}

/*
=================
R_RecursiveWorldNode

Walk the visible part of the BSP front-to-back, frustum-culling by node
bounds. Leaves stamp their surfaces' visframe; nodes then stamp drawframe
on those of their surfaces that face the viewer. The draw passes render
only surfaces with the current drawframe.
=================
*/
static qboolean	r_sky_visible;	// a SURF_DRAWSKY surface was stamped this frame

static void R_RecursiveWorldNode (mnode_t *node)
{
	int			c, side, sidebit;
	cplane_t	*plane;
	msurface_t	*surf, **mark;
	mleaf_t		*pleaf;
	float		dot;

	if (node->contents == CONTENTS_SOLID)
		return;
	if (node->visframe != r_visframecount)
		return;
	if (GL3_CullBox (node->minmaxs, node->minmaxs + 3))
		return;

	if (node->contents != -1)
	{	// leaf: mark its surfaces as being in the PVS this frame
		pleaf = (mleaf_t *)node;

		// door-closed areas are connected off server-side; skip them
		if (r_newrefdef.areabits &&
			!(r_newrefdef.areabits[pleaf->area >> 3] & (1 << (pleaf->area & 7))))
			return;

		mark = pleaf->firstmarksurface;
		c = pleaf->nummarksurfaces;
		while (c--)
		{
			(*mark)->visframe = r_framecount;
			mark++;
		}
		return;
	}

	// which side of the node's plane is the viewer on?
	plane = node->plane;
	switch (plane->type)
	{
	case PLANE_X:	dot = modelorg[0] - plane->dist; break;
	case PLANE_Y:	dot = modelorg[1] - plane->dist; break;
	case PLANE_Z:	dot = modelorg[2] - plane->dist; break;
	default:		dot = DotProduct (modelorg, plane->normal) - plane->dist; break;
	}
	side = (dot >= 0) ? 0 : 1;
	sidebit = side ? SURF_PLANEBACK : 0;

	R_RecursiveWorldNode (node->children[side]);

	// this node's surfaces: draw the ones in the PVS that face the viewer
	surf = r_worldmodel->surfaces + node->firstsurface;
	for (c = node->numsurfaces; c; c--, surf++)
	{
		if (surf->visframe != r_framecount)
			continue;			// not in a visible leaf
		if ((surf->flags & SURF_PLANEBACK) != sidebit)
			continue;			// facing away
		surf->drawframe = r_framecount;
		if (surf->flags & SURF_DRAWSKY)
			r_sky_visible = true;
	}

	R_RecursiveWorldNode (node->children[!side]);
}

// true when this frame's world walk stamped at least one sky surface.
// The skybox must only be painted where real sky brushes are on screen:
// maps are vised with opaque water, so e.g. submerged views legitimately
// exclude the room above -- an unconditional sky backdrop would bleed
// through the translucent water surface there (visible as "skybox
// instead of ceiling" when underwater)
qboolean GL3_SkyVisible (void)
{
	return r_sky_visible;
}

// voxel mode reuses the world's PVS + frustum + facing cull: stamp
// surf->drawframe for this frame's visible surfaces without drawing them
void GL3_MarkVisibleSurfaces (void)
{
	if (!r_worldmodel)
		return;
	VectorCopy (r_newrefdef.vieworg, modelorg);
	r_sky_visible = false;
	R_RecursiveWorldNode (r_worldmodel->nodes);
}

// ------------------------------------------------------------------ draw

// step the animation chain by the entity's frame, matching R_TextureAnimation.
// The world animates at 2 Hz: id sets its worldspawn entity frame to time*2
static image_t *GL3_TextureAnimation (mtexinfo_t *tex, int frame)
{
	int	c;

	if (!tex->next)
		return tex->image;

	c = frame % tex->numframes;
	while (c)
	{
		tex = tex->next;
		c--;
	}
	return tex->image;
}

// SURF_FLOWING scroll for this frame: cycles 0..-64 over 40s (id semantics)
static float GL3_FlowScroll (void)
{
	float	scroll = -64.0f * ((r_newrefdef.time / 40.0f) - (int)(r_newrefdef.time / 40.0f));
	return scroll ? scroll : -64.0f;
}

// flowing WATER scrolls much faster than conveyor walls: id's EmitWaterPolys
// cycles 0..-64 raw units every 2 seconds, added before the /64 normalize
static float GL3_FlowScrollWarp (void)
{
	return -64.0f * ((r_newrefdef.time * 0.5f) - (int)(r_newrefdef.time * 0.5f));
}

// the world's texture-animation frame (R_DrawWorld: ent.frame = time*2)
static int GL3_WorldFrame (void)
{
	return (int)(r_newrefdef.time * 2);
}

// bump is meaningful only when dynamic lights are per-pixel and present
static qboolean GL3_BumpActive (void)
{
	return gl_bump && gl_bump->value
		&& gl_dynamic && gl_dynamic->value == 2
		&& r_newrefdef.num_dlights > 0;
}

// per-surface tangent basis for the bump path: the texinfo's texture axes
// plus the plane normal (flipped for back-side surfaces)
static void GL3_SetSurfTBN (msurface_t *surf, image_t *img)
{
	vec3_t	t, b, n;

	VectorCopy (surf->texinfo->vecs[0], t);
	VectorNormalize (t);
	VectorCopy (surf->texinfo->vecs[1], b);
	VectorNormalize (b);
	VectorCopy (surf->plane->normal, n);
	if (surf->flags & SURF_PLANEBACK)
		VectorNegate (n, n);

	glUniform3fv (gl3_prog3d.u_tbn_t, 1, t);
	glUniform3fv (gl3_prog3d.u_tbn_b, 1, b);
	glUniform3fv (gl3_prog3d.u_tbn_n, 1, n);

	glActiveTexture (GL_TEXTURE2);
	glBindTexture (GL_TEXTURE_2D, img->normaltex ? img->normaltex : gl3_flat_normal);
	glActiveTexture (GL_TEXTURE0);
}

void GL3_DrawWorld (void)
{
	int		i;
	int		cur_lm = -1, cur_lm_enabled = -1, cur_flowing = -1;
	msurface_t	*surf;

	if (!r_worldmodel || !world_vao)
		return;

	// walk the BSP: stamps drawframe on PVS-visible, frustum-passing,
	// front-facing surfaces for all of this frame's world passes
	VectorCopy (r_newrefdef.vieworg, modelorg);
	r_sky_visible = false;
	R_RecursiveWorldNode (r_worldmodel->nodes);

	glUniform1f (gl3_prog3d.u_alpha, 1.0f);
	GL3_UploadDlights (vec3_origin);	// per-pixel dlights (gl_dynamic 2)

	int bump = GL3_BumpActive ();
	glUniform1i (gl3_prog3d.u_bump, bump);

	glBindVertexArray (world_vao);

	surf = r_worldmodel->surfaces;
	for (i = 0; i < r_worldmodel->numsurfaces; i++, surf++)
	{
		image_t	*img;
		glpoly_t *p;
		int		lit;

		if (surf->drawframe != r_framecount)
			continue;			// not visible this frame
		if (surf->flags & (SURF_DRAWSKY | SURF_DRAWTURB))
			continue;			// sky and water drawn in their own passes
		if (surf->texinfo && (surf->texinfo->flags & (SURF_TRANS33 | SURF_TRANS66)))
			continue;			// translucent surfaces drawn in a later pass

		img = surf->texinfo ? GL3_TextureAnimation (surf->texinfo, GL3_WorldFrame ()) : NULL;
		if (!img)
			img = r_notexture;

		{	// conveyor-style scrolling textures
			int flowing = surf->texinfo && (surf->texinfo->flags & SURF_FLOWING);
			if (flowing != cur_flowing)
			{
				glUniform1f (gl3_prog3d.u_scroll, flowing ? GL3_FlowScroll () : 0.0f);
				cur_flowing = flowing;
			}
		}

		// lit surfaces sample their lightmap page on unit 1
		lit = !(surf->flags & SURF_DRAWTURB) && surf->lightmaptexturenum > 0;
		if (lit != cur_lm_enabled)
		{
			glUniform1i (gl3_prog3d.u_lm_enabled, lit);
			cur_lm_enabled = lit;
		}
		if (lit && surf->dlightframe == r_framecount)
		{
			// dlit: rebuild with dynamic light and draw from the dynamic page
			GL3_UpdateDynamicLightmap (surf);
			cur_lm = -1;			// next static surface must rebind its page
		}
		else if (lit && GL3_LightstylesChanged (surf))
		{
			// animated lightstyle: refresh the block in its own page
			GL3_UpdateStaticLightmap (surf);
			cur_lm = surf->lightmaptexturenum;	// left bound by the update
		}
		else if (lit && surf->lightmaptexturenum != cur_lm)
		{
			glActiveTexture (GL_TEXTURE1);
			glBindTexture (GL_TEXTURE_2D, gl3_lightmap_tex[surf->lightmaptexturenum]);
			glActiveTexture (GL_TEXTURE0);
			cur_lm = surf->lightmaptexturenum;
		}

		GL3_Bind (img->texnum);		// diffuse on unit 0

		if (bump)
			GL3_SetSurfTBN (surf, img);	// normal map on unit 2

		for (p = surf->polys; p; p = p->next)
		{
			glDrawArrays (GL_TRIANGLE_FAN, p->vbo_firstvert, p->numverts);
			c_brush_polys++;
		}
	}

	if (cur_flowing == 1)
		glUniform1f (gl3_prog3d.u_scroll, 0.0f);
}

// a turb surface draws blended if the texinfo asks for it (TRANS33/66) or
// when its liquid-class alpha cvar lowers it below opaque: gl_wateralpha
// for plain water, gl_slimealpha for acid/sewage (default opaque -- murk
// hides submerged secrets). Lava is never blended.
static float GL3_TurbAlpha (const msurface_t *surf)
{
	if (surf->texinfo && (surf->texinfo->flags & SURF_TRANS33))
		return 0.33f;
	if (surf->texinfo && (surf->texinfo->flags & SURF_TRANS66))
		return 0.66f;
	if (surf->flags & SURF_DRAWLAVA)
		return 1.0f;
	if (surf->flags & SURF_DRAWSLIME)
		return gl_slimealpha ? gl_slimealpha->value : 1.0f;
	return gl_wateralpha ? gl_wateralpha->value : 1.0f;
}

static qboolean GL3_TurbTranslucent (const msurface_t *surf)
{
	return GL3_TurbAlpha (surf) < 1.0f;
}

/*
=================
GL3_DrawWater

Turb (water/lava/slime) surfaces: raw texcoords warped per-fragment by the
warp shader. Opaque unless the texinfo is flagged translucent or
gl_wateralpha is below 1 (lava always stays opaque).
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
	// id draws ALL turb at inverse_intensity ("the textures are prescaled up
	// for a better lighting range, so scale it back down") -- liquids show
	// the texture's original brightness, never the intensity boost
	glUniform1f (gl3_prog_warp.u_intensity, 1.0f);
	glActiveTexture (GL_TEXTURE0);
	glBindVertexArray (world_vao);

	surf = r_worldmodel->surfaces;
	for (i = 0; i < r_worldmodel->numsurfaces; i++, surf++)
	{
		image_t		*img;
		glpoly_t	*p;

		if (surf->drawframe != r_framecount)
			continue;
		if (!(surf->flags & SURF_DRAWTURB))
			continue;
		if (GL3_TurbTranslucent (surf))
			continue;			// translucent water: blended pass below

		glUniform1f (gl3_prog_warp.u_scroll,
			(surf->texinfo && (surf->texinfo->flags & SURF_FLOWING)) ? GL3_FlowScrollWarp () : 0.0f);

		img = surf->texinfo ? GL3_TextureAnimation (surf->texinfo, GL3_WorldFrame ()) : NULL;
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
// scene grab for refraction, set per frame by GL3_RenderFrame (0 = off)
static GLuint	refract_tex;

void GL3_SetRefractionTex (GLuint tex)
{
	refract_tex = tex;
}

void GL3_DrawWorldTranslucent (void)
{
	int			i, refract;
	msurface_t	*surf;

	if (!r_worldmodel || !world_vao)
		return;

	refract = refract_tex != 0;

	glUseProgram (gl3_prog3d.program);
	glUniformMatrix4fv (gl3_prog3d.u_mvp, 1, GL_FALSE, gl3_viewproj);	// entities may have changed it
	glUniform1i (gl3_prog3d.u_lm_enabled, 0);
	GL3_UploadDlights (vec3_origin);	// bmodels may have left theirs bound
	glUniform1i (gl3_prog3d.u_refract, refract);
	if (refract)
	{
		// the shaders composite the grabbed scene themselves: no blending.
		// Offset budget ~0.8% of the frame height, resolution-independent
		float	rs = GL3_Post_Height () * 0.008f;
		glUniform2f (gl3_prog3d.u_rscale, rs / GL3_Post_Width (), rs / GL3_Post_Height ());
		glActiveTexture (GL_TEXTURE3);
		glBindTexture (GL_TEXTURE_2D, refract_tex);
		glActiveTexture (GL_TEXTURE0);
	}
	else
	{
		glEnable (GL_BLEND);
		glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	}
	glDepthMask (GL_FALSE);
	glBindVertexArray (world_vao);

	surf = r_worldmodel->surfaces;
	for (i = 0; i < r_worldmodel->numsurfaces; i++, surf++)
	{
		image_t		*img;
		glpoly_t	*p;

		if (surf->drawframe != r_framecount)
			continue;
		if (!surf->texinfo || !(surf->texinfo->flags & (SURF_TRANS33 | SURF_TRANS66)))
			continue;
		if (surf->flags & SURF_DRAWTURB)
			continue;			// translucent water drawn below with the warp shader

		glUniform1f (gl3_prog3d.u_alpha,
			(surf->texinfo->flags & SURF_TRANS33) ? 0.33f : 0.66f);

		img = GL3_TextureAnimation (surf->texinfo, GL3_WorldFrame ());
		if (!img)
			img = r_notexture;
		GL3_Bind (img->texnum);

		if (refract)
		{
			// the glass normal map drives the refraction offset; a missing
			// map falls back flat (zero offset = plain composite)
			glActiveTexture (GL_TEXTURE2);
			glBindTexture (GL_TEXTURE_2D, img->normaltex ? img->normaltex : gl3_flat_normal);
			glActiveTexture (GL_TEXTURE0);
		}

		for (p = surf->polys; p; p = p->next)
			glDrawArrays (GL_TRIANGLE_FAN, p->vbo_firstvert, p->numverts);
	}

	glUniform1f (gl3_prog3d.u_alpha, 1.0f);
	glUniform1i (gl3_prog3d.u_refract, 0);

	// translucent water/lava/slime: warp shader, no depth writes; with
	// refraction the shader composites the grab, otherwise plain blending.
	// intensity neutral, like id's inverse_intensity scale for alpha surfaces
	glUseProgram (gl3_prog_warp.program);
	glUniformMatrix4fv (gl3_prog_warp.u_mvp, 1, GL_FALSE, gl3_viewproj);
	glUniform1f (gl3_prog_warp.u_time, r_newrefdef.time);
	glUniform1f (gl3_prog_warp.u_gamma, vid_gamma->value < 0.5f ? 0.5f : vid_gamma->value);
	glUniform1f (gl3_prog_warp.u_intensity, 1.0f);
	glUniform1i (gl3_prog_warp.u_refract, refract);
	if (refract)
	{
		float	rs = GL3_Post_Height () * 0.008f;
		glUniform2f (gl3_prog_warp.u_rscale, rs / GL3_Post_Width (), rs / GL3_Post_Height ());
		glActiveTexture (GL_TEXTURE1);
		glBindTexture (GL_TEXTURE_2D, refract_tex);
		glActiveTexture (GL_TEXTURE0);
	}

	surf = r_worldmodel->surfaces;
	for (i = 0; i < r_worldmodel->numsurfaces; i++, surf++)
	{
		image_t		*img;
		glpoly_t	*p;

		if (surf->drawframe != r_framecount)
			continue;
		if (!(surf->flags & SURF_DRAWTURB))
			continue;
		if (!GL3_TurbTranslucent (surf))
			continue;

		glUniform1f (gl3_prog_warp.u_alpha, GL3_TurbAlpha (surf));
		glUniform1f (gl3_prog_warp.u_scroll,
			(surf->texinfo && (surf->texinfo->flags & SURF_FLOWING)) ? GL3_FlowScrollWarp () : 0.0f);

		img = surf->texinfo ? GL3_TextureAnimation (surf->texinfo, GL3_WorldFrame ()) : NULL;
		if (!img)
			img = r_notexture;
		GL3_Bind (img->texnum);

		for (p = surf->polys; p; p = p->next)
			glDrawArrays (GL_TRIANGLE_FAN, p->vbo_firstvert, p->numverts);
	}

	glUniform1f (gl3_prog_warp.u_alpha, 1.0f);
	glUniform1i (gl3_prog_warp.u_refract, 0);
	glDepthMask (GL_TRUE);
	glDisable (GL_BLEND);
}

// draw a single world surface (diffuse + lightmap) -- used for inline bmodels.
// entalpha < 1 blends the surface at that alpha regardless of texinfo
// (RF_TRANSLUCENT bmodels draw whole at 0.25, R_DrawInlineBModel)
static void GL3_DrawSurface (msurface_t *surf, int frame, float entalpha)
{
	image_t		*img;
	glpoly_t	*p;
	int			lit, trans;

	if (surf->flags & (SURF_DRAWSKY | SURF_DRAWTURB))
		return;			// turb surfaces get a warp-shader pass of their own

	img = surf->texinfo ? GL3_TextureAnimation (surf->texinfo, frame) : NULL;
	if (!img)
		img = r_notexture;

	glUniform1f (gl3_prog3d.u_scroll,
		(surf->texinfo && (surf->texinfo->flags & SURF_FLOWING)) ? GL3_FlowScroll () : 0.0f);

	lit = surf->lightmaptexturenum > 0;
	glUniform1i (gl3_prog3d.u_lm_enabled, lit);
	if (lit && surf->dlightframe == r_framecount)
	{
		GL3_UpdateDynamicLightmap (surf);	// binds the dynamic page
	}
	else if (lit && GL3_LightstylesChanged (surf))
	{
		GL3_UpdateStaticLightmap (surf);	// refresh + bind its own page
	}
	else if (lit)
	{
		glActiveTexture (GL_TEXTURE1);
		glBindTexture (GL_TEXTURE_2D, gl3_lightmap_tex[surf->lightmaptexturenum]);
		glActiveTexture (GL_TEXTURE0);
	}
	GL3_Bind (img->texnum);

	if (GL3_BumpActive ())
		GL3_SetSurfTBN (surf, img);		// normal map on unit 2

	// translucent bmodel surfaces (glass doors etc.) blend in place
	trans = entalpha < 1.0f
		|| (surf->texinfo && (surf->texinfo->flags & (SURF_TRANS33 | SURF_TRANS66)));
	if (trans)
	{
		glEnable (GL_BLEND);
		glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glDepthMask (GL_FALSE);
		glUniform1f (gl3_prog3d.u_alpha,
			entalpha < 1.0f ? entalpha
				: (surf->texinfo->flags & SURF_TRANS33) ? 0.33f : 0.66f);
	}

	for (p = surf->polys; p; p = p->next)
	{
		glDrawArrays (GL_TRIANGLE_FAN, p->vbo_firstvert, p->numverts);
		c_brush_polys++;
	}

	if (trans)
	{
		glUniform1f (gl3_prog3d.u_alpha, 1.0f);
		glDepthMask (GL_TRUE);
		glDisable (GL_BLEND);
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
	int			i, numturb = 0;
	vec3_t		mins, maxs;

	if (!mod || mod->nummodelsurfaces == 0)
		return;

	// frustum-cull by the entity's translated bounds (rotated bmodels get a
	// conservative box the way the original did via the model radius)
	if (e->angles[0] || e->angles[1] || e->angles[2])
	{
		for (i = 0; i < 3; i++)
		{
			mins[i] = e->origin[i] - mod->radius;
			maxs[i] = e->origin[i] + mod->radius;
		}
	}
	else
	{
		VectorAdd (e->origin, mod->mins, mins);
		VectorAdd (e->origin, mod->maxs, maxs);
	}
	if (GL3_CullBox (mins, maxs))
		return;

	// dynamic lights on the bmodel's own subtree (world-space light origins,
	// same approximation the original used for moving models). CPU lightmap
	// mode only: per-pixel mode lights bmodels in the shader (marking here
	// would double-apply), and flashblend draws glow balls instead
	// firstnode < 0 is a valid leaf-encoded headnode (submodel with no node
	// subtree); nodes + firstnode would be a wild pointer, so skip marking
	if (gl_dynamic && gl_dynamic->value == 1
		&& !(gl_flashblend && gl_flashblend->value)
		&& mod->firstnode >= 0)
	{
		dlight_t	*lt = r_newrefdef.dlights;
		for (i = 0; i < r_newrefdef.num_dlights; i++, lt++)
			GL3_MarkLights (lt, 1 << i, r_worldmodel->nodes + mod->firstnode);
	}

	// model matrix: translate then yaw/pitch/roll (R_RotateForEntity)
	GL3_MatTranslate (model_mat, e->origin[0], e->origin[1], e->origin[2]);
	GL3_MatRotate (tmp, e->angles[1], 0, 0, 1); GL3_MatMul (model_mat, model_mat, tmp);
	GL3_MatRotate (tmp, e->angles[0], 0, 1, 0); GL3_MatMul (model_mat, model_mat, tmp);
	GL3_MatRotate (tmp, e->angles[2], 1, 0, 0); GL3_MatMul (model_mat, model_mat, tmp);
	GL3_MatMul (mvp, viewproj, model_mat);

	glUseProgram (gl3_prog3d.program);
	glUniformMatrix4fv (gl3_prog3d.u_mvp, 1, GL_FALSE, mvp);
	GL3_UploadDlights (e->origin);	// shift lights into this bmodel's space
	glBindVertexArray (world_vao);

	// RF_TRANSLUCENT bmodels (ghost doors/platforms) blend whole at 0.25
	float entalpha = (e->flags & RF_TRANSLUCENT) ? 0.25f : 1.0f;

	surf = r_worldmodel->surfaces + mod->firstmodelsurface;
	for (i = 0; i < mod->nummodelsurfaces; i++, surf++)
	{
		if (surf->flags & SURF_DRAWTURB)
			numturb++;
		else
			GL3_DrawSurface (surf, e->frame, entalpha);
	}

	// water brushes (func_water etc.) need the warp shader with this
	// entity's transform
	if (numturb)
	{
		glUseProgram (gl3_prog_warp.program);
		glUniformMatrix4fv (gl3_prog_warp.u_mvp, 1, GL_FALSE, mvp);
		glUniform1f (gl3_prog_warp.u_time, r_newrefdef.time);
		glUniform1f (gl3_prog_warp.u_gamma, vid_gamma->value < 0.5f ? 0.5f : vid_gamma->value);
		glUniform1f (gl3_prog_warp.u_intensity, 1.0f);	// id: all turb at inverse_intensity

		surf = r_worldmodel->surfaces + mod->firstmodelsurface;
		for (i = 0; i < mod->nummodelsurfaces; i++, surf++)
		{
			image_t		*img;
			glpoly_t	*p;
			int			trans;

			if (!(surf->flags & SURF_DRAWTURB))
				continue;

			glUniform1f (gl3_prog_warp.u_scroll,
				(surf->texinfo && (surf->texinfo->flags & SURF_FLOWING)) ? GL3_FlowScrollWarp () : 0.0f);

			trans = entalpha < 1.0f || GL3_TurbTranslucent (surf);
			if (trans)
			{
				glEnable (GL_BLEND);
				glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
				glDepthMask (GL_FALSE);
				glUniform1f (gl3_prog_warp.u_alpha,
					entalpha < 1.0f ? entalpha : GL3_TurbAlpha (surf));
			}

			img = surf->texinfo ? GL3_TextureAnimation (surf->texinfo, e->frame) : NULL;
			if (!img)
				img = r_notexture;
			GL3_Bind (img->texnum);
			for (p = surf->polys; p; p = p->next)
				glDrawArrays (GL_TRIANGLE_FAN, p->vbo_firstvert, p->numverts);

			if (trans)
			{
				glUniform1f (gl3_prog_warp.u_alpha, 1.0f);
				glDepthMask (GL_TRUE);
				glDisable (GL_BLEND);
			}
		}

		glUseProgram (gl3_prog3d.program);	// restore for subsequent entities
	}
}
