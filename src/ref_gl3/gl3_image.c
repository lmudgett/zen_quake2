// gl3_image.c -- Quake 2 asset loading (PCX/TGA/WAL) and GL texture upload
// for the modern OpenGL 3.3 core renderer. High-resolution replacement
// textures (retexture packs) load via stb_image when a .png/.tga/.jpg
// exists at the asset's path.

#include <stdlib.h>
#include <string.h>

#include "gl3_local.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_TGA
#define STBI_NO_STDIO
// stb's default cap is 1<<24 per side; a hostile override texture could
// still demand a multi-GB decode. No real texture pack exceeds GL limits.
#define STBI_MAX_DIMENSIONS 16384
// zero-init stb allocations: closes CVE-2023-45663 (truncated TGA leaks
// uninitialized heap into pixels; unfixed upstream in v2.30) without
// modifying the pristine vendored header
#define STBI_MALLOC(sz)        calloc(1, (sz))
#define STBI_REALLOC(p, newsz) realloc((p), (newsz))
#define STBI_FREE(p)           free(p)
#include "stb_image.h"

unsigned	d_8to24table[256];
image_t		gl3textures[MAX_GLTEXTURES];
int			numgl3textures;
int			registration_sequence;

static void GL3_PurgeOldImages (void);

// pcx_t and miptex_t come from qfiles.h (via the qcommon.h include chain).

typedef struct
{
	unsigned char	id_length, colormap_type, image_type;
	unsigned short	colormap_index, colormap_length;
	unsigned char	colormap_size;
	unsigned short	x_origin, y_origin, width, height;
	unsigned char	pixel_size, attributes;
} TargaHeader;

/*
=================================================================

PCX LOADING

=================================================================
*/

/*
==============
LoadPCX
==============
*/
static void LoadPCX (char *filename, byte **pic, byte **palette, int *width, int *height)
{
	byte	*raw;
	pcx_t	*pcx;
	int		x, y;
	int		len;
	int		dataByte, runLength;
	byte	*out, *pix;

	*pic = NULL;
	if (palette)
		*palette = NULL;

	// load the file
	len = ri.FS_LoadFile (filename, (void **)&raw);
	if (!raw)
	{
		ri.Con_Printf (PRINT_DEVELOPER, "Bad pcx file %s\n", filename);
		return;
	}

	// parse the PCX file
	pcx = (pcx_t *)raw;

	pcx->xmin = LittleShort (pcx->xmin);
	pcx->ymin = LittleShort (pcx->ymin);
	pcx->xmax = LittleShort (pcx->xmax);
	pcx->ymax = LittleShort (pcx->ymax);
	pcx->hres = LittleShort (pcx->hres);
	pcx->vres = LittleShort (pcx->vres);
	pcx->bytes_per_line = LittleShort (pcx->bytes_per_line);
	pcx->palette_type = LittleShort (pcx->palette_type);

	raw = &pcx->data;

	if (pcx->manufacturer != 0x0a
		|| pcx->version != 5
		|| pcx->encoding != 1
		|| pcx->bits_per_pixel != 8
		|| pcx->xmax >= 640
		|| pcx->ymax >= 480)
	{
		ri.Con_Printf (PRINT_ALL, "Bad pcx file %s\n", filename);
		ri.FS_FreeFile (pcx);
		return;
	}

	out = malloc ((pcx->ymax + 1) * (pcx->xmax + 1));

	*pic = out;

	pix = out;

	if (palette)
	{
		*palette = malloc (768);
		memcpy (*palette, (byte *)pcx + len - 768, 768);
	}

	if (width)
		*width = pcx->xmax + 1;
	if (height)
		*height = pcx->ymax + 1;

	for (y = 0; y <= pcx->ymax; y++, pix += pcx->xmax + 1)
	{
		for (x = 0; x <= pcx->xmax; )
		{
			dataByte = *raw++;

			if ((dataByte & 0xC0) == 0xC0)
			{
				runLength = dataByte & 0x3F;
				dataByte = *raw++;
			}
			else
				runLength = 1;

			while (runLength-- > 0)
				pix[x++] = dataByte;
		}
	}

	if (raw - (byte *)pcx > len)
	{
		ri.Con_Printf (PRINT_DEVELOPER, "PCX file %s was malformed", filename);
		free (*pic);
		*pic = NULL;
	}

	ri.FS_FreeFile (pcx);
}

