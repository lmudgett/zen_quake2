// gl3_voxel.c -- textured voxel render mode (world + entities).
//
// When r_voxelize is on, the world (and, with r_voxelize_entities, alias
// models and the view weapon) is rebuilt as a mesh of axis-aligned cubes at
// r_voxelsize resolution. Each surface is snapped to a grid, only the exposed
// cube faces are emitted, and each face is textured with UVs projected from
// the source surface exactly as the normal renderer projects them -- so the
// real Quake textures flow across the blocky surface. Faces are grouped per
// source surface and drawn only for PVS/frustum-visible surfaces, reusing the
// engine's existing culling, which is what keeps fine resolutions affordable.
//
// Author: Len Mudgett

#include "gl3_local.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

cvar_t	*r_voxelize;
cvar_t	*r_voxelsize;
cvar_t	*r_voxelize_entities;

void R_LightPoint (vec3_t p, vec3_t color);	// gl3_mesh.c: sample world light

// one vertex of the voxel-face mesh: world/model pos, projected uv, baked shade
typedef struct { float pos[3]; float uv[2]; float shade; } voxvert_t;
#define VOXVERT_FLOATS	6

// ---------------------------------------------------------------- occupancy
// A grid cell is (x,y,z) integer coordinates relative to vox_origin. During a
// build we hash every occupied cell (tagged with the surface that filled it)
// so Pass B can test whether a neighbour cell is solid and cull hidden faces.

// surf = owning world surface (-1 for models); u,v = skin texcoord (models)
typedef struct { int x, y, z, surf, next; float u, v; } voxcell_t;

static voxcell_t	*cell_pool;
static int			cell_count, cell_cap;
static int			*cell_hash;			// bucket -> head cell index, or -1
static int			cell_hashmask;

static vec3_t		vox_origin;			// world-space grid origin (r_worldmodel->mins)
static float		vox_size;			// cell edge in world units

static unsigned CellKey (int x, int y, int z)
{
	return (unsigned)(x * 73856093) ^ (unsigned)(y * 19349663) ^ (unsigned)(z * 83492791);
}

static qboolean CellExists (int x, int y, int z)
{
	int	i;

	for (i = cell_hash[CellKey (x, y, z) & cell_hashmask]; i != -1; i = cell_pool[i].next)
		if (cell_pool[i].x == x && cell_pool[i].y == y && cell_pool[i].z == z)
			return true;
	return false;
}

static void AddCellXYZ (int x, int y, int z, int surf, float u, float v)
{
	unsigned	h = CellKey (x, y, z) & cell_hashmask;
	int			i;

	for (i = cell_hash[h]; i != -1; i = cell_pool[i].next)
		if (cell_pool[i].x == x && cell_pool[i].y == y && cell_pool[i].z == z)
			return;					// already occupied (first writer owns it)

	if (cell_count == cell_cap)
	{
		cell_cap = cell_cap ? cell_cap * 2 : 65536;
		cell_pool = realloc (cell_pool, cell_cap * sizeof(voxcell_t));
		if (!cell_pool)
			ri.Sys_Error (ERR_FATAL, "GL3 voxel build: out of memory");
	}
	cell_pool[cell_count].x = x;
	cell_pool[cell_count].y = y;
	cell_pool[cell_count].z = z;
	cell_pool[cell_count].surf = surf;
	cell_pool[cell_count].u = u;
	cell_pool[cell_count].v = v;
	cell_pool[cell_count].next = cell_hash[h];
	cell_hash[h] = cell_count;
	cell_count++;
}

static void AddCellAt (const float *p, int surf)		// world: owned by surface
{
	AddCellXYZ ((int)floorf ((p[0] - vox_origin[0]) / vox_size),
				(int)floorf ((p[1] - vox_origin[1]) / vox_size),
				(int)floorf ((p[2] - vox_origin[2]) / vox_size), surf, 0.0f, 0.0f);
}

static void AddCellAtUV (const float *p, float u, float v)	// model: carries skin uv
{
	AddCellXYZ ((int)floorf ((p[0] - vox_origin[0]) / vox_size),
				(int)floorf ((p[1] - vox_origin[1]) / vox_size),
				(int)floorf ((p[2] - vox_origin[2]) / vox_size), -1, u, v);
}

