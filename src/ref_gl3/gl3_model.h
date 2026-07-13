// gl3_model.h -- in-memory Quake 2 model structures (BSP brush models, MD2
// alias models, SP2 sprites). d*_t on-disk formats live in qfiles.h.

#ifndef GL3_MODEL_H
#define GL3_MODEL_H

// ----------------------------------------------------------------- brush models

typedef struct { vec3_t position; } mvertex_t;

typedef struct
{
	vec3_t		mins, maxs;
	vec3_t		origin;			// for sounds or lights
	float		radius;
	int			headnode;
	int			visleafs;		// not including the solid leaf 0
	int			firstface, numfaces;
} mmodel_t;

#define	SIDE_FRONT	0
#define	SIDE_BACK	1
#define	SIDE_ON		2

#define	SURF_PLANEBACK		2
#define	SURF_DRAWSKY		4
#define SURF_DRAWTURB		0x10
#define SURF_DRAWBACKGROUND	0x40
#define SURF_UNDERWATER		0x80

typedef struct
{
	unsigned short	v[2];
	unsigned int	cachededgeoffset;
} medge_t;

typedef struct mtexinfo_s
{
	float		vecs[2][4];
	int			flags;
	int			numframes;
	struct mtexinfo_s	*next;		// animation chain
	image_t		*image;
} mtexinfo_t;

#define	VERTEXSIZE	7		// x y z  s t  lm_s lm_t

typedef struct glpoly_s
{
	struct	glpoly_s	*next;
	struct	glpoly_s	*chain;
	int		numverts;
	int		flags;					// for SURF_UNDERWATER
	int		vbo_firstvert;			// base vertex in the world VBO (filled at upload)
	float	verts[4][VERTEXSIZE];	// variable-sized (allocated with numverts)
} glpoly_t;

typedef struct msurface_s
{
	int			visframe;		// in a PVS-visible leaf this frame
	int			drawframe;		// passed the node walk (facing + frustum): draw it

	cplane_t	*plane;
	int			flags;

	int			firstedge;		// look up in model->surfedges[], negative = backwards
	int			numedges;

	short		texturemins[2];
	short		extents[2];

	int			light_s, light_t;	// gl lightmap coordinates
	int			dlight_s, dlight_t;	// gl lightmap coords for dynamic lightmaps

	glpoly_t	*polys;				// multiple if warped
	struct	msurface_s	*texturechain;
	struct	msurface_s	*lightmapchain;

	mtexinfo_t	*texinfo;

	// lighting info
	int			dlightframe;
	int			dlightbits;

	int			lightmaptexturenum;
	byte		styles[MAXLIGHTMAPS];
	float		cached_light[MAXLIGHTMAPS];
	byte		*samples;			// [numstyles*surfsize]
} msurface_t;

typedef struct mnode_s
{
	int			contents;		// -1, to differentiate from leafs
	int			visframe;

	float		minmaxs[6];		// for bounding box culling

	struct mnode_s	*parent;

	cplane_t	*plane;
	struct mnode_s	*children[2];

	unsigned short	firstsurface;
	unsigned short	numsurfaces;
} mnode_t;

typedef struct mleaf_s
{
	int			contents;		// negative contents number
	int			visframe;

	float		minmaxs[6];

	struct mnode_s	*parent;

	int			cluster;
	int			area;

	msurface_t	**firstmarksurface;
	int			nummarksurfaces;
} mleaf_t;

// ----------------------------------------------------------------- whole model

typedef enum { mod_bad, mod_brush, mod_sprite, mod_alias } modtype_t;

typedef struct model_s
{
	char		name[MAX_QPATH];
	int			registration_sequence;

	modtype_t	type;
	int			numframes;
	int			flags;

	vec3_t		mins, maxs;
	float		radius;

	qboolean	clipbox;
	vec3_t		clipmins, clipmaxs;

	// brush model
	int			firstmodelsurface, nummodelsurfaces;
	int			lightmap;		// only for submodels

	int			numsubmodels;
	mmodel_t	*submodels;

	int			numplanes;
	cplane_t	*planes;

	int			numleafs;		// visible leafs, not counting 0
	mleaf_t		*leafs;

	int			numvertexes;
	mvertex_t	*vertexes;

	int			numedges;
	medge_t		*edges;

	int			numnodes;
	int			firstnode;
	mnode_t		*nodes;

	int			numtexinfo;
	mtexinfo_t	*texinfo;

	int			numsurfaces;
	msurface_t	*surfaces;

	int			numsurfedges;
	int			*surfedges;

	int			nummarksurfaces;
	msurface_t	**marksurfaces;

	dvis_t		*vis;
	byte		*lightdata;

	// alias model skins
	image_t		*skins[MAX_MD2SKINS];

	int			extradatasize;
	void		*extradata;
} model_t;

// ----------------------------------------------------------------- model API

void	 GL3_Mod_Init (void);
void	 GL3_Mod_FreeAll (void);
model_t *GL3_Mod_ForName (char *name, qboolean crash);
mleaf_t *GL3_Mod_PointInLeaf (vec3_t p, model_t *model);
byte	*GL3_Mod_ClusterPVS (int cluster, model_t *model);
void	 GL3_Mod_Modellist_f (void);

void	 GL3_BeginRegistration (char *model);
struct model_s *GL3_RegisterModel (char *name);
struct image_s *GL3_RegisterSkin (char *name);
void	 GL3_EndRegistration (void);

extern model_t	*r_worldmodel;

#endif // GL3_MODEL_H
