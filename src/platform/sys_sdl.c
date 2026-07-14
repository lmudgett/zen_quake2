// sys_sdl.c -- SDL3 system services: timing, dynamic libraries, console,
// hunk allocation, directory scanning. Replaces win32/sys_win.c +
// win32/q_shwin.c and linux/sys_linux.c + linux/q_shlinux.c.

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "../common/qcommon.h"

#include <errno.h>
#include <stdio.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#include <io.h>
#include <conio.h>
#else
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <fnmatch.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/types.h>
#endif

int			curtime;
unsigned	sys_frame_time;

qboolean	stdin_active = true;

/*
===============================================================================

SYSTEM IO

===============================================================================
*/

void Sys_Error (char *error, ...)
{
	va_list		argptr;
	char		text[1024];

	CL_Shutdown ();
	Qcommon_Shutdown ();

	va_start (argptr, error);
	vsnprintf (text, sizeof(text), error, argptr);
	va_end (argptr);

	fprintf (stderr, "Sys_Error: %s\n", text);

	if (!dedicated || !dedicated->value)
		SDL_ShowSimpleMessageBox (SDL_MESSAGEBOX_ERROR, "Quake 2 Error", text, NULL);

	exit (1);
}

void Sys_Quit (void)
{
	CL_Shutdown ();
	Qcommon_Shutdown ();
	exit (0);
}

void Sys_Init (void)
{
}

void Sys_AppActivate (void)
{
}

void Sys_CopyProtect (void)
{
}

/*
================
Sys_ConsoleInput

Non-blocking line input from the dedicated server terminal
================
*/
static char	console_text[256];
static int	console_textlen;

char *Sys_ConsoleInput (void)
{
	if (!dedicated || !dedicated->value)
		return NULL;

#ifdef _WIN32
	// piped or file-redirected stdin (scripts, service wrappers) can't use
	// the conio console API — poll the handle and read whole lines instead
	{
		static DWORD	stdin_type = FILE_TYPE_UNKNOWN;
		HANDLE			hstdin = GetStdHandle (STD_INPUT_HANDLE);

		if (stdin_type == FILE_TYPE_UNKNOWN)
			stdin_type = GetFileType (hstdin);

		if (stdin_type != FILE_TYPE_CHAR)
		{
			DWORD	avail;
			size_t	len;

			if (!stdin_active)
				return NULL;

			if (stdin_type == FILE_TYPE_PIPE)
			{
				if (!PeekNamedPipe (hstdin, NULL, 0, NULL, &avail, NULL))
				{
					stdin_active = false;	// writer closed the pipe
					return NULL;
				}
				if (!avail)
					return NULL;
			}

			if (!fgets (console_text, sizeof(console_text), stdin))
			{
				stdin_active = false;		// eof
				return NULL;
			}
			len = strlen (console_text);
			while (len > 0 && (console_text[len-1] == '\n' || console_text[len-1] == '\r'))
				console_text[--len] = 0;
			return console_text;
		}
	}

	while (_kbhit ())
	{
		int ch = _getch ();

		switch (ch)
		{
		case '\r':
		case '\n':
			putchar ('\n');
			if (console_textlen)
			{
				console_text[console_textlen] = 0;
				console_textlen = 0;
				return console_text;
			}
			break;

		case '\b':
		case 127:
			if (console_textlen)
			{
				console_textlen--;
				fputs ("\b \b", stdout);
				fflush (stdout);
			}
			break;

		default:
			if (ch >= ' ' && console_textlen < (int)sizeof(console_text)-2)
			{
				putchar (ch);
				fflush (stdout);
				console_text[console_textlen++] = ch;
			}
			break;
		}
	}
	return NULL;
#else
	fd_set			fdset;
	struct timeval	timeout;
	int				len;

	if (!stdin_active)
		return NULL;

	FD_ZERO (&fdset);
	FD_SET (0, &fdset);	// stdin
	timeout.tv_sec = 0;
	timeout.tv_usec = 0;
	if (select (1, &fdset, NULL, NULL, &timeout) == -1 || !FD_ISSET(0, &fdset))
		return NULL;

	len = read (0, console_text, sizeof(console_text));
	if (len == 0)	// eof
	{
		stdin_active = false;
		return NULL;
	}
	if (len < 1)
		return NULL;
	console_text[len-1] = 0;	// rip off the \n and terminate

	return console_text;
#endif
}