// rasterize one triangle into cells: its 3 vertices, its edges (walked at
// half-cell steps), and its interior (barycentric-sampled at half-cell steps).
// Oversampling guarantees a gap-free shell for large flat BSP faces.
static void RasterTri (const float *a, const float *b, const float *c, int surf)
{
	vec3_t	ab, ac;
	float	e1, e2, u, v, step;
	int		i, j, n, en;

	AddCellAt (a, surf);
	AddCellAt (b, surf);
	AddCellAt (c, surf);

	VectorSubtract (b, a, ab);
	VectorSubtract (c, a, ac);
	e1 = VectorLength (ab);
	e2 = VectorLength (ac);
	step = vox_size * 0.5f;

	// edges
	{
		const float	*ev[3][2] = { { a, b }, { b, c }, { c, a } };
		vec3_t		d, p;
		float		len;
		int			k;
		for (k = 0; k < 3; k++)
		{
			VectorSubtract (ev[k][1], ev[k][0], d);
			len = VectorLength (d);
			en = (int)(len / step) + 1;
			for (i = 1; i < en; i++)
			{
				float t = (float)i / en;
				p[0] = ev[k][0][0] + d[0] * t;
				p[1] = ev[k][0][1] + d[1] * t;
				p[2] = ev[k][0][2] + d[2] * t;
				AddCellAt (p, surf);
			}
		}
	}

	// interior
	n = (int)((e1 > e2 ? e1 : e2) / step) + 1;
	for (i = 0; i <= n; i++)
	{
		u = (float)i / n;
		for (j = 0; j <= n - i; j++)
		{
			vec3_t	p;
			v = (float)j / n;
			p[0] = a[0] + ab[0] * u + ac[0] * v;
			p[1] = a[1] + ab[1] * u + ac[1] * v;
			p[2] = a[2] + ab[2] * u + ac[2] * v;
			AddCellAt (p, surf);
		}
	}
}

// model rasterization: like RasterTri but stores an interpolated skin uv per
// cell (models have per-vertex skin coords, not a planar texinfo projection)
static void RasterTriUV (const float *a, const float *b, const float *c,
	const float *uva, const float *uvb, const float *uvc)
{
	vec3_t	ab, ac;
	float	e1, e2, step, u, v;
	int		i, j, n;

	AddCellAtUV (a, uva[0], uva[1]);
	AddCellAtUV (b, uvb[0], uvb[1]);
	AddCellAtUV (c, uvc[0], uvc[1]);

	VectorSubtract (b, a, ab);
	VectorSubtract (c, a, ac);
	e1 = VectorLength (ab);
	e2 = VectorLength (ac);
	step = vox_size * 0.5f;

	n = (int)((e1 > e2 ? e1 : e2) / step) + 1;
	for (i = 0; i <= n; i++)
	{
		u = (float)i / n;
		for (j = 0; j <= n - i; j++)
		{
			vec3_t	p;
			float	wa;
			v = (float)j / n;
			wa = 1.0f - u - v;
			p[0] = a[0] + ab[0] * u + ac[0] * v;
			p[1] = a[1] + ab[1] * u + ac[1] * v;
			p[2] = a[2] + ab[2] * u + ac[2] * v;
			AddCellAtUV (p, uva[0]*wa + uvb[0]*u + uvc[0]*v,
						uva[1]*wa + uvb[1]*u + uvc[1]*v);
		}
	}
}

// ---------------------------------------------------------------- cube faces
// The 6 axis faces. For each: outward normal, and the two in-plane half-edge
// axes U,V ordered so U x V = +normal, giving CCW-from-outside winding. A
// per-face shade fakes directional lighting (Quake Z is up).

typedef struct { int axis, dir; int ua, va; float shade; } voxface_t;
static const voxface_t voxfaces[6] = {
	{ 0, +1, 1, 2, 0.80f },	// +X  U=+Y V=+Z
	{ 0, -1, 2, 1, 0.80f },	// -X  U=+Z V=+Y
	{ 1, +1, 2, 0, 0.65f },	// +Y  U=+Z V=+X
	{ 1, -1, 0, 2, 0.65f },	// -Y  U=+X V=+Z
	{ 2, +1, 0, 1, 1.00f },	// +Z (up)    U=+X V=+Y
	{ 2, -1, 1, 0, 0.45f },	// -Z (down)  U=+Y V=+X
};

