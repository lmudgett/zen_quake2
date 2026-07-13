// vid_sdl.c -- video mode / refresh-DLL management for the SDL3 client.
// Replaces win32/vid_dll.c. The window and GL context are owned by the
// renderer's GLimp layer (which links SDL too); this module only loads the
// renderer, wires the import table, and tracks video state.

#include <SDL3/SDL.h>

#include "../client/client.h"

// Structure containing functions exported from the refresh library
refexport_t	re;

cvar_t		*vid_gamma;
cvar_t		*vid_ref;			// name of refresh library to load
cvar_t		*vid_xpos;
cvar_t		*vid_ypos;
cvar_t		*vid_fullscreen;

viddef_t	viddef;				// global video state, used by other modules

static SDL_SharedObject	*reflib_library;
static qboolean			reflib_active;

/*
==========================================================================
DLL GLUE
==========================================================================
*/

#define	MAXPRINTMSG	4096
void VID_Printf (int print_level, char *fmt, ...)
{
	va_list	argptr;
	char	msg[MAXPRINTMSG];

	va_start (argptr, fmt);
	vsnprintf (msg, sizeof(msg), fmt, argptr);
	va_end (argptr);

	if (print_level == PRINT_ALL)
		Com_Printf ("%s", msg);
	else if (print_level == PRINT_DEVELOPER)
		Com_DPrintf ("%s", msg);
	else if (print_level == PRINT_ALERT)
		Com_Printf ("^1%s", msg);
}

void VID_Error (int err_level, char *fmt, ...)
{
	va_list	argptr;
	char	msg[MAXPRINTMSG];

	va_start (argptr, fmt);
	vsnprintf (msg, sizeof(msg), fmt, argptr);
	va_end (argptr);

	Com_Error (err_level, "%s", msg);
}

//==========================================================================

void VID_Restart_f (void)
{
	vid_ref->modified = true;
}

/*
** VID_GetModeInfo
*/
typedef struct vidmode_s
{
	const char *description;
	int         width, height;
	int         mode;
} vidmode_t;

static vidmode_t vid_modes[] =
{
	{ "Mode 0: 320x240",   320, 240,   0 },
	{ "Mode 1: 400x300",   400, 300,   1 },
	{ "Mode 2: 512x384",   512, 384,   2 },
	{ "Mode 3: 640x480",   640, 480,   3 },
	{ "Mode 4: 800x600",   800, 600,   4 },
	{ "Mode 5: 960x720",   960, 720,   5 },
	{ "Mode 6: 1024x768",  1024, 768,  6 },
	{ "Mode 7: 1152x864",  1152, 864,  7 },
	{ "Mode 8: 1280x960",  1280, 960,  8 },
	{ "Mode 9: 1600x1200", 1600, 1200, 9 },
	{ "Mode 10: 1920x1080", 1920, 1080, 10 },
	{ "Mode 11: 2560x1440", 2560, 1440, 11 },
};
#define VID_NUM_MODES ( sizeof( vid_modes ) / sizeof( vid_modes[0] ) )

qboolean VID_GetModeInfo (int *width, int *height, int mode)
{
	if (mode < 0 || mode >= (int)VID_NUM_MODES)
		return false;

	*width  = vid_modes[mode].width;
	*height = vid_modes[mode].height;

	return true;
}

/*
** VID_NewWindow -- the renderer calls this after (re)creating the window
*/
void VID_NewWindow (int width, int height)
{
	viddef.width  = width;
	viddef.height = height;

	cl.force_refdef = true;		// can't use a paused refdef
}

void VID_AppActivate (qboolean active)
{
	if (reflib_active)
		re.AppActivate (active);
	IN_Activate (active);
	S_Activate (active);
	CDAudio_Activate (active);
}

static void VID_FreeReflib (void)
{
	if (reflib_library)
		SDL_UnloadObject (reflib_library);
	memset (&re, 0, sizeof(re));
	reflib_library = NULL;
	reflib_active  = false;
}

