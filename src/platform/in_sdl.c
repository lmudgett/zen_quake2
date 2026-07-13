// in_sdl.c -- SDL3 keyboard, mouse and event pump for the client.
// Replaces win32/in_win.c. Also drives window focus/close events, so it is
// the single place the SDL event queue is drained (via IN_Update, called
// from Sys_SendKeyEvents each frame).

#include <SDL3/SDL.h>

#include "../client/client.h"

static qboolean	mouse_active;		// relative mode engaged
static qboolean	input_active;		// window has focus and we own input
static int		mouse_dx, mouse_dy;

static cvar_t	*in_mouse;
cvar_t			*in_joystick;	// global: the controls menu externs this
static cvar_t	*m_filter;

// these mouse-look cvars/state live in the input module (as in the original)
static qboolean	mlooking;

extern cvar_t	*vid_fullscreen;
void VID_AppActivate (qboolean active);

static void IN_MLookDown (void) { mlooking = true; }
static void IN_MLookUp (void)
{
	mlooking = false;
	if (!freelook->value && lookspring->value)
		IN_CenterView ();
}

/*
=============
IN_TranslateKey

Map an SDL key event to a Quake K_* keynum. Scancode-based so the binding
layout is keyboard-position stable regardless of the OS keymap.
=============
*/
static int IN_TranslateKey (const SDL_KeyboardEvent *ev)
{
	// printable ASCII: use the (lowercased) virtual keycode so bindings and
	// the console see the expected character
	SDL_Keycode kc = ev->key;
	if (kc >= SDLK_SPACE && kc < 127)
	{
		if (kc >= 'A' && kc <= 'Z')
			kc += 'a' - 'A';
		return (int)kc;
	}

	switch (ev->scancode)
	{
	case SDL_SCANCODE_TAB:			return K_TAB;
	case SDL_SCANCODE_RETURN:		return K_ENTER;
	case SDL_SCANCODE_ESCAPE:		return K_ESCAPE;
	case SDL_SCANCODE_SPACE:		return K_SPACE;
	case SDL_SCANCODE_BACKSPACE:	return K_BACKSPACE;

	case SDL_SCANCODE_UP:			return K_UPARROW;
	case SDL_SCANCODE_DOWN:			return K_DOWNARROW;
	case SDL_SCANCODE_LEFT:			return K_LEFTARROW;
	case SDL_SCANCODE_RIGHT:		return K_RIGHTARROW;

	case SDL_SCANCODE_LALT:
	case SDL_SCANCODE_RALT:			return K_ALT;
	case SDL_SCANCODE_LCTRL:
	case SDL_SCANCODE_RCTRL:		return K_CTRL;
	case SDL_SCANCODE_LSHIFT:
	case SDL_SCANCODE_RSHIFT:		return K_SHIFT;

	case SDL_SCANCODE_F1:			return K_F1;
	case SDL_SCANCODE_F2:			return K_F2;
	case SDL_SCANCODE_F3:			return K_F3;
	case SDL_SCANCODE_F4:			return K_F4;
	case SDL_SCANCODE_F5:			return K_F5;
	case SDL_SCANCODE_F6:			return K_F6;
	case SDL_SCANCODE_F7:			return K_F7;
	case SDL_SCANCODE_F8:			return K_F8;
	case SDL_SCANCODE_F9:			return K_F9;
	case SDL_SCANCODE_F10:			return K_F10;
	case SDL_SCANCODE_F11:			return K_F11;
	case SDL_SCANCODE_F12:			return K_F12;

	case SDL_SCANCODE_INSERT:		return K_INS;
	case SDL_SCANCODE_DELETE:		return K_DEL;
	case SDL_SCANCODE_PAGEDOWN:		return K_PGDN;
	case SDL_SCANCODE_PAGEUP:		return K_PGUP;
	case SDL_SCANCODE_HOME:			return K_HOME;
	case SDL_SCANCODE_END:			return K_END;
	case SDL_SCANCODE_PAUSE:		return K_PAUSE;

	case SDL_SCANCODE_KP_7:			return K_KP_HOME;
	case SDL_SCANCODE_KP_8:			return K_KP_UPARROW;
	case SDL_SCANCODE_KP_9:			return K_KP_PGUP;
	case SDL_SCANCODE_KP_4:			return K_KP_LEFTARROW;
	case SDL_SCANCODE_KP_5:			return K_KP_5;
	case SDL_SCANCODE_KP_6:			return K_KP_RIGHTARROW;
	case SDL_SCANCODE_KP_1:			return K_KP_END;
	case SDL_SCANCODE_KP_2:			return K_KP_DOWNARROW;
	case SDL_SCANCODE_KP_3:			return K_KP_PGDN;
	case SDL_SCANCODE_KP_0:			return K_KP_INS;
	case SDL_SCANCODE_KP_PERIOD:	return K_KP_DEL;
	case SDL_SCANCODE_KP_DIVIDE:	return K_KP_SLASH;
	case SDL_SCANCODE_KP_MINUS:		return K_KP_MINUS;
	case SDL_SCANCODE_KP_PLUS:		return K_KP_PLUS;
	case SDL_SCANCODE_KP_ENTER:		return K_KP_ENTER;
	default:						return 0;
	}
}

