// gl3_screenshot.c -- "screenshot" console command. Captures the back buffer
// at end-of-frame (after drawing, before swap) so the image is exactly what
// was rendered; front-buffer reads are unreliable under the Windows compositor.
// Written as PNG via the vendored stb_image_write (v1.16).

#include "gl3_local.h"
#include <stdio.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

static qboolean	screenshot_pending;

void GL3_ScreenShot_f (void)
{
	screenshot_pending = true;	// captured at the next GL3_EndFrame
}

// called from GL3_EndFrame after the frame is drawn, before the buffer swap
void GL3_ScreenShot_Capture (void)
{
	int		w, h;
	byte	*rgb;
	char	path[MAX_QPATH];

	if (!screenshot_pending)
		return;
	screenshot_pending = false;

	w = gl3state.width;
	h = gl3state.height;

	rgb = malloc (w * h * 3);
	if (!rgb)
		return;

	glReadBuffer (GL_BACK);
	glPixelStorei (GL_PACK_ALIGNMENT, 1);
	glReadPixels (0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, rgb);

	// numbered like id's quake00-99.tga so successive shots don't overwrite
	{
		int		n;
		FILE	*probe;

		for (n = 0; n < 1000; n++)
		{
			Com_sprintf (path, sizeof(path), "%s/screenshot%03d.png", ri.FS_Gamedir (), n);
			probe = fopen (path, "rb");
			if (!probe)
				break;
			fclose (probe);
		}
		if (n == 1000)
		{
			ri.Con_Printf (PRINT_ALL, "screenshot: all 1000 slots taken\n");
			free (rgb);
			return;
		}
	}

	stbi_flip_vertically_on_write (1);	// GL rows are bottom-up, PNG top-down
	if (stbi_write_png (path, w, h, 3, rgb, w * 3))
		ri.Con_Printf (PRINT_ALL, "Wrote %s (%dx%d)\n", path, w, h);

	free (rgb);
}