/*
=========================================================

TARGA LOADING

=========================================================
*/

/*
=============
LoadTGA
=============
*/
static void LoadTGA (char *name, byte **pic, int *width, int *height)
{
	int			columns, rows, numPixels;
	byte		*pixbuf;
	int			row, column;
	byte		*buf_p;
	byte		*buffer;
	TargaHeader	targa_header;
	byte		*targa_rgba;
	byte		tmp[2];

	*pic = NULL;

	// load the file
	ri.FS_LoadFile (name, (void **)&buffer);
	if (!buffer)
	{
		ri.Con_Printf (PRINT_DEVELOPER, "Bad tga file %s\n", name);
		return;
	}

	buf_p = buffer;

	targa_header.id_length = *buf_p++;
	targa_header.colormap_type = *buf_p++;
	targa_header.image_type = *buf_p++;

	tmp[0] = buf_p[0];
	tmp[1] = buf_p[1];
	targa_header.colormap_index = LittleShort (*((short *)tmp));
	buf_p += 2;
	tmp[0] = buf_p[0];
	tmp[1] = buf_p[1];
	targa_header.colormap_length = LittleShort (*((short *)tmp));
	buf_p += 2;
	targa_header.colormap_size = *buf_p++;
	targa_header.x_origin = LittleShort (*((short *)buf_p));
	buf_p += 2;
	targa_header.y_origin = LittleShort (*((short *)buf_p));
	buf_p += 2;
	targa_header.width = LittleShort (*((short *)buf_p));
	buf_p += 2;
	targa_header.height = LittleShort (*((short *)buf_p));
	buf_p += 2;
	targa_header.pixel_size = *buf_p++;
	targa_header.attributes = *buf_p++;

	if (targa_header.image_type != 2
		&& targa_header.image_type != 10)
		ri.Sys_Error (ERR_DROP, "LoadTGA: Only type 2 and 10 targa RGB images supported\n");

	if (targa_header.colormap_type != 0
		|| (targa_header.pixel_size != 32 && targa_header.pixel_size != 24))
		ri.Sys_Error (ERR_DROP, "LoadTGA: Only 32 or 24 bit images supported (no colormaps)\n");

	columns = targa_header.width;
	rows = targa_header.height;
	numPixels = columns * rows;

	if (width)
		*width = columns;
	if (height)
		*height = rows;

	targa_rgba = malloc (numPixels * 4);
	*pic = targa_rgba;

	if (targa_header.id_length != 0)
		buf_p += targa_header.id_length;	// skip TARGA image comment

	if (targa_header.image_type == 2)
	{	// Uncompressed, RGB images
		for (row = rows - 1; row >= 0; row--)
		{
			pixbuf = targa_rgba + row * columns * 4;
			for (column = 0; column < columns; column++)
			{
				unsigned char red, green, blue, alphabyte;
				switch (targa_header.pixel_size)
				{
				case 24:
					blue = *buf_p++;
					green = *buf_p++;
					red = *buf_p++;
					*pixbuf++ = red;
					*pixbuf++ = green;
					*pixbuf++ = blue;
					*pixbuf++ = 255;
					break;
				case 32:
					blue = *buf_p++;
					green = *buf_p++;
					red = *buf_p++;
					alphabyte = *buf_p++;
					*pixbuf++ = red;
					*pixbuf++ = green;
					*pixbuf++ = blue;
					*pixbuf++ = alphabyte;
					break;
				}
			}
		}
	}
	else if (targa_header.image_type == 10)
	{	// Runlength encoded RGB images
		unsigned char red, green, blue, alphabyte, packetHeader, packetSize, j;
		red = green = blue = alphabyte = 0;
		for (row = rows - 1; row >= 0; row--)
		{
			pixbuf = targa_rgba + row * columns * 4;
			for (column = 0; column < columns; )
			{
				packetHeader = *buf_p++;
				packetSize = 1 + (packetHeader & 0x7f);
				if (packetHeader & 0x80)
				{	// run-length packet
					switch (targa_header.pixel_size)
					{
					case 24:
						blue = *buf_p++;
						green = *buf_p++;
						red = *buf_p++;
						alphabyte = 255;
						break;
					case 32:
						blue = *buf_p++;
						green = *buf_p++;
						red = *buf_p++;
						alphabyte = *buf_p++;
						break;
					}

					for (j = 0; j < packetSize; j++)
					{
						*pixbuf++ = red;
						*pixbuf++ = green;
						*pixbuf++ = blue;
						*pixbuf++ = alphabyte;
						column++;
						if (column == columns)
						{	// run spans across rows
							column = 0;
							if (row > 0)
								row--;
							else
								goto breakOut;
							pixbuf = targa_rgba + row * columns * 4;
						}
					}
				}
				else
				{	// non run-length packet
					for (j = 0; j < packetSize; j++)
					{
						switch (targa_header.pixel_size)
						{
						case 24:
							blue = *buf_p++;
							green = *buf_p++;
							red = *buf_p++;
							*pixbuf++ = red;
							*pixbuf++ = green;
							*pixbuf++ = blue;
							*pixbuf++ = 255;
							break;
						case 32:
							blue = *buf_p++;
							green = *buf_p++;
							red = *buf_p++;
							alphabyte = *buf_p++;
							*pixbuf++ = red;
							*pixbuf++ = green;
							*pixbuf++ = blue;
							*pixbuf++ = alphabyte;
							break;
						}
						column++;
						if (column == columns)
						{	// pixel packet run spans across rows
							column = 0;
							if (row > 0)
								row--;
							else
								goto breakOut;
							pixbuf = targa_rgba + row * columns * 4;
						}
					}
				}
			}
		breakOut:;
		}
	}

	ri.FS_FreeFile (buffer);
}