// project a world point through a surface's texinfo, as GL3_BuildPolygonFromSurface
static void SurfUV (msurface_t *surf, const float *p, float *s, float *t)
{
	mtexinfo_t	*tex = surf->texinfo;
	*s = (DotProduct (p, tex->vecs[0]) + tex->vecs[0][3]) / tex->image->width;
	*t = (DotProduct (p, tex->vecs[1]) + tex->vecs[1][3]) / tex->image->height;
}

// ---------------------------------------------------------------- world mesh

static GLuint		world_vao, world_vbo;
static int			world_totalverts;
static float		world_built_size = -1.0f;
static int			world_built_seq = -1;	// registration_sequence the mesh was built for

typedef struct { int firstvert, numverts; image_t *img; } voxsurf_t;
static voxsurf_t	*world_surfranges;

static void FreeWorldVoxels (void)
{
	if (world_vbo) { glDeleteBuffers (1, &world_vbo); world_vbo = 0; }
	if (world_vao) { glDeleteVertexArrays (1, &world_vao); world_vao = 0; }
	free (world_surfranges);
	world_surfranges = NULL;
	world_totalverts = 0;
	world_built_seq = -1;
	world_built_size = -1.0f;
}

// write the 6 verts (2 tris) of one exposed face into dst; return dst advanced
static voxvert_t *EmitFace (voxvert_t *dst, msurface_t *surf, const voxcell_t *cell,
	const voxface_t *f)
{
	vec3_t	center, corner[4];
	float	half = vox_size * 0.5f;
	int		k;
	static const int order[6] = { 0, 1, 2, 0, 2, 3 };	// two CCW tris

	// cell center in world space
	center[0] = vox_origin[0] + (cell->x + 0.5f) * vox_size;
	center[1] = vox_origin[1] + (cell->y + 0.5f) * vox_size;
	center[2] = vox_origin[2] + (cell->z + 0.5f) * vox_size;
	// slide to the face plane along the outward normal
	center[f->axis] += f->dir * half;

	// 4 corners: center +/- U +/- V (CCW from outside)
	for (k = 0; k < 4; k++)
	{
		float	su = (k == 1 || k == 2) ? 1.0f : -1.0f;	// -U,+U,+U,-U
		float	sv = (k >= 2) ? 1.0f : -1.0f;				// -V,-V,+V,+V
		VectorCopy (center, corner[k]);
		corner[k][f->ua] += su * half;
		corner[k][f->va] += sv * half;
	}

	for (k = 0; k < 6; k++)
	{
		float	*c = corner[order[k]];
		VectorCopy (c, dst->pos);
		if (surf)					// world: project through the surface texinfo
			SurfUV (surf, c, &dst->uv[0], &dst->uv[1]);
		else						// model: the cell's stored skin uv (flat per cube)
		{
			dst->uv[0] = cell->u;
			dst->uv[1] = cell->v;
		}
		dst->shade = f->shade;
		dst++;
	}
	return dst;
}

// bind the standard voxel-mesh attributes to the currently-bound VBO/VAO
static void SetupVoxelAttribs (void)
{
	glEnableVertexAttribArray (0);	// a_pos
	glVertexAttribPointer (0, 3, GL_FLOAT, GL_FALSE, sizeof(voxvert_t), (void *)0);
	glEnableVertexAttribArray (1);	// a_uv
	glVertexAttribPointer (1, 2, GL_FLOAT, GL_FALSE, sizeof(voxvert_t), (void *)(3 * sizeof(float)));
	glEnableVertexAttribArray (3);	// a_color = baked face shade (single float)
	glVertexAttribPointer (3, 1, GL_FLOAT, GL_FALSE, sizeof(voxvert_t), (void *)(5 * sizeof(float)));
}