/*
================
Sys_ConsoleOutput

Print text to the dedicated console
================
*/
void Sys_ConsoleOutput (char *string)
{
	if (!dedicated || !dedicated->value)
		return;

	fputs (string, stdout);
	fflush (stdout);
}

/*
================
Sys_SendKeyEvents

The SDL event pump lives in the client input layer (in_sdl.c);
for the dedicated server there are no events to send.
================
*/
void Sys_SendKeyEvents (void)
{
#ifndef DEDICATED_ONLY
	void IN_Update (void);
	IN_Update ();
#endif

	// grab frame time
	sys_frame_time = Sys_Milliseconds ();
}

char *Sys_GetClipboardData (void)
{
	char	*data = NULL;
	char	*cliptext;

	if (!SDL_WasInit (SDL_INIT_VIDEO))
		return NULL;

	cliptext = SDL_GetClipboardText ();
	if (cliptext && cliptext[0])
	{
		data = malloc (strlen(cliptext) + 1);
		if (data)
			strcpy (data, cliptext);
	}
	SDL_free (cliptext);

	return data;
}

/*
========================================================================

GAME DLL

========================================================================
*/

static SDL_SharedObject	*game_library;

#ifdef _WIN32
static const char *gamename = "game_x86_64.dll";
#elif defined(__APPLE__)
static const char *gamename = "game_x86_64.dylib";
#else
static const char *gamename = "game_x86_64.so";
#endif

void Sys_UnloadGame (void)
{
	if (game_library)
		SDL_UnloadObject (game_library);
	game_library = NULL;
}

/*
=================
Sys_GetGameAPI

Loads the game dll
=================
*/
void *Sys_GetGameAPI (void *parms)
{
	void	*(*GetGameAPI) (void *);
	char	name[MAX_OSPATH];
	char	*path;

	if (game_library)
		Com_Error (ERR_FATAL, "Sys_GetGameAPI without Sys_UnloadGame");

	// run through the filesystem search paths
	path = NULL;
	while (1)
	{
		path = FS_NextPath (path);
		if (!path)
			return NULL;		// couldn't find one anywhere
		Com_sprintf (name, sizeof(name), "%s/%s", path, gamename);
		game_library = SDL_LoadObject (name);
		if (game_library)
		{
			Com_DPrintf ("SDL_LoadObject (%s)\n", name);
			break;
		}
	}

	GetGameAPI = (void *(*)(void *))SDL_LoadFunction (game_library, "GetGameAPI");
	if (!GetGameAPI)
	{
		Sys_UnloadGame ();
		return NULL;
	}

	return GetGameAPI (parms);
}

/*
========================================================================

HUNK ALLOCATION

Reserve a large address range, commit/trim as the level data loads.

========================================================================
*/

static byte	*membase;
static int	maxhunksize;
static int	curhunksize;

#ifdef _WIN32

void *Hunk_Begin (int maxsize)
{
	curhunksize = 0;
	maxhunksize = maxsize;
	membase = VirtualAlloc (NULL, maxhunksize, MEM_RESERVE, PAGE_NOACCESS);
	if (!membase)
		Sys_Error ("Hunk_Begin: VirtualAlloc reserve failed");
	return (void *)membase;
}

void *Hunk_Alloc (int size)
{
	void	*buf;

	// round to cacheline
	size = (size+31)&~31;

	// commit pages as needed
	buf = VirtualAlloc (membase, curhunksize+size, MEM_COMMIT, PAGE_READWRITE);
	if (!buf)
		Sys_Error ("Hunk_Alloc: VirtualAlloc commit failed");
	curhunksize += size;
	if (curhunksize > maxhunksize)
		Sys_Error ("Hunk_Alloc overflow");

	return (void *)(membase+curhunksize-size);
}