/*
=================================================================

TEXTURE BINDING / UPLOAD

=================================================================
*/

/*
================
GL3_Bind

Binds a GL texture object, caching the currently bound one so
redundant glBindTexture calls are avoided.
================
*/
void GL3_Bind (GLuint texnum)
{
	if (gl3state.currenttexture == (int)texnum)
		return;
	gl3state.currenttexture = (int)texnum;
	glBindTexture (GL_TEXTURE_2D, texnum);
}

/*
================
GL3_LoadPic

Uploads an 8-bit indexed (bits==8) or already-RGBA (bits==32) image
as a GL_RGBA8 texture and registers it in gl3textures[].
================
*/
/*
=================================================================

NORMAL MAPS (bump under dynamic lights)

=================================================================
*/

GLuint	gl3_flat_normal;	// 1x1 "no bump" normal map

static GLuint GL3_UploadNormalMap (const byte *nm, int w, int h)
{
	GLuint	tex;

	glGenTextures (1, &tex);
	glBindTexture (GL_TEXTURE_2D, tex);
	glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nm);
	glGenerateMipmap (GL_TEXTURE_2D);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	gl3state.currenttexture = -1;
	return tex;
}

// derive a tangent-space normal map from the diffuse: luminance as a height
// field, 3x3 Sobel gradients, tileable (wrapping) sampling
static GLuint GL3_GenNormalMap (const unsigned *rgba, int w, int h)
{
	byte	*nm = malloc (w * h * 4);
	float	*lum = malloc (w * h * sizeof(float));
	float	strength = 2.5f;
	GLuint	tex;
	int		x, y;

	if (!nm || !lum)
	{
		free (nm); free (lum);
		return 0;
	}

	for (y = 0; y < w * h; y++)
	{
		const byte *p = (const byte *)&rgba[y];
		lum[y] = (p[0] * 0.299f + p[1] * 0.587f + p[2] * 0.114f) / 255.0f;
	}

#define LUM(px, py) lum[(((py) + h) % h) * w + (((px) + w) % w)]
	for (y = 0; y < h; y++)
	{
		for (x = 0; x < w; x++)
		{
			float gx = (LUM(x-1,y-1) + 2*LUM(x-1,y) + LUM(x-1,y+1))
					 - (LUM(x+1,y-1) + 2*LUM(x+1,y) + LUM(x+1,y+1));
			float gy = (LUM(x-1,y-1) + 2*LUM(x,y-1) + LUM(x+1,y-1))
					 - (LUM(x-1,y+1) + 2*LUM(x,y+1) + LUM(x+1,y+1));
			float nx = gx * strength, ny = gy * strength, nz = 1.0f;
			float ilen = 1.0f / sqrtf (nx*nx + ny*ny + nz*nz);
			byte *o = nm + (y * w + x) * 4;
			o[0] = (byte)((nx * ilen * 0.5f + 0.5f) * 255.0f);
			o[1] = (byte)((ny * ilen * 0.5f + 0.5f) * 255.0f);
			o[2] = (byte)((nz * ilen * 0.5f + 0.5f) * 255.0f);
			o[3] = 255;
		}
	}
#undef LUM

	tex = GL3_UploadNormalMap (nm, w, h);
	free (nm);
	free (lum);
	return tex;
}

