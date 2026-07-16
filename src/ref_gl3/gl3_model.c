// gl3_model.c -- model loading and caching for the modern OpenGL 3.3 renderer.
// Ported from the original ref_gl (gl_model.c / gl_rsurf.c). The on-disk
// formats and constants come from qfiles.h via gl3_local.h's include chain.

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "gl3_local.h"

// values that live in the original gl_local.h but are not in the gl3 chain
#define	MAX_LBM_HEIGHT		480
#define	BLOCK_WIDTH			128
#define	BLOCK_HEIGHT		128

model_t		*r_worldmodel;

static model_t	*loadmodel;
static int		modfilelen;

static void Mod_LoadSpriteModel (model_t *mod, void *buffer);
static void Mod_LoadBrushModel (model_t *mod, void *buffer);
static void Mod_LoadAliasModel (model_t *mod, void *buffer);
static void Mod_Free (model_t *mod);
void GL3_BuildPolygonFromSurface (msurface_t *fa);

static byte	mod_novis[MAX_MAP_LEAFS/8];

#define	MAX_MOD_KNOWN	512
static model_t	mod_known[MAX_MOD_KNOWN];
static int		mod_numknown;

// the inline * models from the current map are kept seperate
static model_t	mod_inline[MAX_MOD_KNOWN];

/*
===============================================================================

				RENDERER-LOCAL HUNK ALLOCATOR

The original loader relied on the platform's Hunk_Begin/Alloc/End/Free. Here we
provide simple malloc-based versions with the same semantics: Hunk_Begin mallocs
and zeroes a buffer, Hunk_Alloc bump-allocates (32-byte aligned) from it, and
Hunk_End returns how many bytes were used so the model records extradatasize.

===============================================================================
*/

static byte	*hunk_base;
static int	hunk_maxsize;
static int	hunk_cursize;

static void *Hunk_Begin (int maxsize)
{
	hunk_maxsize = maxsize;
	hunk_cursize = 0;
	hunk_base = malloc (maxsize);
	if (!hunk_base)
		ri.Sys_Error (ERR_FATAL, "Hunk_Begin: failed to allocate %i bytes", maxsize);
	memset (hunk_base, 0, maxsize);
	return hunk_base;
}

static void *Hunk_Alloc (int size)
{
	byte	*buf;

	// round to cacheline (32-byte alignment)
	size = (size + 31) & ~31;

	if (hunk_cursize + size > hunk_maxsize)
		ri.Sys_Error (ERR_FATAL, "Hunk_Alloc: overflow (%i bytes)", size);

	buf = hunk_base + hunk_cursize;
	hunk_cursize += size;
	return buf;
}

static int Hunk_End (void)
{
	return hunk_cursize;
}

static void Hunk_Free (void *base)
{
	if (base)
		free (base);
}

/*
===============
GL3_Mod_PointInLeaf
===============
*/
mleaf_t *GL3_Mod_PointInLeaf (vec3_t p, model_t *model)
{
	mnode_t		*node;
	float		d;
	cplane_t	*plane;

	if (!model || !model->nodes)
		ri.Sys_Error (ERR_DROP, "Mod_PointInLeaf: bad model");

	node = model->nodes;
	while (1)
	{
		if (node->contents != -1)
			return (mleaf_t *)node;
		plane = node->plane;
		d = DotProduct (p,plane->normal) - plane->dist;
		if (d > 0)
			node = node->children[0];
		else
			node = node->children[1];
	}

	return NULL;	// never reached
}


/*
===================
Mod_DecompressVis
===================
*/
static byte *Mod_DecompressVis (byte *in, model_t *model)
{
	static byte	decompressed[MAX_MAP_LEAFS/8];
	int		c;
	byte	*out;
	int		row;

	row = (model->vis->numclusters+7)>>3;
	out = decompressed;

	if (!in)
	{	// no vis info, so make all visible
		while (row)
		{
			*out++ = 0xff;
			row--;
		}
		return decompressed;
	}

	do
	{
		if (*in)
		{
			*out++ = *in++;
			continue;
		}

		c = in[1];
		in += 2;
		if ((out - decompressed) + c > row)	// clamp the run so a crafted
			c = row - (out - decompressed);	// vis lump can't overrun decompressed[]
		while (c)
		{
			*out++ = 0;
			c--;
		}
	} while (out - decompressed < row);

	return decompressed;
}

/*
==============
GL3_Mod_ClusterPVS
==============
*/
byte *GL3_Mod_ClusterPVS (int cluster, model_t *model)
{
	// reject an out-of-range cluster (crafted leaf) before it indexes bitofs[]
	if (cluster < 0 || !model->vis || cluster >= model->vis->numclusters)
		return mod_novis;
	return Mod_DecompressVis ( (byte *)model->vis + model->vis->bitofs[cluster][DVIS_PVS],
		model);
}


//===============================================================================

/*
================
GL3_Mod_Modellist_f
================
*/
void GL3_Mod_Modellist_f (void)
{
	int		i;
	model_t	*mod;
	int		total;

	total = 0;
	ri.Con_Printf (PRINT_ALL,"Loaded models:\n");
	for (i=0, mod=mod_known ; i < mod_numknown ; i++, mod++)
	{
		if (!mod->name[0])
			continue;
		ri.Con_Printf (PRINT_ALL, "%8i : %s\n",mod->extradatasize, mod->name);
		total += mod->extradatasize;
	}
	ri.Con_Printf (PRINT_ALL, "Total resident: %i\n", total);
}

/*
===============
GL3_Mod_Init
===============
*/
void GL3_Mod_Init (void)
{
	memset (mod_novis, 0xff, sizeof(mod_novis));
}