int Hunk_End (void)
{
	return curhunksize;
}

void Hunk_Free (void *base)
{
	if (base)
		VirtualFree (base, 0, MEM_RELEASE);
}

#else	// POSIX: mmap the full range, trim the unused tail in Hunk_End

// a size header is stored at the start of the mapping so Hunk_Free
// knows how much to munmap after Hunk_End trimmed the region
#define HUNK_HEADER	32		// keep Hunk_Alloc's cacheline alignment

void *Hunk_Begin (int maxsize)
{
	maxhunksize = maxsize + HUNK_HEADER;
	curhunksize = 0;
	membase = mmap (0, maxhunksize, PROT_READ|PROT_WRITE,
		MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
	if (membase == NULL || membase == (byte *)-1)
		Sys_Error ("Hunk_Begin: unable to mmap %d bytes", maxsize);

	*((int *)membase) = curhunksize;

	return membase + HUNK_HEADER;
}

void *Hunk_Alloc (int size)
{
	byte	*buf;

	// round to cacheline
	size = (size+31)&~31;
	if (curhunksize + size > maxhunksize)
		Sys_Error ("Hunk_Alloc overflow");
	buf = membase + HUNK_HEADER + curhunksize;
	curhunksize += size;
	return buf;
}

int Hunk_End (void)
{
	long	pagesize;
	int		used, trim;

	pagesize = sysconf (_SC_PAGESIZE);
	used = (curhunksize + HUNK_HEADER + pagesize-1) & ~(pagesize-1);
	trim = maxhunksize - used;
	if (trim > 0)
		munmap (membase + used, trim);

	*((int *)membase) = used;

	return curhunksize;
}

void Hunk_Free (void *base)
{
	byte	*m;

	if (base)
	{
		m = ((byte *)base) - HUNK_HEADER;
		if (munmap (m, *((int *)m)))
			Sys_Error ("Hunk_Free: munmap failed (%d)", errno);
	}
}

#endif

/*
========================================================================

TIMING AND FILE SYSTEM SCANNING

========================================================================
*/

int Sys_Milliseconds (void)
{
	curtime = (int)SDL_GetTicks ();
	return curtime;
}

void Sys_Mkdir (char *path)
{
#ifdef _WIN32
	_mkdir (path);
#else
	mkdir (path, 0777);
#endif
}

static char	findbase[MAX_OSPATH];
static char	findpath[MAX_OSPATH];

#ifdef _WIN32

static intptr_t	findhandle = -1;

static qboolean CompareAttributes (unsigned found, unsigned musthave, unsigned canthave)
{
	if ( ( found & _A_RDONLY ) && ( canthave & SFF_RDONLY ) )
		return false;
	if ( ( found & _A_HIDDEN ) && ( canthave & SFF_HIDDEN ) )
		return false;
	if ( ( found & _A_SYSTEM ) && ( canthave & SFF_SYSTEM ) )
		return false;
	if ( ( found & _A_SUBDIR ) && ( canthave & SFF_SUBDIR ) )
		return false;
	if ( ( found & _A_ARCH ) && ( canthave & SFF_ARCH ) )
		return false;

	if ( ( musthave & SFF_RDONLY ) && !( found & _A_RDONLY ) )
		return false;
	if ( ( musthave & SFF_HIDDEN ) && !( found & _A_HIDDEN ) )
		return false;
	if ( ( musthave & SFF_SYSTEM ) && !( found & _A_SYSTEM ) )
		return false;
	if ( ( musthave & SFF_SUBDIR ) && !( found & _A_SUBDIR ) )
		return false;
	if ( ( musthave & SFF_ARCH ) && !( found & _A_ARCH ) )
		return false;

	return true;
}

char *Sys_FindFirst (char *path, unsigned musthave, unsigned canthave)
{
	struct _finddata_t findinfo;

	if (findhandle != -1)
		Sys_Error ("Sys_FindFirst without close");

	COM_FilePath (path, findbase);
	findhandle = _findfirst (path, &findinfo);
	if (findhandle == -1)
		return NULL;
	if ( !CompareAttributes( findinfo.attrib, musthave, canthave ) )
		return Sys_FindNext (musthave, canthave);
	Com_sprintf (findpath, sizeof(findpath), "%s/%s", findbase, findinfo.name);
	return findpath;
}

char *Sys_FindNext (unsigned musthave, unsigned canthave)
{
	struct _finddata_t findinfo;

	if (findhandle == -1)
		return NULL;
	while (_findnext (findhandle, &findinfo) != -1)
	{
		if ( CompareAttributes( findinfo.attrib, musthave, canthave ) )
		{
			Com_sprintf (findpath, sizeof(findpath), "%s/%s", findbase, findinfo.name);
			return findpath;
		}
	}
	return NULL;
}

void Sys_FindClose (void)
{
	if (findhandle != -1)
		_findclose (findhandle);
	findhandle = -1;
}

#else	// POSIX

static char	findpattern[MAX_OSPATH];
static DIR	*fdir;

static qboolean CompareAttributes (char *path, char *name,
	unsigned musthave, unsigned canthave)
{
	struct stat st;
	char fn[MAX_OSPATH];

	// . and .. never match
	if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
		return false;

	Com_sprintf (fn, sizeof(fn), "%s/%s", path, name);
	if (stat(fn, &st) == -1)
		return false;	// shouldn't happen

	if ( S_ISDIR(st.st_mode) && ( canthave & SFF_SUBDIR ) )
		return false;

	if ( ( musthave & SFF_SUBDIR ) && !S_ISDIR(st.st_mode) )
		return false;

	return true;
}

char *Sys_FindFirst (char *path, unsigned musthave, unsigned canthave)
{
	char *p;

	if (fdir)
		Sys_Error ("Sys_FindFirst without close");

	strncpy (findbase, path, sizeof(findbase)-1);
	findbase[sizeof(findbase)-1] = 0;

	if ((p = strrchr(findbase, '/')) != NULL)
	{
		*p = 0;
		strncpy (findpattern, p + 1, sizeof(findpattern)-1);
		findpattern[sizeof(findpattern)-1] = 0;
	}
	else
		Q_strlcpy (findpattern, "*", sizeof(findpattern));

	if (strcmp(findpattern, "*.*") == 0)
		Q_strlcpy (findpattern, "*", sizeof(findpattern));

	if ((fdir = opendir(findbase)) == NULL)
		return NULL;

	return Sys_FindNext (musthave, canthave);
}

char *Sys_FindNext (unsigned musthave, unsigned canthave)
{
	struct dirent *d;

	if (fdir == NULL)
		return NULL;
	while ((d = readdir(fdir)) != NULL)
	{
		if (!*findpattern || fnmatch(findpattern, d->d_name, 0) == 0)
		{
			if (CompareAttributes(findbase, d->d_name, musthave, canthave))
			{
				Com_sprintf (findpath, sizeof(findpath), "%s/%s", findbase, d->d_name);
				return findpath;
			}
		}
	}
	return NULL;
}

void Sys_FindClose (void)
{
	if (fdir != NULL)
		closedir (fdir);
	fdir = NULL;
}

#endif

//=============================================================================

int main (int argc, char **argv)
{
	int	time, oldtime, newtime;

	Qcommon_Init (argc, argv);

	oldtime = Sys_Milliseconds ();

	while (1)
	{
		// don't burn a core spinning; the server ticks at 10 Hz anyway
		if (dedicated && dedicated->value)
			SDL_Delay (1);

		do
		{
			newtime = Sys_Milliseconds ();
			time = newtime - oldtime;
		} while (time < 1);

		Qcommon_Frame (time);

		oldtime = newtime;
	}

	// never gets here
	return 0;
}