// look for a pack-supplied <name>_n.png/tga/jpg normal map
static GLuint GL3_LoadNormalOverride (const char *name)
{
	static const char *exts[] = { ".png", ".tga", ".jpg" };
	char	trial[MAX_QPATH + 8];
	byte	*raw;
	int		rawlen, i, w, h, comp;
	size_t	base = strlen (name) - 4;

	for (i = 0; i < 3; i++)
	{
		memcpy (trial, name, base);
		strcpy (trial + base, "_n");
		strcpy (trial + base + 2, exts[i]);

		rawlen = ri.FS_LoadFile (trial, (void **)&raw);
		if (!raw)
			continue;

		stbi_uc *pixels = stbi_load_from_memory (raw, rawlen, &w, &h, &comp, 4);
		ri.FS_FreeFile (raw);
		if (!pixels)
			continue;

		GLuint tex = GL3_UploadNormalMap (pixels, w, h);
		stbi_image_free (pixels);
		return tex;
	}
	return 0;
}

/*
=================
R_FloodFillSkin

Fill background pixels so mipmapping doesn't have haloes
=================
*/

typedef struct
{
	short		x, y;
} floodfill_t;

// must be a power of 2
#define FLOODFILL_FIFO_SIZE 0x1000
#define FLOODFILL_FIFO_MASK (FLOODFILL_FIFO_SIZE - 1)

#define FLOODFILL_STEP( off, dx, dy ) \
{ \
	if (pos[off] == fillcolor) \
	{ \
		pos[off] = 255; \
		fifo[inpt].x = x + (dx), fifo[inpt].y = y + (dy); \
		inpt = (inpt + 1) & FLOODFILL_FIFO_MASK; \
	} \
	else if (pos[off] != 255) fdc = pos[off]; \
}