/*
==============
VID_LoadRefresh
==============
*/
static qboolean VID_LoadRefresh (char *name)
{
	refimport_t	ri;
	GetRefAPI_t	GetRefAPI;

	if (reflib_active)
	{
		re.Shutdown ();
		VID_FreeReflib ();
	}

	Com_Printf ("------- Loading %s -------\n", name);

	if ((reflib_library = SDL_LoadObject (name)) == NULL)
	{
		Com_Printf ("SDL_LoadObject(\"%s\") failed: %s\n", name, SDL_GetError());
		return false;
	}

	ri.Cmd_AddCommand = Cmd_AddCommand;
	ri.Cmd_RemoveCommand = Cmd_RemoveCommand;
	ri.Cmd_Argc = Cmd_Argc;
	ri.Cmd_Argv = Cmd_Argv;
	ri.Cmd_ExecuteText = Cbuf_ExecuteText;
	ri.Con_Printf = VID_Printf;
	ri.Sys_Error = VID_Error;
	ri.FS_LoadFile = FS_LoadFile;
	ri.FS_FreeFile = FS_FreeFile;
	ri.FS_Gamedir = FS_Gamedir;
	ri.Cvar_Get = Cvar_Get;
	ri.Cvar_Set = Cvar_Set;
	ri.Cvar_SetValue = Cvar_SetValue;
	ri.Vid_GetModeInfo = VID_GetModeInfo;
	ri.Vid_MenuInit = VID_MenuInit;
	ri.Vid_NewWindow = VID_NewWindow;

	GetRefAPI = (GetRefAPI_t)SDL_LoadFunction (reflib_library, "GetRefAPI");
	if (!GetRefAPI)
		Com_Error (ERR_FATAL, "SDL_LoadFunction(GetRefAPI) failed on %s", name);

	re = GetRefAPI (ri);

	if (re.api_version != API_VERSION)
	{
		VID_FreeReflib ();
		Com_Error (ERR_FATAL, "%s has incompatible api_version", name);
	}

	if (re.Init (NULL, NULL) == -1)
	{
		re.Shutdown ();
		VID_FreeReflib ();
		return false;
	}

	Com_Printf ("------------------------------------\n");
	reflib_active = true;

	vidref_val = VIDREF_OTHER;
	if (vid_ref)
	{
		if (!strncmp (vid_ref->string, "gl", 2))	// "gl", "gl3"
			vidref_val = VIDREF_GL;
		else if (!strcmp (vid_ref->string, "soft"))
			vidref_val = VIDREF_SOFT;
	}

	return true;
}

/*
============
VID_CheckChanges
============
*/
void VID_CheckChanges (void)
{
	char	name[128];

	if (vid_ref->modified)
	{
		cl.force_refdef = true;		// can't use a paused refdef
		S_StopAllSounds ();
	}
	while (vid_ref->modified)
	{
		vid_ref->modified = false;
		vid_fullscreen->modified = true;
		cl.refresh_prepped = false;
		cls.disable_screen = true;

		Com_sprintf (name, sizeof(name), "ref_%s", vid_ref->string);
		if (!VID_LoadRefresh (name))
		{
			if (!strcmp (vid_ref->string, "gl3"))
				Com_Error (ERR_FATAL, "Couldn't load the default renderer ref_gl3!");
			Cvar_Set ("vid_ref", "gl3");

			// drop the console if we fail to load a refresh
			if (cls.key_dest != key_console)
				Con_ToggleConsole_f ();
		}
		cls.disable_screen = false;
	}
}

/*
============
VID_Init
============
*/
void VID_Init (void)
{
	vid_ref = Cvar_Get ("vid_ref", "gl3", CVAR_ARCHIVE);
	vid_xpos = Cvar_Get ("vid_xpos", "3", CVAR_ARCHIVE);
	vid_ypos = Cvar_Get ("vid_ypos", "22", CVAR_ARCHIVE);
	vid_fullscreen = Cvar_Get ("vid_fullscreen", "0", CVAR_ARCHIVE);
	vid_gamma = Cvar_Get ("vid_gamma", "1", CVAR_ARCHIVE);

	Cmd_AddCommand ("vid_restart", VID_Restart_f);

	// start the graphics mode and load the refresh library
	VID_CheckChanges ();
}

/*
============
VID_Shutdown
============
*/
void VID_Shutdown (void)
{
	if (reflib_active)
	{
		re.Shutdown ();
		VID_FreeReflib ();
	}
}