static void IN_ActivateMouse (qboolean grab)
{
	SDL_Window *win = SDL_GL_GetCurrentWindow();
	if (!win)
		return;
	SDL_SetWindowRelativeMouseMode (win, grab);
	mouse_dx = mouse_dy = 0;
	mouse_active = grab;
}

/*
=============
IN_Update

Drain the SDL event queue: keyboard, mouse buttons/wheel/motion, window
focus and quit. Called once per frame from Sys_SendKeyEvents.
=============
*/
void IN_Update (void)
{
	SDL_Event	ev;
	unsigned	t = Sys_Milliseconds ();

	while (SDL_PollEvent (&ev))
	{
		switch (ev.type)
		{
		case SDL_EVENT_QUIT:
			Com_Quit ();
			break;

		case SDL_EVENT_KEY_DOWN:
		case SDL_EVENT_KEY_UP:
			// alt-enter toggles fullscreen
			if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.scancode == SDL_SCANCODE_RETURN
				&& (ev.key.mod & SDL_KMOD_ALT))
			{
				if (vid_fullscreen)
					Cvar_SetValue ("vid_fullscreen", !vid_fullscreen->value);
				break;
			}
			Key_Event (IN_TranslateKey (&ev.key), ev.type == SDL_EVENT_KEY_DOWN, t);
			break;

		case SDL_EVENT_MOUSE_BUTTON_DOWN:
		case SDL_EVENT_MOUSE_BUTTON_UP:
		{
			int key;
			switch (ev.button.button)
			{
			case SDL_BUTTON_LEFT:	key = K_MOUSE1; break;
			case SDL_BUTTON_RIGHT:	key = K_MOUSE2; break;
			case SDL_BUTTON_MIDDLE:	key = K_MOUSE3; break;
			case SDL_BUTTON_X1:		key = K_AUX1; break;
			case SDL_BUTTON_X2:		key = K_AUX2; break;
			default:				key = 0; break;
			}
			if (key)
				Key_Event (key, ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN, t);
			break;
		}

		case SDL_EVENT_MOUSE_WHEEL:
			if (ev.wheel.y > 0)
			{
				Key_Event (K_MWHEELUP, true, t);
				Key_Event (K_MWHEELUP, false, t);
			}
			else if (ev.wheel.y < 0)
			{
				Key_Event (K_MWHEELDOWN, true, t);
				Key_Event (K_MWHEELDOWN, false, t);
			}
			break;

		case SDL_EVENT_MOUSE_MOTION:
			if (mouse_active)
			{
				mouse_dx += (int)ev.motion.xrel;
				mouse_dy += (int)ev.motion.yrel;
			}
			break;

		case SDL_EVENT_WINDOW_FOCUS_GAINED:
			input_active = true;
			VID_AppActivate (true);
			break;
		case SDL_EVENT_WINDOW_FOCUS_LOST:
			input_active = false;
			VID_AppActivate (false);
			break;
		case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
			Com_Quit ();
			break;
		default:
			break;
		}
	}
}

//==========================================================================

void IN_Init (void)
{
	in_mouse = Cvar_Get ("in_mouse", "1", CVAR_ARCHIVE);
	in_joystick = Cvar_Get ("in_joystick", "0", CVAR_ARCHIVE);
	m_filter = Cvar_Get ("m_filter", "0", 0);

	Cmd_AddCommand ("+mlook", IN_MLookDown);
	Cmd_AddCommand ("-mlook", IN_MLookUp);

	SDL_InitSubSystem (SDL_INIT_EVENTS);
	input_active = true;
	Com_Printf ("Input initialized (SDL3).\n");
}

void IN_Shutdown (void)
{
	IN_ActivateMouse (false);
}

void IN_Activate (qboolean active)
{
	input_active = active;
}

/*
=============
IN_Frame

Engage relative mouse only when the game wants it: focused, mouse enabled,
and not sitting in a menu/console where the OS cursor should be free.
=============
*/
void IN_Frame (void)
{
	qboolean want;

	want = input_active && in_mouse->value &&
		!(cls.key_dest == key_console || cls.key_dest == key_menu);

	if (want != mouse_active)
		IN_ActivateMouse (want);
}

void IN_Commands (void)
{
}

/*
=============
IN_Move -- add accumulated relative mouse motion to the movement command
=============
*/
void IN_Move (usercmd_t *cmd)
{
	static float	old_mx, old_my;
	float			mx, my;

	if (!mouse_active)
		return;

	if (m_filter->value)
	{
		mx = (mouse_dx + old_mx) * 0.5f;
		my = (mouse_dy + old_my) * 0.5f;
	}
	else
	{
		mx = (float)mouse_dx;
		my = (float)mouse_dy;
	}
	old_mx = (float)mouse_dx;
	old_my = (float)mouse_dy;
	mouse_dx = mouse_dy = 0;

	mx *= sensitivity->value;
	my *= sensitivity->value;

	// apply to yaw/pitch or strafe/forward exactly as the original IN_MouseMove
	extern kbutton_t in_strafe;
	if ((in_strafe.state & 1) || (lookstrafe->value && mlooking))
		cmd->sidemove += m_side->value * mx;
	else
		cl.viewangles[YAW] -= m_yaw->value * mx;

	if ((mlooking || freelook->value) && !(in_strafe.state & 1))
		cl.viewangles[PITCH] += m_pitch->value * my;
	else
		cmd->forwardmove -= m_forward->value * my;
}