static void R_FloodFillSkin (byte *skin, int skinwidth, int skinheight)
{
	byte				fillcolor = *skin; // assume this is the pixel to fill
	static floodfill_t	fifo[FLOODFILL_FIFO_SIZE];
	int					inpt = 0, outpt = 0;
	int					filledcolor = -1;
	int					i;

	if (filledcolor == -1)
	{
		filledcolor = 0;
		// attempt to find opaque black
		for (i = 0; i < 256; ++i)
			if (d_8to24table[i] == (255 << 0)) // alpha 1.0
			{
				filledcolor = i;
				break;
			}
	}

	// can't fill to filled color or to transparent color (used as visited marker)
	if ((fillcolor == filledcolor) || (fillcolor == 255))
		return;

	fifo[inpt].x = 0, fifo[inpt].y = 0;
	inpt = (inpt + 1) & FLOODFILL_FIFO_MASK;

	while (outpt != inpt)
	{
		int			x = fifo[outpt].x, y = fifo[outpt].y;
		int			fdc = filledcolor;
		byte		*pos = &skin[x + skinwidth * y];

		outpt = (outpt + 1) & FLOODFILL_FIFO_MASK;

		if (x > 0)				FLOODFILL_STEP( -1, -1, 0 );
		if (x < skinwidth - 1)	FLOODFILL_STEP( 1, 1, 0 );
		if (y > 0)				FLOODFILL_STEP( -skinwidth, 0, -1 );
		if (y < skinheight - 1)	FLOODFILL_STEP( skinwidth, 0, 1 );
		skin[x + skinwidth * y] = fdc;
	}
}

image_t *GL3_LoadPic (char *name, byte *pic, int width, int height, imagetype_t type, int bits)
{
	image_t		*image;
	int			i;
	int			s;
	unsigned	*rgba;

	// find a free image_t slot
	for (i = 0, image = gl3textures; i < numgl3textures; i++, image++)
	{
		if (image->name[0] == 0)
			break;
	}
	if (i == numgl3textures)
	{
		if (numgl3textures == MAX_GLTEXTURES)
		{
			// asset cache kept old maps' textures around — evict the
			// stale ones and retry
			GL3_PurgeOldImages ();
			for (i = 0, image = gl3textures; i < numgl3textures; i++, image++)
			{
				if (image->name[0] == 0)
					break;
			}
			if (i == numgl3textures)
				ri.Sys_Error (ERR_DROP, "MAX_GLTEXTURES");
		}
		else
			numgl3textures++;
	}
	image = &gl3textures[i];

	if (strlen (name) >= sizeof (image->name))
		ri.Sys_Error (ERR_DROP, "GL3_LoadPic: \"%s\" is too long", name);
	strcpy (image->name, name);
	image->registration_sequence = registration_sequence;

	image->width = width;
	image->height = height;
	image->type = type;
	image->has_alpha = false;
	image->texturechain = NULL;

	s = width * height;
	rgba = malloc (s * 4);
	if (!rgba)
		ri.Sys_Error (ERR_DROP, "GL3_LoadPic: out of memory (%dx%d %s)", width, height, name);

	if (bits == 8)
	{
		// skins: fill the background color so mipmap averaging doesn't halo
		if (type == it_skin)
			R_FloodFillSkin (pic, width, height);

		// expand each palette index into RGBA
		for (i = 0; i < s; i++)
		{
			int p = pic[i];
			rgba[i] = d_8to24table[p];
			if (p == 255)
			{	// transparent, so scan around for another color
				// to avoid alpha fringes when filtering/mipmapping
				image->has_alpha = true;
				if (i > width && pic[i - width] != 255)
					p = pic[i - width];
				else if (i < s - width && pic[i + width] != 255)
					p = pic[i + width];
				else if (i > 0 && pic[i - 1] != 255)
					p = pic[i - 1];
				else if (i < s - 1 && pic[i + 1] != 255)
					p = pic[i + 1];
				else
					p = 0;
				// copy rgb components, keep alpha 0
				((byte *)&rgba[i])[0] = ((byte *)&d_8to24table[p])[0];
				((byte *)&rgba[i])[1] = ((byte *)&d_8to24table[p])[1];
				((byte *)&rgba[i])[2] = ((byte *)&d_8to24table[p])[2];
			}
		}
	}
	else
	{
		// already RGBA; detect any partial transparency
		memcpy (rgba, pic, s * 4);
		for (i = 0; i < s; i++)
		{
			if (((byte *)&rgba[i])[3] != 255)
			{
				image->has_alpha = true;
				break;
			}
		}
	}

	// world textures get a normal map for bump under dynamic lights:
	// a pack-supplied <name>_n image, else generated from the diffuse
	image->normaltex = 0;
	if (type == it_wall && gl_bump && gl_bump->value >= 1 && name[0] != '*')
	{
		image->normaltex = GL3_LoadNormalOverride (name);
		if (!image->normaltex && gl_bump->value >= 2)
			image->normaltex = GL3_GenNormalMap (rgba, width, height);
	}

	glGenTextures (1, &image->texnum);
	GL3_Bind (image->texnum);

	glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0,
		GL_RGBA, GL_UNSIGNED_BYTE, rgba);

	if (type == it_pic)
	{
		// pics: no mipmaps, clamp, linear
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}
	else if (type == it_sky)
	{
		// skies: no mipmaps (id never mips them; minified mips defeat the
		// 1/512 seam inset) and clamped so face edges never wrap around
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}
	else
	{
		// world/model textures: mipmapped and repeated
		glGenerateMipmap (GL_TEXTURE_2D);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
		if (gl_anisotropy && gl_anisotropy->value > 1)
		{
			float	maxaniso;
			glGetFloatv (GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &maxaniso);
			glTexParameterf (GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT,
				gl_anisotropy->value < maxaniso ? gl_anisotropy->value : maxaniso);
		}
	}

	free (rgba);

	return image;
}