void GL3_BuildWorldVoxels (void)
{
	int			i, s, d;
	msurface_t	*surf;
	int			*facecount, *cursor;
	voxvert_t	*mesh, *dst;
	int			nsurf;

	FreeWorldVoxels ();

	if (!r_worldmodel)
		return;

	nsurf = r_worldmodel->numsurfaces;
	vox_size = r_voxelsize->value;
	if (vox_size < 1.0f)
		vox_size = 1.0f;
	VectorCopy (r_worldmodel->mins, vox_origin);

	// occupancy hash (transient) -- sized generously, freed after the build
	cell_hashmask = (1 << 20) - 1;
	cell_hash = malloc ((cell_hashmask + 1) * sizeof(int));
	memset (cell_hash, 0xff, (cell_hashmask + 1) * sizeof(int));	// -1
	cell_pool = NULL;
	cell_count = cell_cap = 0;

	// Pass A: rasterize every world surface into occupied cells
	surf = r_worldmodel->surfaces;
	for (i = 0; i < nsurf; i++, surf++)
	{
		glpoly_t	*p;

		if (surf->flags & (SURF_DRAWSKY | SURF_DRAWTURB))
			continue;						// sky/water get their own look
		if (!surf->texinfo || !surf->texinfo->image)
			continue;

		for (p = surf->polys; p; p = p->next)
		{
			int	k;
			// fan-triangulate the polygon
			for (k = 1; k < p->numverts - 1; k++)
				RasterTri (p->verts[0], p->verts[k], p->verts[k + 1], i);
		}
	}

	// Pass B step 1: count exposed faces per surface
	facecount = calloc (nsurf, sizeof(int));
	for (i = 0; i < cell_count; i++)
	{
		voxcell_t	*cell = &cell_pool[i];
		for (d = 0; d < 6; d++)
		{
			const voxface_t	*f = &voxfaces[d];
			int	nx = cell->x + (f->axis == 0 ? f->dir : 0);
			int	ny = cell->y + (f->axis == 1 ? f->dir : 0);
			int	nz = cell->z + (f->axis == 2 ? f->dir : 0);
			if (!CellExists (nx, ny, nz))
				facecount[cell->surf]++;
		}
	}

	// per-surface vertex ranges (6 verts per face)
	world_surfranges = calloc (nsurf, sizeof(voxsurf_t));
	world_totalverts = 0;
	for (s = 0; s < nsurf; s++)
	{
		world_surfranges[s].firstvert = world_totalverts;
		world_surfranges[s].numverts = facecount[s] * 6;
		world_surfranges[s].img = r_worldmodel->surfaces[s].texinfo
			? r_worldmodel->surfaces[s].texinfo->image : NULL;
		world_totalverts += facecount[s] * 6;
	}

	if (!world_totalverts)
	{
		free (facecount); free (cell_hash); free (cell_pool);
		cell_hash = NULL; cell_pool = NULL;
		world_built_seq = registration_sequence;
		world_built_size = vox_size;
		return;
	}

	// Pass B step 2: emit faces into per-surface ranges
	mesh = malloc (world_totalverts * sizeof(voxvert_t));
	cursor = malloc (nsurf * sizeof(int));
	for (s = 0; s < nsurf; s++)
		cursor[s] = world_surfranges[s].firstvert;

	for (i = 0; i < cell_count; i++)
	{
		voxcell_t	*cell = &cell_pool[i];
		surf = &r_worldmodel->surfaces[cell->surf];
		for (d = 0; d < 6; d++)
		{
			const voxface_t	*f = &voxfaces[d];
			int	nx = cell->x + (f->axis == 0 ? f->dir : 0);
			int	ny = cell->y + (f->axis == 1 ? f->dir : 0);
			int	nz = cell->z + (f->axis == 2 ? f->dir : 0);
			if (CellExists (nx, ny, nz))
				continue;
			dst = mesh + cursor[cell->surf];
			dst = EmitFace (dst, surf, cell, f);
			cursor[cell->surf] += 6;
		}
	}

	// upload
	glGenVertexArrays (1, &world_vao);
	glGenBuffers (1, &world_vbo);
	glBindVertexArray (world_vao);
	glBindBuffer (GL_ARRAY_BUFFER, world_vbo);
	glBufferData (GL_ARRAY_BUFFER, world_totalverts * sizeof(voxvert_t), mesh, GL_STATIC_DRAW);
	SetupVoxelAttribs ();
	glBindVertexArray (0);

	free (mesh);
	free (cursor);
	free (facecount);
	free (cell_hash);
	free (cell_pool);
	cell_hash = NULL;
	cell_pool = NULL;

	world_built_seq = registration_sequence;
	world_built_size = vox_size;

	ri.Con_Printf (PRINT_ALL, "world voxels: %d faces, %d cells @ %g units\n",
		world_totalverts / 6, cell_count, vox_size);
}