/*
==================
GL3_Mod_ForName

Loads in a model for the given name
==================
*/
model_t *GL3_Mod_ForName (char *name, qboolean crash)
{
	model_t	*mod;
	unsigned *buf;
	int		i;

	if (!name[0])
		ri.Sys_Error (ERR_DROP, "Mod_ForName: NULL name");

	//
	// inline models are grabbed only from worldmodel
	//
	if (name[0] == '*')
	{
		i = atoi(name+1);
		if (i < 1 || !r_worldmodel || i >= r_worldmodel->numsubmodels)
			ri.Sys_Error (ERR_DROP, "bad inline model number");
		return &mod_inline[i];
	}

	//
	// search the currently loaded models
	//
	for (i=0 , mod=mod_known ; i<mod_numknown ; i++, mod++)
	{
		if (!mod->name[0])
			continue;
		if (!strcmp (mod->name, name) )
			return mod;
	}

	//
	// find a free model slot spot
	//
	for (i=0 , mod=mod_known ; i<mod_numknown ; i++, mod++)
	{
		if (!mod->name[0])
			break;	// free spot
	}
	if (i == mod_numknown)
	{
		if (mod_numknown == MAX_MOD_KNOWN)
		{
			// asset cache kept old maps' models around — evict the stale
			// ones (not registered this sequence) and retry
			for (i=0 , mod=mod_known ; i<mod_numknown ; i++, mod++)
			{
				if (mod->name[0] && mod->registration_sequence != registration_sequence)
					Mod_Free (mod);
			}
			for (i=0 , mod=mod_known ; i<mod_numknown ; i++, mod++)
			{
				if (!mod->name[0])
					break;
			}
			if (i == mod_numknown)
				ri.Sys_Error (ERR_DROP, "mod_numknown == MAX_MOD_KNOWN");
		}
		else
			mod_numknown++;
	}
	strcpy (mod->name, name);

	//
	// load the file
	//
	modfilelen = ri.FS_LoadFile (mod->name, (void **)&buf);
	if (!buf)
	{
		if (crash)
			ri.Sys_Error (ERR_DROP, "Mod_NumForName: %s not found", mod->name);
		memset (mod->name, 0, sizeof(mod->name));
		return NULL;
	}

	loadmodel = mod;

	//
	// fill it in
	//


	// call the apropriate loader

	switch (LittleLong(*(unsigned *)buf))
	{
	case IDALIASHEADER:
		loadmodel->extradata = Hunk_Begin (0x200000);
		Mod_LoadAliasModel (mod, buf);
		break;

	case IDSPRITEHEADER:
		loadmodel->extradata = Hunk_Begin (0x10000);
		Mod_LoadSpriteModel (mod, buf);
		break;

	case IDBSPHEADER:
		loadmodel->extradata = Hunk_Begin (0x1000000);
		Mod_LoadBrushModel (mod, buf);
		break;

	default:
		ri.Sys_Error (ERR_DROP,"Mod_NumForName: unknown fileid for %s", mod->name);
		break;
	}

	loadmodel->extradatasize = Hunk_End ();

	ri.FS_FreeFile (buf);

	return mod;
}

/*
===============================================================================

					BRUSHMODEL LOADING

===============================================================================
*/

static byte	*mod_base;
static int	mod_lightdatasize;	// bytes in the lighting lump (bounds face lightofs)


/*
=================
Mod_LoadLighting
=================
*/
static void Mod_LoadLighting (lump_t *l)
{
	if (!l->filelen)
	{
		loadmodel->lightdata = NULL;
		mod_lightdatasize = 0;
		return;
	}
	loadmodel->lightdata = Hunk_Alloc ( l->filelen);
	mod_lightdatasize = l->filelen;
	memcpy (loadmodel->lightdata, mod_base + l->fileofs, l->filelen);
}


/*
=================
Mod_LoadVisibility
=================
*/
static void Mod_LoadVisibility (lump_t *l)
{
	int		i;

	if (!l->filelen)
	{
		loadmodel->vis = NULL;
		return;
	}
	loadmodel->vis = Hunk_Alloc ( l->filelen);
	memcpy (loadmodel->vis, mod_base + l->fileofs, l->filelen);

	if (l->filelen < 4)
		ri.Sys_Error (ERR_DROP, "Mod_LoadVisibility: %s has a truncated vis lump", loadmodel->name);
	loadmodel->vis->numclusters = LittleLong (loadmodel->vis->numclusters);
	// the bitofs[numclusters][2] table must fit inside the lump we allocated
	// (4-byte numclusters header + 8 bytes per cluster); div form avoids
	// overflow in numclusters * 8. Also cap at MAX_MAP_LEAFS so the decode row
	// (numclusters+7)>>3 cannot exceed Mod_DecompressVis's decompressed[] buffer.
	if (loadmodel->vis->numclusters < 0
		|| loadmodel->vis->numclusters > MAX_MAP_LEAFS
		|| loadmodel->vis->numclusters > (l->filelen - 4) / 8)
		ri.Sys_Error (ERR_DROP, "Mod_LoadVisibility: %s has bad numclusters", loadmodel->name);
	for (i=0 ; i<loadmodel->vis->numclusters ; i++)
	{
		loadmodel->vis->bitofs[i][0] = LittleLong (loadmodel->vis->bitofs[i][0]);
		loadmodel->vis->bitofs[i][1] = LittleLong (loadmodel->vis->bitofs[i][1]);
	}
}