/*
================
GL3_UpdateAnisotropy

Re-apply the gl_anisotropy level to every mipmapped texture (live cvar
change, no vid_restart needed).
================
*/
void GL3_UpdateAnisotropy (void)
{
	int		i;
	float	maxaniso, level;
	image_t	*image;

	glGetFloatv (GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &maxaniso);
	level = gl_anisotropy ? gl_anisotropy->value : 1.0f;
	if (level < 1.0f) level = 1.0f;
	if (level > maxaniso) level = maxaniso;

	for (i = 0, image = gl3textures; i < numgl3textures; i++, image++)
	{
		if (!image->registration_sequence)
			continue;
		if (image->type == it_pic || image->type == it_sky)
			continue;	// unmipped

		glBindTexture (GL_TEXTURE_2D, image->texnum);
		glTexParameterf (GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, level);
	}
	gl3state.currenttexture = -1;
}

/*
================
GL3_PurgeOldImages

Frees every image not touched by this registration sequence,
regardless of the asset cache (pics persist for the session,
like the original).
================
*/
static void GL3_PurgeOldImages (void)
{
	int		i;
	image_t	*image;

	for (i = 0, image = gl3textures; i < numgl3textures; i++, image++)
	{
		if (image->registration_sequence == registration_sequence)
			continue;		// used this sequence
		if (!image->registration_sequence)
			continue;		// free slot
		if (image->type == it_pic)
			continue;		// pics (HUD/menu) live for the whole session

		glDeleteTextures (1, &image->texnum);
		if (image->normaltex)
			glDeleteTextures (1, &image->normaltex);
		if (gl3state.currenttexture == (int)image->texnum)
			gl3state.currenttexture = -1;
		memset (image, 0, sizeof(*image));
	}
}

/*
================
GL3_FreeUnusedImages

Any image that was not touched by this registration sequence
will be freed — unless the asset cache is keeping textures
resident across maps.
================
*/
void GL3_FreeUnusedImages (void)
{
	if (cache_assets && cache_assets->value)
		return;
	GL3_PurgeOldImages ();
}