static const float voxel_identity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };

void GL3_DrawWorldVoxels (const float *viewproj)
{
	int			i;
	msurface_t	*surf;

	if (!r_worldmodel)
		return;

	// (re)build on map change (registration_sequence) or a live r_voxelsize change
	if (world_built_seq != registration_sequence || world_built_size != r_voxelsize->value)
		GL3_BuildWorldVoxels ();

	if (!world_totalverts)
		return;

	// stamp surf->drawframe for this frame's PVS + frustum (reuses the world path)
	GL3_MarkVisibleSurfaces ();

	glUseProgram (gl3_prog_voxel.program);
	glUniformMatrix4fv (gl3_prog_voxel.u_mvp, 1, GL_FALSE, viewproj);
	glUniformMatrix4fv (gl3_prog_voxel.u_model, 1, GL_FALSE, voxel_identity);
	glUniform1f (gl3_prog_voxel.u_intensity, gl_intensity->value);
	glUniform3f (gl3_prog_voxel.u_entcolor, 1.0f, 1.0f, 1.0f);

	glActiveTexture (GL_TEXTURE0);
	glBindVertexArray (world_vao);

	surf = r_worldmodel->surfaces;
	for (i = 0; i < r_worldmodel->numsurfaces; i++, surf++)
	{
		voxsurf_t	*vs = &world_surfranges[i];
		if (surf->drawframe != r_framecount)
			continue;
		if (!vs->numverts || !vs->img)
			continue;
		GL3_Bind (vs->img->texnum);
		glDrawArrays (GL_TRIANGLES, vs->firstvert, vs->numverts);
	}
	glBindVertexArray (0);
}

// ---------------------------------------------------------------- entities
// Each alias model is voxelized once (from reference frame 0) into a model-
// space cube mesh, cached by model pointer, and drawn with the entity matrix.
// Reference-frame voxels are rigid (no animation) -- the expected trade-off
// for this stylized mode; per-frame revoxelization would be far too costly.

#define MAX_VOXMODELS	512

typedef struct
{
	model_t	*mod;
	float	built_size;
	GLuint	vao, vbo;
	int		numverts;
} voxmodel_t;
static voxmodel_t	vox_models[MAX_VOXMODELS];

static voxmodel_t *FindModelSlot (model_t *mod)
{
	int	i, freeidx = -1;

	for (i = 0; i < MAX_VOXMODELS; i++)
	{
		if (vox_models[i].mod == mod)
			return &vox_models[i];
		if (freeidx < 0 && !vox_models[i].mod)
			freeidx = i;
	}
	return &vox_models[freeidx < 0 ? 0 : freeidx];		// evict slot 0 if full
}

static void FreeModelSlot (voxmodel_t *vm)
{
	if (vm->vbo) { glDeleteBuffers (1, &vm->vbo); vm->vbo = 0; }
	if (vm->vao) { glDeleteVertexArrays (1, &vm->vao); vm->vao = 0; }
	vm->numverts = 0;
	vm->built_size = -1.0f;
}

