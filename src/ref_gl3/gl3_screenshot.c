// gl3_screenshot.c -- "screenshot" console command. Captures the back buffer
// at end-of-frame (after drawing, before swap) so the image is exactly what
// was rendered; front-buffer reads are unreliable under the Windows compositor.

#include "gl3_local.h"
#include <stdio.h>

static qboolean	screenshot_pending;

void GL3_ScreenShot_f (void)
{
	screenshot_pending = true;	// captured at the next GL3_EndFrame
}

// called from GL3_EndFrame after the frame is drawn, before the buffer swap
void GL3_ScreenShot_Capture (void)
{
	int		w, h, size, i;
	byte	*rgb, *tga;
	char	path[MAX_QPATH];
	FILE	*f;

	if (!screenshot_pending)
		return;
	screenshot_pending = false;

	w = gl3state.width;
	h = gl3state.height;
	size = w * h * 3;

	rgb = malloc (size);
	if (!rgb)
		return;

	glReadBuffer (GL_BACK);
	glPixelStorei (GL_PACK_ALIGNMENT, 1);
	glReadPixels (0, 0, w, h, GL_BGR, GL_UNSIGNED_BYTE, rgb);

	tga = malloc (size + 18);
	memset (tga, 0, 18);
	tga[2] = 2;					// uncompressed true-colour
	tga[12] = w & 255;   tga[13] = (w >> 8) & 255;
	tga[14] = h & 255;   tga[15] = (h >> 8) & 255;
	tga[16] = 24;
	memcpy (tga + 18, rgb, size);	// GL bottom-left origin == TGA default

	Com_sprintf (path, sizeof(path), "%s/screenshot.tga", ri.FS_Gamedir ());
	f = fopen (path, "wb");
	if (f)
	{
		fwrite (tga, 1, size + 18, f);
		fclose (f);
		ri.Con_Printf (PRINT_ALL, "Wrote %s (%dx%d)\n", path, w, h);
	}

	free (tga);
	free (rgb);
}