/*
================
LoadWAL

Loads a Quake 2 .wal wall texture (8-bit indexed, level-0 mip only).
================
*/
static image_t *LoadWAL (char *name)
{
	miptex_t	*mt;
	int			width, height, ofs;
	image_t		*image;

	ri.FS_LoadFile (name, (void **)&mt);
	if (!mt)
	{
		ri.Con_Printf (PRINT_ALL, "GL3_FindImage: can't load %s\n", name);
		return NULL;
	}

	width = LittleLong (mt->width);
	height = LittleLong (mt->height);
	ofs = LittleLong (mt->offsets[0]);

	image = GL3_LoadPic (name, (byte *)mt + ofs, width, height, it_wall, 8);

	ri.FS_FreeFile ((void *)mt);

	return image;
}

/*
================
GL3_FindImage

Finds or loads the given image by name/extension.
================
*/
/*
================
GL3_TryOverride

Retexture-pack support: look for a high-resolution .png/.tga/.jpg at the
asset's own path (e.g. textures/e1u1/floor1.wal -> .../floor1.png). The
image keeps the ORIGINAL asset's logical size so world texcoords and 2D
layouts are unaffected -- only the pixels are sharper.
================
*/
static image_t *GL3_TryOverride (char *name, imagetype_t type, int orig_w, int orig_h)
{
	static const char *exts[] = { ".png", ".tga", ".jpg" };
	char		trial[MAX_QPATH];
	byte		*raw;
	int			rawlen, i, w, h, comp;
	stbi_uc		*pixels;
	image_t		*image;
	size_t		base;

	if (!gl_retexture || !gl_retexture->value)
		return NULL;

	base = strlen (name) - 4;		// callers guarantee a 4-char extension
	if (base + 5 > sizeof(trial))
		return NULL;

	for (i = 0; i < 3; i++)
	{
		memcpy (trial, name, base);
		strcpy (trial + base, exts[i]);

		rawlen = ri.FS_LoadFile (trial, (void **)&raw);
		if (!raw)
			continue;

		pixels = stbi_load_from_memory (raw, rawlen, &w, &h, &comp, 4);
		ri.FS_FreeFile (raw);
		if (!pixels)
		{
			ri.Con_Printf (PRINT_ALL, "GL3_TryOverride: bad image %s\n", trial);
			continue;
		}

		// register under the ORIGINAL name so lookups keep working
		image = GL3_LoadPic (name, pixels, w, h, type, 32);
		stbi_image_free (pixels);

		// preserve the original logical size for texcoords / HUD layout
		if (orig_w > 0 && orig_h > 0)
		{
			image->width = orig_w;
			image->height = orig_h;
		}
		return image;
	}
	return NULL;
}

image_t *GL3_FindImage (char *name, imagetype_t type)
{
	image_t	*image;
	int		i, len;
	byte	*pic, *palette;
	int		width, height;

	if (!name)
		return NULL;
	len = strlen (name);
	if (len < 5)
		return NULL;

	// look for it
	for (i = 0, image = gl3textures; i < numgl3textures; i++, image++)
	{
		if (!strcmp (name, image->name))
		{
			image->registration_sequence = registration_sequence;
			return image;
		}
	}

	// load the pic from disk
	pic = NULL;
	palette = NULL;
	image = NULL;

	if (!strcmp (name + len - 4, ".pcx"))
	{
		LoadPCX (name, &pic, &palette, &width, &height);
		if (pic)
			image = GL3_TryOverride (name, type, width, height);
		else
			image = GL3_TryOverride (name, type, 0, 0);
		if (!image)
		{
			if (!pic)
				return NULL;
			image = GL3_LoadPic (name, pic, width, height, type, 8);
		}
	}
	else if (!strcmp (name + len - 4, ".wal"))
	{
		// the .wal header supplies the logical size even when overridden
		miptex_t	*mt = NULL;
		int			orig_w = 0, orig_h = 0;

		if (ri.FS_LoadFile (name, (void **)&mt) > 0 && mt)
		{
			orig_w = LittleLong (mt->width);
			orig_h = LittleLong (mt->height);
			ri.FS_FreeFile (mt);
		}
		image = GL3_TryOverride (name, type, orig_w, orig_h);
		if (!image)
			image = LoadWAL (name);
	}
	else if (!strcmp (name + len - 4, ".tga"))
	{
		LoadTGA (name, &pic, &width, &height);
		if (!pic)
			return NULL;
		image = GL3_LoadPic (name, pic, width, height, type, 32);
	}
	else
		return NULL;

	if (pic)
		free (pic);
	if (palette)
		free (palette);

	return image;
}