void GL3_VoxelizeAliasModel (model_t *mod)
{
	voxmodel_t		*vm;
	dmdl_t			*hdr;
	daliasframe_t	*frame;
	dtriangle_t		*tris;
	dstvert_t		*sts;
	float			sw, sh;
	int				t, d, i, nfaces;
	voxvert_t		*mesh, *dst;

	if (!mod || mod->type != mod_alias || !mod->extradata)
		return;

	vm = FindModelSlot (mod);
	if (vm->mod == mod && vm->vbo && vm->built_size == r_voxelsize->value)
		return;						// already cached at this resolution
	FreeModelSlot (vm);
	vm->mod = mod;

	hdr = (dmdl_t *)mod->extradata;
	frame = (daliasframe_t *)((byte *)hdr + hdr->ofs_frames);	// reference frame 0
	tris = (dtriangle_t *)((byte *)hdr + hdr->ofs_tris);
	sts = (dstvert_t *)((byte *)hdr + hdr->ofs_st);
	sw = hdr->skinwidth ? (float)hdr->skinwidth : 1.0f;
	sh = hdr->skinheight ? (float)hdr->skinheight : 1.0f;

	vox_size = r_voxelsize->value;
	if (vox_size < 1.0f)
		vox_size = 1.0f;
	VectorCopy (mod->mins, vox_origin);

	cell_hashmask = (1 << 15) - 1;		// models are small
	cell_hash = malloc ((cell_hashmask + 1) * sizeof(int));
	memset (cell_hash, 0xff, (cell_hashmask + 1) * sizeof(int));
	cell_pool = NULL;
	cell_count = cell_cap = 0;

	// Pass A: rasterize the model's triangles into cells carrying skin uv
	for (t = 0; t < hdr->num_tris; t++)
	{
		vec3_t	p[3];
		float	uv[3][2];
		int		k;
		for (k = 0; k < 3; k++)
		{
			dtrivertx_t	*dv = &frame->verts[ tris[t].index_xyz[k] ];
			dstvert_t	*st = &sts[ tris[t].index_st[k] ];
			p[k][0] = dv->v[0] * frame->scale[0] + frame->translate[0];
			p[k][1] = dv->v[1] * frame->scale[1] + frame->translate[1];
			p[k][2] = dv->v[2] * frame->scale[2] + frame->translate[2];
			uv[k][0] = ((float)st->s + 0.5f) / sw;
			uv[k][1] = ((float)st->t + 0.5f) / sh;
		}
		RasterTriUV (p[0], p[1], p[2], uv[0], uv[1], uv[2]);
	}

	// Pass B: emit exposed faces (one mesh for the whole model)
	nfaces = 0;
	for (i = 0; i < cell_count; i++)
		for (d = 0; d < 6; d++)
		{
			const voxface_t	*f = &voxfaces[d];
			if (!CellExists (cell_pool[i].x + (f->axis == 0 ? f->dir : 0),
							cell_pool[i].y + (f->axis == 1 ? f->dir : 0),
							cell_pool[i].z + (f->axis == 2 ? f->dir : 0)))
				nfaces++;
		}
	vm->numverts = nfaces * 6;
	vm->built_size = vox_size;

	if (!vm->numverts)
	{
		free (cell_hash); free (cell_pool);
		cell_hash = NULL; cell_pool = NULL;
		return;
	}

	mesh = malloc (vm->numverts * sizeof(voxvert_t));
	dst = mesh;
	for (i = 0; i < cell_count; i++)
		for (d = 0; d < 6; d++)
		{
			const voxface_t	*f = &voxfaces[d];
			if (CellExists (cell_pool[i].x + (f->axis == 0 ? f->dir : 0),
							cell_pool[i].y + (f->axis == 1 ? f->dir : 0),
							cell_pool[i].z + (f->axis == 2 ? f->dir : 0)))
				continue;
			dst = EmitFace (dst, NULL, &cell_pool[i], f);
		}

	glGenVertexArrays (1, &vm->vao);
	glGenBuffers (1, &vm->vbo);
	glBindVertexArray (vm->vao);
	glBindBuffer (GL_ARRAY_BUFFER, vm->vbo);
	glBufferData (GL_ARRAY_BUFFER, vm->numverts * sizeof(voxvert_t), mesh, GL_STATIC_DRAW);
	SetupVoxelAttribs ();
	glBindVertexArray (0);

	free (mesh);
	free (cell_hash);
	free (cell_pool);
	cell_hash = NULL;
	cell_pool = NULL;
}