/*
=================
Mod_LoadVertexes
=================
*/
static void Mod_LoadVertexes (lump_t *l)
{
	dvertex_t	*in;
	mvertex_t	*out;
	int			i, count;

	in = (void *)(mod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
		ri.Sys_Error (ERR_DROP, "MOD_LoadBmodel: funny lump size in %s",loadmodel->name);
	count = l->filelen / sizeof(*in);
	out = Hunk_Alloc ( count*sizeof(*out));

	loadmodel->vertexes = out;
	loadmodel->numvertexes = count;

	for ( i=0 ; i<count ; i++, in++, out++)
	{
		out->position[0] = LittleFloat (in->point[0]);
		out->position[1] = LittleFloat (in->point[1]);
		out->position[2] = LittleFloat (in->point[2]);
	}
}

/*
=================
RadiusFromBounds
=================
*/
static float RadiusFromBounds (vec3_t mins, vec3_t maxs)
{
	int		i;
	vec3_t	corner;

	for (i=0 ; i<3 ; i++)
	{
		corner[i] = fabs(mins[i]) > fabs(maxs[i]) ? fabs(mins[i]) : fabs(maxs[i]);
	}

	return VectorLength (corner);
}


/*
=================
Mod_LoadSubmodels
=================
*/
static void Mod_LoadSubmodels (lump_t *l)
{
	dmodel_t	*in;
	mmodel_t	*out;
	int			i, j, count;

	in = (void *)(mod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
		ri.Sys_Error (ERR_DROP, "MOD_LoadBmodel: funny lump size in %s",loadmodel->name);
	count = l->filelen / sizeof(*in);
	out = Hunk_Alloc ( count*sizeof(*out));

	loadmodel->submodels = out;
	loadmodel->numsubmodels = count;

	for ( i=0 ; i<count ; i++, in++, out++)
	{
		for (j=0 ; j<3 ; j++)
		{	// spread the mins / maxs by a pixel
			out->mins[j] = LittleFloat (in->mins[j]) - 1;
			out->maxs[j] = LittleFloat (in->maxs[j]) + 1;
			out->origin[j] = LittleFloat (in->origin[j]);
		}
		out->radius = RadiusFromBounds (out->mins, out->maxs);
		out->headnode = LittleLong (in->headnode);
		out->firstface = LittleLong (in->firstface);
		out->numfaces = LittleLong (in->numfaces);
	}
}

/*
=================
Mod_LoadEdges
=================
*/
static void Mod_LoadEdges (lump_t *l)
{
	dedge_t *in;
	medge_t *out;
	int 	i, count;

	in = (void *)(mod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
		ri.Sys_Error (ERR_DROP, "MOD_LoadBmodel: funny lump size in %s",loadmodel->name);
	count = l->filelen / sizeof(*in);
	out = Hunk_Alloc ( (count + 1) * sizeof(*out));

	loadmodel->edges = out;
	loadmodel->numedges = count;

	for ( i=0 ; i<count ; i++, in++, out++)
	{
		out->v[0] = (unsigned short)LittleShort(in->v[0]);
		out->v[1] = (unsigned short)LittleShort(in->v[1]);
		if ((int)out->v[0] >= loadmodel->numvertexes
			|| (int)out->v[1] >= loadmodel->numvertexes)
			ri.Sys_Error (ERR_DROP, "Mod_LoadEdges: bad vertex index in %s", loadmodel->name);
	}
}

/*
=================
Mod_LoadTexinfo
=================
*/
static void Mod_LoadTexinfo (lump_t *l)
{
	texinfo_t *in;
	mtexinfo_t *out, *step;
	int 	i, j, count;
	char	name[MAX_QPATH];
	int		next;

	in = (void *)(mod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
		ri.Sys_Error (ERR_DROP, "MOD_LoadBmodel: funny lump size in %s",loadmodel->name);
	count = l->filelen / sizeof(*in);
	out = Hunk_Alloc ( count*sizeof(*out));

	loadmodel->texinfo = out;
	loadmodel->numtexinfo = count;

	for ( i=0 ; i<count ; i++, in++, out++)
	{
		for (j=0 ; j<8 ; j++)
			out->vecs[0][j] = LittleFloat (in->vecs[0][j]);

		out->flags = LittleLong (in->flags);
		next = LittleLong (in->nexttexinfo);
		if (next > 0)
		{
			if (next >= count)	// animation chain is walked as a pointer list
				ri.Sys_Error (ERR_DROP, "Mod_LoadTexinfo: bad nexttexinfo in %s", loadmodel->name);
			out->next = loadmodel->texinfo + next;
		}
		else
		    out->next = NULL;
		Com_sprintf (name, sizeof(name), "textures/%s.wal", in->texture);

		out->image = GL3_FindImage (name, it_wall);
		if (!out->image)
		{
			ri.Con_Printf (PRINT_ALL, "Couldn't load %s\n", name);
			out->image = r_notexture;
		}
	}

	// count animation frames
	for (i=0 ; i<count ; i++)
	{
		out = &loadmodel->texinfo[i];
		out->numframes = 1;
		for (step = out->next ; step && step != out ; step=step->next)
			out->numframes++;
	}
}

/*
================
CalcSurfaceExtents

Fills in s->texturemins[] and s->extents[]
================
*/
static void CalcSurfaceExtents (msurface_t *s)
{
	float	mins[2], maxs[2], val;
	int		i,j, e;
	mvertex_t	*v;
	mtexinfo_t	*tex;
	int		bmins[2], bmaxs[2];

	mins[0] = mins[1] = 999999;
	maxs[0] = maxs[1] = -99999;

	tex = s->texinfo;

	for (i=0 ; i<s->numedges ; i++)
	{
		e = loadmodel->surfedges[s->firstedge+i];
		if (e >= 0)
			v = &loadmodel->vertexes[loadmodel->edges[e].v[0]];
		else
			v = &loadmodel->vertexes[loadmodel->edges[-e].v[1]];

		for (j=0 ; j<2 ; j++)
		{
			val = v->position[0] * tex->vecs[j][0] +
				v->position[1] * tex->vecs[j][1] +
				v->position[2] * tex->vecs[j][2] +
				tex->vecs[j][3];
			if (val < mins[j])
				mins[j] = val;
			if (val > maxs[j])
				maxs[j] = val;
		}
	}

	for (i=0 ; i<2 ; i++)
	{
		bmins[i] = floor(mins[i]/16);
		bmaxs[i] = ceil(maxs[i]/16);

		s->texturemins[i] = bmins[i] * 16;
		s->extents[i] = (bmaxs[i] - bmins[i]) * 16;

//		if ( !(tex->flags & TEX_SPECIAL) && s->extents[i] > 512 /* 256 */ )
//			ri.Sys_Error (ERR_DROP, "Bad surface extents");
	}
}


/*
=================
Mod_LoadFaces
=================
*/
static void Mod_LoadFaces (lump_t *l)
{
	dface_t		*in;
	msurface_t 	*out;
	int			i, count, surfnum;
	int			planenum, side;
	int			ti;

	in = (void *)(mod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
		ri.Sys_Error (ERR_DROP, "MOD_LoadBmodel: funny lump size in %s",loadmodel->name);
	count = l->filelen / sizeof(*in);
	out = Hunk_Alloc ( count*sizeof(*out));

	loadmodel->surfaces = out;
	loadmodel->numsurfaces = count;

	GL3_BeginBuildingLightmaps (loadmodel);

	for ( surfnum=0 ; surfnum<count ; surfnum++, in++, out++)
	{
		out->firstedge = LittleLong(in->firstedge);
		out->numedges = LittleShort(in->numedges);
		out->flags = 0;
		out->polys = NULL;

		// the surface's edge run must lie within the surfedges lump
		if (out->numedges < 0 || out->firstedge < 0
			|| out->firstedge > loadmodel->numsurfedges - out->numedges)
			ri.Sys_Error (ERR_DROP, "Mod_LoadFaces: bad edge range in %s", loadmodel->name);

		planenum = (unsigned short)LittleShort(in->planenum);
		side = LittleShort(in->side);
		if (side)
			out->flags |= SURF_PLANEBACK;

		if (planenum >= loadmodel->numplanes)
			ri.Sys_Error (ERR_DROP, "Mod_LoadFaces: bad planenum in %s", loadmodel->name);
		out->plane = loadmodel->planes + planenum;

		ti = LittleShort (in->texinfo);
		if (ti < 0 || ti >= loadmodel->numtexinfo)
			ri.Sys_Error (ERR_DROP, "MOD_LoadBmodel: bad texinfo number");
		out->texinfo = loadmodel->texinfo + ti;

		CalcSurfaceExtents (out);

	// lighting info

		for (i=0 ; i<MAXLIGHTMAPS ; i++)
			out->styles[i] = in->styles[i];
		i = LittleLong(in->lightofs);
		if (i == -1)
			out->samples = NULL;
		else
		{
			// R_BuildLightMap reads nummaps * smax*tmax*3 bytes starting at
			// samples; keep that whole block inside the lighting lump (extents
			// here are the real ones -- the SURF_WARP override runs below)
			int	smax = (out->extents[0] >> 4) + 1;
			int	tmax = (out->extents[1] >> 4) + 1;
			int	nmaps;
			long long	need;
			for (nmaps = 0; nmaps < MAXLIGHTMAPS && out->styles[nmaps] != 255; nmaps++)
				;
			need = (long long)smax * tmax * 3 * nmaps;
			if (i < 0 || smax <= 0 || tmax <= 0 || i > mod_lightdatasize - need)
				ri.Sys_Error (ERR_DROP, "Mod_LoadFaces: bad lightofs in %s", loadmodel->name);
			out->samples = loadmodel->lightdata + i;
		}

	// set the drawing flags

		if (out->texinfo->flags & SURF_SKY)
			out->flags |= SURF_DRAWSKY;	// skipped by the world pass so the
										// skybox shows through the hole

		if (out->texinfo->flags & SURF_WARP)
		{
			out->flags |= SURF_DRAWTURB;
			// classify the liquid by texture name: lava is never blended,
			// and acid/sewage uses gl_slimealpha (opaque by default -- it
			// hides submerged secrets like base3's door) instead of
			// gl_wateralpha
			if (out->texinfo->image)
			{
				const char *name = out->texinfo->image->name;
				if (strstr (name, "lava"))
					out->flags |= SURF_DRAWLAVA;
				else if (strstr (name, "sewer") || strstr (name, "slime") || strstr (name, "tox"))
					out->flags |= SURF_DRAWSLIME;
			}
			for (i=0 ; i<2 ; i++)
			{
				out->extents[i] = 16384;
				out->texturemins[i] = -8192;
			}
			// original: GL_SubdivideSurface (out) cut the polygon up for
			// warps. That is not ported here; GL3_BuildPolygonFromSurface
			// below builds a single (unsubdivided) polygon instead.
		}

		// create lightmaps and polygons
		if ( !(out->texinfo->flags & (SURF_SKY|SURF_TRANS33|SURF_TRANS66|SURF_WARP) ) )
			GL3_CreateSurfaceLightmap (out);

		GL3_BuildPolygonFromSurface (out);
	}

	GL3_EndBuildingLightmaps ();
}


/*
=================
Mod_SetParent
=================
*/
static void Mod_SetParent (mnode_t *node, mnode_t *parent)
{
	node->parent = parent;
	if (node->contents != -1)
		return;
	Mod_SetParent (node->children[0], node);
	Mod_SetParent (node->children[1], node);
}

/*
=================
Mod_LoadNodes
=================
*/
static void Mod_LoadNodes (lump_t *l)
{
	int			i, j, count, p;
	dnode_t		*in;
	mnode_t 	*out;

	in = (void *)(mod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
		ri.Sys_Error (ERR_DROP, "MOD_LoadBmodel: funny lump size in %s",loadmodel->name);
	count = l->filelen / sizeof(*in);
	out = Hunk_Alloc ( count*sizeof(*out));

	loadmodel->nodes = out;
	loadmodel->numnodes = count;

	for ( i=0 ; i<count ; i++, in++, out++)
	{
		for (j=0 ; j<3 ; j++)
		{
			out->minmaxs[j] = LittleShort (in->mins[j]);
			out->minmaxs[3+j] = LittleShort (in->maxs[j]);
		}

		p = LittleLong(in->planenum);
		if (p < 0 || p >= loadmodel->numplanes)
			ri.Sys_Error (ERR_DROP, "Mod_LoadNodes: bad planenum in %s", loadmodel->name);
		out->plane = loadmodel->planes + p;

		out->firstsurface = (unsigned short)LittleShort (in->firstface);
		out->numsurfaces = (unsigned short)LittleShort (in->numfaces);
		out->contents = -1;	// differentiate from leafs

		// the node's surface run indexes the surfaces lump when the world is
		// drawn (GL3_RecursiveWorldNode etc.) -- keep it in range
		if ((int)out->firstsurface + (int)out->numsurfaces > loadmodel->numsurfaces)
			ri.Sys_Error (ERR_DROP, "Mod_LoadNodes: bad surface range in %s", loadmodel->name);

		for (j=0 ; j<2 ; j++)
		{
			p = LittleLong (in->children[j]);
			// unchecked, these become raw pointers that Mod_SetParent writes
			// through (node->parent = ...) -> a write-what-where primitive
			if (p >= 0)
			{
				if (p >= loadmodel->numnodes)
					ri.Sys_Error (ERR_DROP, "Mod_LoadNodes: bad child node in %s", loadmodel->name);
				out->children[j] = loadmodel->nodes + p;
			}
			else
			{
				if ((-1 - p) < 0 || (-1 - p) >= loadmodel->numleafs)
					ri.Sys_Error (ERR_DROP, "Mod_LoadNodes: bad child leaf in %s", loadmodel->name);
				out->children[j] = (mnode_t *)(loadmodel->leafs + (-1 - p));
			}
		}
	}

	Mod_SetParent (loadmodel->nodes, NULL);	// sets nodes and leafs
}

/*
=================
Mod_LoadLeafs
=================
*/
static void Mod_LoadLeafs (lump_t *l)
{
	dleaf_t 	*in;
	mleaf_t 	*out;
	int			i, j, count, p;

	in = (void *)(mod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
		ri.Sys_Error (ERR_DROP, "MOD_LoadBmodel: funny lump size in %s",loadmodel->name);
	count = l->filelen / sizeof(*in);
	out = Hunk_Alloc ( count*sizeof(*out));

	loadmodel->leafs = out;
	loadmodel->numleafs = count;

	for ( i=0 ; i<count ; i++, in++, out++)
	{
		for (j=0 ; j<3 ; j++)
		{
			out->minmaxs[j] = LittleShort (in->mins[j]);
			out->minmaxs[3+j] = LittleShort (in->maxs[j]);
		}

		p = LittleLong(in->contents);
		out->contents = p;

		out->cluster = LittleShort(in->cluster);
		// -1 (no cluster) is valid; anything below indexes vis[cluster>>3]
		// out of bounds in GL3_MarkLeaves
		if (out->cluster < -1)
			ri.Sys_Error (ERR_DROP, "Mod_LoadLeafs: bad cluster in %s", loadmodel->name);
		out->area = LittleShort(in->area);
		// area indexes r_newrefdef.areabits[area>>3] (32 bytes) at render time
		if (out->area < 0 || out->area >= MAX_MAP_AREAS)
			ri.Sys_Error (ERR_DROP, "Mod_LoadLeafs: bad area in %s", loadmodel->name);

		{
			unsigned short firstmark = (unsigned short)LittleShort(in->firstleafface);
			unsigned short nummark   = (unsigned short)LittleShort(in->numleaffaces);
			// the leaf's marksurface run must lie within the marksurfaces lump
			if ((int)firstmark + (int)nummark > loadmodel->nummarksurfaces)
				ri.Sys_Error (ERR_DROP, "Mod_LoadLeafs: bad marksurface range in %s", loadmodel->name);
			out->firstmarksurface = loadmodel->marksurfaces + firstmark;
			out->nummarksurfaces = nummark;
		}
	}
}

/*
=================
Mod_LoadMarksurfaces
=================
*/
static void Mod_LoadMarksurfaces (lump_t *l)
{
	int		i, j, count;
	short		*in;
	msurface_t **out;

	in = (void *)(mod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
		ri.Sys_Error (ERR_DROP, "MOD_LoadBmodel: funny lump size in %s",loadmodel->name);
	count = l->filelen / sizeof(*in);
	out = Hunk_Alloc ( count*sizeof(*out));

	loadmodel->marksurfaces = out;
	loadmodel->nummarksurfaces = count;

	for ( i=0 ; i<count ; i++)
	{
		j = LittleShort(in[i]);
		if (j < 0 ||  j >= loadmodel->numsurfaces)
			ri.Sys_Error (ERR_DROP, "Mod_ParseMarksurfaces: bad surface number");
		out[i] = loadmodel->surfaces + j;
	}
}

/*
=================
Mod_LoadSurfedges
=================
*/
static void Mod_LoadSurfedges (lump_t *l)
{
	int		i, count;
	int		*in, *out;

	in = (void *)(mod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
		ri.Sys_Error (ERR_DROP, "MOD_LoadBmodel: funny lump size in %s",loadmodel->name);
	count = l->filelen / sizeof(*in);
	if (count < 1 || count >= MAX_MAP_SURFEDGES)
		ri.Sys_Error (ERR_DROP, "MOD_LoadBmodel: bad surfedges count in %s: %i",
		loadmodel->name, count);

	out = Hunk_Alloc ( count*sizeof(*out));

	loadmodel->surfedges = out;
	loadmodel->numsurfedges = count;

	for ( i=0 ; i<count ; i++)
	{
		out[i] = LittleLong (in[i]);
		// used as edges[out[i]] (out[i]>=0) or edges[-out[i]]; |value| < numedges
		if (out[i] >= loadmodel->numedges || out[i] <= -loadmodel->numedges)
			ri.Sys_Error (ERR_DROP, "Mod_LoadSurfedges: bad edge index in %s", loadmodel->name);
	}
}


/*
=================
Mod_LoadPlanes
=================
*/
static void Mod_LoadPlanes (lump_t *l)
{
	int			i, j;
	cplane_t	*out;
	dplane_t 	*in;
	int			count;
	int			bits;

	in = (void *)(mod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
		ri.Sys_Error (ERR_DROP, "MOD_LoadBmodel: funny lump size in %s",loadmodel->name);
	count = l->filelen / sizeof(*in);
	out = Hunk_Alloc ( count*2*sizeof(*out));

	loadmodel->planes = out;
	loadmodel->numplanes = count;

	for ( i=0 ; i<count ; i++, in++, out++)
	{
		bits = 0;
		for (j=0 ; j<3 ; j++)
		{
			out->normal[j] = LittleFloat (in->normal[j]);
			if (out->normal[j] < 0)
				bits |= 1<<j;
		}

		out->dist = LittleFloat (in->dist);
		out->type = LittleLong (in->type);
		out->signbits = bits;
	}
}

/*
=================
Mod_LoadBrushModel
=================
*/
static void Mod_LoadBrushModel (model_t *mod, void *buffer)
{
	int			i;
	dheader_t	*header;
	mmodel_t 	*bm;

	loadmodel->type = mod_brush;
	if (loadmodel != mod_known)
		ri.Sys_Error (ERR_DROP, "Loaded a brush model after the world");

	header = (dheader_t *)buffer;

	i = LittleLong (header->version);
	if (i != BSPVERSION)
		ri.Sys_Error (ERR_DROP, "Mod_LoadBrushModel: %s has wrong version number (%i should be %i)", mod->name, i, BSPVERSION);

// swap all the lumps
	mod_base = (byte *)header;

	for (i=0 ; i<sizeof(dheader_t)/4 ; i++)
		((int *)header)[i] = LittleLong ( ((int *)header)[i]);

	// validate every lump lies within the loaded file before the individual
	// loaders trust fileofs/filelen. modfilelen is the size of this .bsp.
	// The subtraction can't underflow: fileofs is checked >= 0 and the
	// > modfilelen short-circuits before modfilelen - filelen is used.
	for (i=0 ; i<HEADER_LUMPS ; i++)
	{
		lump_t	*l = &header->lumps[i];
		if (l->fileofs < 0 || l->filelen < 0
			|| l->fileofs > modfilelen
			|| l->filelen > modfilelen - l->fileofs)
			ri.Sys_Error (ERR_DROP, "Mod_LoadBrushModel: %s lump %i out of bounds",
					 mod->name, i);
	}

// load into heap

	Mod_LoadVertexes (&header->lumps[LUMP_VERTEXES]);
	Mod_LoadEdges (&header->lumps[LUMP_EDGES]);
	Mod_LoadSurfedges (&header->lumps[LUMP_SURFEDGES]);
	Mod_LoadLighting (&header->lumps[LUMP_LIGHTING]);
	Mod_LoadPlanes (&header->lumps[LUMP_PLANES]);
	Mod_LoadTexinfo (&header->lumps[LUMP_TEXINFO]);
	Mod_LoadFaces (&header->lumps[LUMP_FACES]);
	Mod_LoadMarksurfaces (&header->lumps[LUMP_LEAFFACES]);
	Mod_LoadVisibility (&header->lumps[LUMP_VISIBILITY]);
	Mod_LoadLeafs (&header->lumps[LUMP_LEAFS]);
	Mod_LoadNodes (&header->lumps[LUMP_NODES]);
	Mod_LoadSubmodels (&header->lumps[LUMP_MODELS]);
	mod->numframes = 2;		// regular and alternate animation

//
// set up the submodels
//
	for (i=0 ; i<mod->numsubmodels ; i++)
	{
		model_t	*starmod;

		bm = &mod->submodels[i];
		starmod = &mod_inline[i];

		*starmod = *loadmodel;

		starmod->firstmodelsurface = bm->firstface;
		starmod->nummodelsurfaces = bm->numfaces;
		// GL3_DrawBrushModel walks surfaces[firstmodelsurface .. +nummodel]
		if (bm->firstface < 0 || bm->numfaces < 0
			|| bm->firstface > loadmodel->numsurfaces - bm->numfaces)
			ri.Sys_Error (ERR_DROP, "Inline model %i has bad surface range", i);
		starmod->firstnode = bm->headnode;
		// a valid inline model may have a negative (leaf-encoded) headnode --
		// only the upper bound is a load error; the dynamic-light path guards
		// the negative case itself (GL3_DrawBrushModel)
		if (starmod->firstnode >= loadmodel->numnodes)
			ri.Sys_Error (ERR_DROP, "Inline model %i has bad firstnode", i);

		VectorCopy (bm->maxs, starmod->maxs);
		VectorCopy (bm->mins, starmod->mins);
		starmod->radius = bm->radius;

		if (i == 0)
			*loadmodel = *starmod;

		starmod->numleafs = bm->visleafs;
	}
}

/*
==============================================================================

ALIAS MODELS

==============================================================================
*/

/*
=================
Mod_LoadAliasModel
=================
*/
static void Mod_LoadAliasModel (model_t *mod, void *buffer)
{
	int					i, j;
	dmdl_t				*pinmodel, *pheader;
	dstvert_t			*pinst, *poutst;
	dtriangle_t			*pintri, *pouttri;
	daliasframe_t		*pinframe, *poutframe;
	int					*pincmd, *poutcmd;
	int					version;
	int					ofsend;

	pinmodel = (dmdl_t *)buffer;

	version = LittleLong (pinmodel->version);
	if (version != ALIAS_VERSION)
		ri.Sys_Error (ERR_DROP, "%s has wrong version number (%i should be %i)",
				 mod->name, version, ALIAS_VERSION);

	// ofs_end is the model's self-declared size and the size we allocate; it
	// must be sane and lie within the file we actually loaded
	ofsend = LittleLong (pinmodel->ofs_end);
	if (ofsend < (int)sizeof(dmdl_t) || ofsend > modfilelen)
		ri.Sys_Error (ERR_DROP, "%s has a bad ofs_end (%i, file is %i)",
				 mod->name, ofsend, modfilelen);

	pheader = Hunk_Alloc (ofsend);

	// byte swap the header fields and sanity check
	for (i=0 ; i<sizeof(dmdl_t)/4 ; i++)
		((int *)pheader)[i] = LittleLong (((int *)buffer)[i]);

	if (pheader->skinheight > MAX_LBM_HEIGHT)
		ri.Sys_Error (ERR_DROP, "model %s has a skin taller than %d", mod->name,
				   MAX_LBM_HEIGHT);

	if (pheader->num_xyz <= 0)
		ri.Sys_Error (ERR_DROP, "model %s has no vertices", mod->name);

	if (pheader->num_xyz > MAX_VERTS)
		ri.Sys_Error (ERR_DROP, "model %s has too many vertices", mod->name);

	if (pheader->num_st <= 0)
		ri.Sys_Error (ERR_DROP, "model %s has no st vertices", mod->name);

	if (pheader->num_tris <= 0)
		ri.Sys_Error (ERR_DROP, "model %s has no triangles", mod->name);

	if (pheader->num_frames <= 0)
		ri.Sys_Error (ERR_DROP, "model %s has no frames", mod->name);

	// Every lump is written into the ofs_end-sized hunk block below at an
	// offset/count taken from the header. Validate that each region fits, so
	// a crafted model cannot write past the allocation (or read past the
	// file, since ofs_end <= modfilelen). 64-bit math avoids overflow in the
	// offset + count*size products.
	if (pheader->num_skins < 0 || pheader->num_skins > MAX_MD2SKINS)
		ri.Sys_Error (ERR_DROP, "model %s has too many skins (%i)",
				 mod->name, pheader->num_skins);
	if (pheader->num_glcmds < 0)
		ri.Sys_Error (ERR_DROP, "model %s has a bad glcmd count", mod->name);
	if (pheader->framesize < (int)(sizeof(daliasframe_t)
			+ (pheader->num_xyz - 1) * sizeof(dtrivertx_t)))
		ri.Sys_Error (ERR_DROP, "model %s has a bad framesize", mod->name);

	if (pheader->ofs_skins  < (int)sizeof(dmdl_t)
	 || pheader->ofs_st     < (int)sizeof(dmdl_t)
	 || pheader->ofs_tris   < (int)sizeof(dmdl_t)
	 || pheader->ofs_glcmds < (int)sizeof(dmdl_t)
	 || pheader->ofs_frames < (int)sizeof(dmdl_t)
	 || (long long)pheader->ofs_skins  + (long long)pheader->num_skins  * MAX_SKINNAME          > ofsend
	 || (long long)pheader->ofs_st     + (long long)pheader->num_st     * sizeof(dstvert_t)     > ofsend
	 || (long long)pheader->ofs_tris   + (long long)pheader->num_tris   * sizeof(dtriangle_t)   > ofsend
	 || (long long)pheader->ofs_glcmds + (long long)pheader->num_glcmds * sizeof(int)           > ofsend
	 || (long long)pheader->ofs_frames + (long long)pheader->num_frames * pheader->framesize    > ofsend)
		ri.Sys_Error (ERR_DROP, "model %s has a lump that overruns the file", mod->name);

//
// load base s and t vertices (not used in gl version)
//
	pinst = (dstvert_t *) ((byte *)pinmodel + pheader->ofs_st);
	poutst = (dstvert_t *) ((byte *)pheader + pheader->ofs_st);

	for (i=0 ; i<pheader->num_st ; i++)
	{
		poutst[i].s = LittleShort (pinst[i].s);
		poutst[i].t = LittleShort (pinst[i].t);
	}

//
// load triangle lists
//
	pintri = (dtriangle_t *) ((byte *)pinmodel + pheader->ofs_tris);
	pouttri = (dtriangle_t *) ((byte *)pheader + pheader->ofs_tris);

	for (i=0 ; i<pheader->num_tris ; i++)
	{
		for (j=0 ; j<3 ; j++)
		{
			pouttri[i].index_xyz[j] = LittleShort (pintri[i].index_xyz[j]);
			pouttri[i].index_st[j] = LittleShort (pintri[i].index_st[j]);
		}
	}

//
// load the frames
//
	for (i=0 ; i<pheader->num_frames ; i++)
	{
		pinframe = (daliasframe_t *) ((byte *)pinmodel
			+ pheader->ofs_frames + i * pheader->framesize);
		poutframe = (daliasframe_t *) ((byte *)pheader
			+ pheader->ofs_frames + i * pheader->framesize);

		memcpy (poutframe->name, pinframe->name, sizeof(poutframe->name));
		for (j=0 ; j<3 ; j++)
		{
			poutframe->scale[j] = LittleFloat (pinframe->scale[j]);
			poutframe->translate[j] = LittleFloat (pinframe->translate[j]);
		}
		// verts are all 8 bit, so no swapping needed
		memcpy (poutframe->verts, pinframe->verts,
			pheader->num_xyz*sizeof(dtrivertx_t));

	}

	mod->type = mod_alias;

	//
	// load the glcmds
	//
	pincmd = (int *) ((byte *)pinmodel + pheader->ofs_glcmds);
	poutcmd = (int *) ((byte *)pheader + pheader->ofs_glcmds);
	for (i=0 ; i<pheader->num_glcmds ; i++)
		poutcmd[i] = LittleLong (pincmd[i]);


	// register all skins
	memcpy ((char *)pheader + pheader->ofs_skins, (char *)pinmodel + pheader->ofs_skins,
		pheader->num_skins*MAX_SKINNAME);
	for (i=0 ; i<pheader->num_skins ; i++)
	{
		char *skinname = (char *)pheader + pheader->ofs_skins + i*MAX_SKINNAME;
		skinname[MAX_SKINNAME-1] = 0;	// a crafted skin field may not be terminated
		mod->skins[i] = GL3_RegisterSkin (skinname);
	}

	mod->mins[0] = -32;
	mod->mins[1] = -32;
	mod->mins[2] = -32;
	mod->maxs[0] = 32;
	mod->maxs[1] = 32;
	mod->maxs[2] = 32;
}

/*
==============================================================================

SPRITE MODELS

==============================================================================
*/

/*
=================
Mod_LoadSpriteModel
=================
*/
static void Mod_LoadSpriteModel (model_t *mod, void *buffer)
{
	dsprite_t	*sprin, *sprout;
	int			i;

	sprin = (dsprite_t *)buffer;
	sprout = Hunk_Alloc (modfilelen);

	// the fixed header (ident/version/numframes, i.e. dsprite_t minus its
	// frames[1]) must be present before we read those fields
	if (modfilelen < (int)(sizeof(dsprite_t) - sizeof(dsprframe_t)))
		ri.Sys_Error (ERR_DROP, "%s is not a valid sprite", mod->name);

	sprout->ident = LittleLong (sprin->ident);
	sprout->version = LittleLong (sprin->version);
	sprout->numframes = LittleLong (sprin->numframes);

	if (sprout->version != SPRITE_VERSION)
		ri.Sys_Error (ERR_DROP, "%s has wrong version number (%i should be %i)",
				 mod->name, sprout->version, SPRITE_VERSION);

	if (sprout->numframes < 0 || sprout->numframes > MAX_MD2SKINS)
		ri.Sys_Error (ERR_DROP, "%s has too many frames (%i > %i)",
				 mod->name, sprout->numframes, MAX_MD2SKINS);

	// the numframes frame records must lie within the file we loaded
	if ((long long)(sizeof(dsprite_t) - sizeof(dsprframe_t))
			+ (long long)sprout->numframes * sizeof(dsprframe_t) > modfilelen)
		ri.Sys_Error (ERR_DROP, "%s is too small for %i frames",
				 mod->name, sprout->numframes);

	// byte swap everything
	for (i=0 ; i<sprout->numframes ; i++)
	{
		sprout->frames[i].width = LittleLong (sprin->frames[i].width);
		sprout->frames[i].height = LittleLong (sprin->frames[i].height);
		sprout->frames[i].origin_x = LittleLong (sprin->frames[i].origin_x);
		sprout->frames[i].origin_y = LittleLong (sprin->frames[i].origin_y);
		memcpy (sprout->frames[i].name, sprin->frames[i].name, MAX_SKINNAME);
		mod->skins[i] = GL3_FindImage (sprout->frames[i].name, it_sprite);
	}

	mod->type = mod_sprite;
}

//=============================================================================

/*
=================
GL3_BuildPolygonFromSurface

Ported from gl_rsurf.c. Reconstructs each surface's polygon from the model's
surfedges/edges/vertexes, computing diffuse and lightmap texture coordinates.
=================
*/
void GL3_BuildPolygonFromSurface (msurface_t *fa)
{
	int			i, lindex, lnumverts;
	medge_t		*pedges, *r_pedge;
	float		*vec;
	float		s, t;
	glpoly_t	*poly;
	vec3_t		total;

// reconstruct the polygon
	pedges = loadmodel->edges;
	lnumverts = fa->numedges;

	VectorClear (total);
	//
	// draw texture
	//
	// glpoly_t already has room for 4 verts; only add storage beyond that.
	// (The original subtracted 4 unconditionally, which underflows size_t on
	// 64-bit for triangle faces — the old 32-bit Hunk_Alloc(int) hid it.)
	{
		int extra = (lnumverts > 4) ? (lnumverts - 4) : 0;
		poly = malloc (sizeof(glpoly_t) + (size_t)extra * VERTEXSIZE * sizeof(float));
	}
	if (!poly)
		ri.Sys_Error (ERR_FATAL, "GL3_BuildPolygonFromSurface: out of memory");
	poly->next = fa->polys;
	poly->flags = fa->flags;
	fa->polys = poly;
	poly->numverts = lnumverts;

	for (i=0 ; i<lnumverts ; i++)
	{
		lindex = loadmodel->surfedges[fa->firstedge + i];

		if (lindex > 0)
		{
			r_pedge = &pedges[lindex];
			vec = loadmodel->vertexes[r_pedge->v[0]].position;
		}
		else
		{
			r_pedge = &pedges[-lindex];
			vec = loadmodel->vertexes[r_pedge->v[1]].position;
		}
		s = DotProduct (vec, fa->texinfo->vecs[0]) + fa->texinfo->vecs[0][3];
		t = DotProduct (vec, fa->texinfo->vecs[1]) + fa->texinfo->vecs[1][3];

		// turb (water) surfaces keep RAW texcoords -- the warp shader divides
		// by 64 and adds the sin distortion at draw time
		if (!(fa->flags & SURF_DRAWTURB))
		{
			s /= fa->texinfo->image->width;
			t /= fa->texinfo->image->height;
		}

		VectorAdd (total, vec, total);
		VectorCopy (vec, poly->verts[i]);
		poly->verts[i][3] = s;
		poly->verts[i][4] = t;

		//
		// lightmap texture coordinates
		//
		s = DotProduct (vec, fa->texinfo->vecs[0]) + fa->texinfo->vecs[0][3];
		s -= fa->texturemins[0];
		s += fa->light_s*16;
		s += 8;
		s /= BLOCK_WIDTH*16; //fa->texinfo->texture->width;

		t = DotProduct (vec, fa->texinfo->vecs[1]) + fa->texinfo->vecs[1][3];
		t -= fa->texturemins[1];
		t += fa->light_t*16;
		t += 8;
		t /= BLOCK_HEIGHT*16; //fa->texinfo->texture->height;

		poly->verts[i][5] = s;
		poly->verts[i][6] = t;
	}

	poly->numverts = lnumverts;
}

//=============================================================================

/*
@@@@@@@@@@@@@@@@@@@@@
GL3_BeginRegistration

Specifies the model that will be used as the world
@@@@@@@@@@@@@@@@@@@@@
*/
void GL3_BeginRegistration (char *model)
{
	char	fullname[MAX_QPATH];
	cvar_t	*flushmap;

	registration_sequence++;
	GL3_DecalsNewMap ();	// impact marks belong to the outgoing world

	Com_sprintf (fullname, sizeof(fullname), "maps/%s.bsp", model);

	// explicitly free the old map if different
	// this guarantees that mod_known[0] is the world map
	flushmap = ri.Cvar_Get ("flushmap", "0", 0);
	if ( strcmp(mod_known[0].name, fullname) || flushmap->value)
		Mod_Free (&mod_known[0]);

	// GL3_Mod_ForName loads the brush model, and the surface loader
	// (Mod_LoadFaces) brackets its work with GL3_BeginBuildingLightmaps /
	// GL3_EndBuildingLightmaps -- matching the original's ordering, where
	// lightmap building is driven from inside the loader rather than here.
	r_worldmodel = GL3_Mod_ForName (fullname, true);
}


/*
@@@@@@@@@@@@@@@@@@@@@
GL3_RegisterModel

@@@@@@@@@@@@@@@@@@@@@
*/
struct model_s *GL3_RegisterModel (char *name)
{
	model_t	*mod;
	int		i;
	dsprite_t	*sprout;
	dmdl_t		*pheader;

	mod = GL3_Mod_ForName (name, false);
	if (mod)
	{
		mod->registration_sequence = registration_sequence;

		// register any images used by the models
		if (mod->type == mod_sprite)
		{
			sprout = (dsprite_t *)mod->extradata;
			for (i=0 ; i<sprout->numframes ; i++)
				mod->skins[i] = GL3_FindImage (sprout->frames[i].name, it_sprite);
		}
		else if (mod->type == mod_alias)
		{
			pheader = (dmdl_t *)mod->extradata;
			for (i=0 ; i<pheader->num_skins ; i++)
				mod->skins[i] = GL3_RegisterSkin ((char *)pheader + pheader->ofs_skins + i*MAX_SKINNAME);
//PGM
			mod->numframes = pheader->num_frames;
//PGM
		}
		else if (mod->type == mod_brush)
		{
			for (i=0 ; i<mod->numtexinfo ; i++)
				mod->texinfo[i].image->registration_sequence = registration_sequence;
		}
	}
	return mod;
}


/*
@@@@@@@@@@@@@@@@@@@@@
GL3_RegisterSkin

@@@@@@@@@@@@@@@@@@@@@
*/
struct image_s *GL3_RegisterSkin (char *name)
{
	return GL3_FindImage (name, it_skin);
}


/*
@@@@@@@@@@@@@@@@@@@@@
GL3_EndRegistration

@@@@@@@@@@@@@@@@@@@@@
*/
void GL3_EndRegistration (void)
{
	int		i;
	model_t	*mod;

	for (i=0, mod=mod_known ; i<mod_numknown ; i++, mod++)
	{
		if (!mod->name[0])
			continue;
		if (mod->registration_sequence != registration_sequence)
		{	// not needed by this map — but with the asset cache on,
			// alias/sprite models stay resident for later maps; stale
			// brush models (old worlds) are always freed
			if (!(cache_assets && cache_assets->value) || mod->type == mod_brush)
				Mod_Free (mod);
		}
	}

	GL3_FreeUnusedImages ();

	// upload all world geometry to the GPU now that registration is done
	GL3_BuildWorldVBO ();
}


//=============================================================================


/*
================
Mod_Free
================
*/
static void Mod_Free (model_t *mod)
{
	Hunk_Free (mod->extradata);
	memset (mod, 0, sizeof(*mod));
}

/*
================
GL3_Mod_FreeAll
================
*/
void GL3_Mod_FreeAll (void)
{
	int		i;

	for (i=0 ; i<mod_numknown ; i++)
	{
		if (mod_known[i].extradatasize)
			Mod_Free (&mod_known[i]);
	}
}