/*
=================================================================

INIT / SHUTDOWN / LISTING

=================================================================
*/

/*
===============
GL3_InitImages

Loads the game palette and builds d_8to24table[].
===============
*/
void GL3_InitImages (void)
{
	int			i;
	int			r, g, b;
	unsigned	v;
	byte		*pic, *pal;
	int			width, height;

	registration_sequence = 1;

	{	// 1x1 "no bump" normal map: straight up in tangent space
		static const byte flat[4] = { 128, 128, 255, 255 };
		gl3_flat_normal = GL3_UploadNormalMap (flat, 1, 1);
	}

	LoadPCX ("pics/colormap.pcx", &pic, &pal, &width, &height);
	if (!pal)
		ri.Sys_Error (ERR_FATAL, "Couldn't load pics/colormap.pcx");

	for (i = 0; i < 256; i++)
	{
		r = pal[i * 3 + 0];
		g = pal[i * 3 + 1];
		b = pal[i * 3 + 2];

		v = (255 << 24) + (r << 0) + (g << 8) + (b << 16);
		d_8to24table[i] = LittleLong (v);
	}

	d_8to24table[255] &= LittleLong (0xffffff);		// 255 is transparent

	if (pic)
		free (pic);
	if (pal)
		free (pal);
}

/*
===============
GL3_ShutdownImages

Frees every uploaded GL texture and clears the registry.
===============
*/
void GL3_ShutdownImages (void)
{
	int		i;
	image_t	*image;

	for (i = 0, image = gl3textures; i < numgl3textures; i++, image++)
	{
		if (image->name[0] == 0)
			continue;		// free slot
		glDeleteTextures (1, &image->texnum);
		if (image->normaltex)
			glDeleteTextures (1, &image->normaltex);
	}

	if (gl3_flat_normal)
	{
		glDeleteTextures (1, &gl3_flat_normal);
		gl3_flat_normal = 0;
	}

	memset (gl3textures, 0, sizeof (gl3textures));
	numgl3textures = 0;
	gl3state.currenttexture = -1;
}

/*
===============
GL3_ImageList_f
===============
*/
void GL3_ImageList_f (void)
{
	int		i;
	image_t	*image;
	int		count = 0;
	int		texels = 0;

	ri.Con_Printf (PRINT_ALL, "------------------\n");

	for (i = 0, image = gl3textures; i < numgl3textures; i++, image++)
	{
		if (image->name[0] == 0)
			continue;		// free slot

		texels += image->width * image->height;
		count++;

		switch (image->type)
		{
		case it_skin:   ri.Con_Printf (PRINT_ALL, "M"); break;
		case it_sprite: ri.Con_Printf (PRINT_ALL, "S"); break;
		case it_wall:   ri.Con_Printf (PRINT_ALL, "W"); break;
		case it_pic:    ri.Con_Printf (PRINT_ALL, "P"); break;
		case it_sky:    ri.Con_Printf (PRINT_ALL, "Y"); break;
		default:        ri.Con_Printf (PRINT_ALL, " "); break;
		}

		ri.Con_Printf (PRINT_ALL, " %3i %3i %s: %s\n",
			image->width, image->height,
			image->has_alpha ? "RGBA" : "RGB ",
			image->name);
	}

	ri.Con_Printf (PRINT_ALL, "Total texel count (not counting mipmaps): %i\n", texels);
	ri.Con_Printf (PRINT_ALL, "%i images loaded\n", count);
}