void GL3_DrawAliasModelVoxels (entity_t *e, const float *viewproj)
{
	model_t		*mod = e->model;
	voxmodel_t	*vm;
	float		model[16], mvp[16], tmp[16];
	vec3_t		light;
	image_t		*skin;
	int			k, lefthand;

	if (!mod || mod->type != mod_alias || !mod->extradata
		|| (e->flags & RF_WEAPONMODEL))		// the held weapon sits at the eye;
	{										// coarse cubes there fill the screen, so
		GL3_DrawAliasModel (e, viewproj);	// draw it (and non-alias) normally
		return;
	}

	vm = FindModelSlot (mod);
	if (vm->mod != mod || !vm->vbo || vm->built_size != r_voxelsize->value)
		GL3_VoxelizeAliasModel (mod);
	if (!vm->numverts)
		return;

	// model matrix, matching GL3_DrawAliasModel (alias-only pitch negation)
	GL3_MatIdentity (model);
	GL3_MatTranslate (tmp, e->origin[0], e->origin[1], e->origin[2]);
	GL3_MatMul (model, model, tmp);
	GL3_MatRotate (tmp, e->angles[1], 0, 0, 1);	GL3_MatMul (model, model, tmp);	// yaw
	GL3_MatRotate (tmp, e->angles[0], 0, 1, 0);	GL3_MatMul (model, model, tmp);	// pitch
	GL3_MatRotate (tmp, -e->angles[2], 1, 0, 0);	GL3_MatMul (model, model, tmp);	// roll
	GL3_MatMul (mvp, viewproj, model);

	lefthand = (e->flags & RF_WEAPONMODEL) && r_lefthand->value == 1.0f;
	if (lefthand)
	{
		mvp[0] = -mvp[0]; mvp[4] = -mvp[4]; mvp[8] = -mvp[8]; mvp[12] = -mvp[12];
		glCullFace (GL_BACK);
	}

	// tint the figure by the world light at its origin
	R_LightPoint (e->origin, light);
	for (k = 0; k < 3; k++)
		light[k] = light[k] < 0.15f ? 0.15f : (light[k] > 1.2f ? 1.2f : light[k]);

	// skin (built with skin-0 uvs, so any skin variant shares the layout)
	skin = e->skin;
	if (!skin)
	{
		int	sn = e->skinnum;
		if (sn < 0 || sn >= MAX_MD2SKINS)
			sn = 0;
		skin = mod->skins[sn] ? mod->skins[sn] : mod->skins[0];
	}
	if (!skin)
		skin = r_notexture;

	glUseProgram (gl3_prog_voxel.program);
	glUniformMatrix4fv (gl3_prog_voxel.u_mvp, 1, GL_FALSE, mvp);
	glUniformMatrix4fv (gl3_prog_voxel.u_model, 1, GL_FALSE, voxel_identity);
	glUniform1f (gl3_prog_voxel.u_intensity, gl_intensity->value);
	glUniform3f (gl3_prog_voxel.u_entcolor, light[0], light[1], light[2]);

	if (e->flags & RF_DEPTHHACK)	// view weapon: keep it out of the world depth
		glDepthRange (0.0, 0.3);

	glActiveTexture (GL_TEXTURE0);
	GL3_Bind (skin->texnum);
	glBindVertexArray (vm->vao);
	glDrawArrays (GL_TRIANGLES, 0, vm->numverts);
	glBindVertexArray (0);

	if (e->flags & RF_DEPTHHACK)
		glDepthRange (0.0, 1.0);
	if (lefthand)
		glCullFace (GL_FRONT);		// restore the scene's cull face
}

// ---------------------------------------------------------------- init/shutdown

void GL3_InitVoxels (void)
{
	r_voxelize          = ri.Cvar_Get ("r_voxelize", "0", CVAR_ARCHIVE);
	r_voxelsize         = ri.Cvar_Get ("r_voxelsize", "8", CVAR_ARCHIVE);
	r_voxelize_entities = ri.Cvar_Get ("r_voxelize_entities", "1", CVAR_ARCHIVE);
}

void GL3_ShutdownVoxels (void)
{
	int	i;

	FreeWorldVoxels ();
	for (i = 0; i < MAX_VOXMODELS; i++)
	{
		FreeModelSlot (&vox_models[i]);
		vox_models[i].mod = NULL;
	}
}
