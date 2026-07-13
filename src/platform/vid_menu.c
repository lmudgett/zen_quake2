// vid_menu.c -- the in-game Video options menu for the SDL/GL3 client.
// Trimmed from win32/vid_menu.c: one renderer, no software/3Dfx driver picker.

#include "../client/client.h"
#include "../client/qmenu.h"

extern cvar_t *vid_ref;
extern cvar_t *vid_fullscreen;
extern cvar_t *vid_gamma;
extern cvar_t *scr_viewsize;

static cvar_t *gl_mode;
static cvar_t *gl_picmip;

extern void M_ForceMenuOff (void);
extern void M_PopMenu (void);

static menuframework_s	s_video_menu;
static menulist_s		s_mode_list;
static menuslider_s		s_screensize_slider;
static menuslider_s		s_brightness_slider;
static menulist_s		s_fs_box;
static menuslider_s		s_tq_slider;
static menuaction_s		s_defaults_action;
static menuaction_s		s_apply_action;

static void ScreenSizeCallback (void *s)
{
	menuslider_s *slider = (menuslider_s *)s;
	Cvar_SetValue ("viewsize", slider->curvalue * 10);
}

static void BrightnessCallback (void *s)
{
	menuslider_s *slider = (menuslider_s *)s;
	float gamma = (0.8f - (slider->curvalue / 10.0f - 0.5f)) + 0.5f;
	Cvar_SetValue ("vid_gamma", gamma);
}

static void ResetDefaults (void *unused)
{
	VID_MenuInit ();
}

static void ApplyChanges (void *unused)
{
	float gamma = (0.8f - (s_brightness_slider.curvalue / 10.0f - 0.5f)) + 0.5f;

	Cvar_SetValue ("vid_gamma", gamma);
	Cvar_SetValue ("gl_picmip", 3 - s_tq_slider.curvalue);
	Cvar_SetValue ("vid_fullscreen", s_fs_box.curvalue);
	Cvar_SetValue ("gl_mode", s_mode_list.curvalue);

	vid_ref->modified = true;	// force a vid_restart

	M_ForceMenuOff ();
}

void VID_MenuInit (void)
{
	static const char *resolutions[] =
	{
		"[320 240   ]", "[400 300   ]", "[512 384   ]", "[640 480   ]",
		"[800 600   ]", "[960 720   ]", "[1024 768  ]", "[1152 864  ]",
		"[1280 960  ]", "[1600 1200 ]", "[1920 1080 ]", "[2560 1440 ]",
		0
	};
	static const char *yesno_names[] = { "no", "yes", 0 };

	if (!gl_mode)
		gl_mode = Cvar_Get ("gl_mode", "3", CVAR_ARCHIVE);
	if (!gl_picmip)
		gl_picmip = Cvar_Get ("gl_picmip", "0", 0);
	if (!scr_viewsize)
		scr_viewsize = Cvar_Get ("viewsize", "100", CVAR_ARCHIVE);

	s_video_menu.x = viddef.width * 0.50f;
	s_video_menu.nitems = 0;

	s_mode_list.generic.type = MTYPE_SPINCONTROL;
	s_mode_list.generic.name = "video mode";
	s_mode_list.generic.x = 0;
	s_mode_list.generic.y = 0;
	s_mode_list.itemnames = resolutions;
	s_mode_list.curvalue = gl_mode->value;

	s_screensize_slider.generic.type = MTYPE_SLIDER;
	s_screensize_slider.generic.x = 0;
	s_screensize_slider.generic.y = 10;
	s_screensize_slider.generic.name = "screen size";
	s_screensize_slider.minvalue = 3;
	s_screensize_slider.maxvalue = 12;
	s_screensize_slider.generic.callback = ScreenSizeCallback;
	s_screensize_slider.curvalue = scr_viewsize->value / 10;

	s_brightness_slider.generic.type = MTYPE_SLIDER;
	s_brightness_slider.generic.x = 0;
	s_brightness_slider.generic.y = 20;
	s_brightness_slider.generic.name = "brightness";
	s_brightness_slider.generic.callback = BrightnessCallback;
	s_brightness_slider.minvalue = 5;
	s_brightness_slider.maxvalue = 13;
	s_brightness_slider.curvalue = (1.3f - vid_gamma->value + 0.5f) * 10;

	s_fs_box.generic.type = MTYPE_SPINCONTROL;
	s_fs_box.generic.x = 0;
	s_fs_box.generic.y = 30;
	s_fs_box.generic.name = "fullscreen";
	s_fs_box.itemnames = yesno_names;
	s_fs_box.curvalue = vid_fullscreen->value;

	s_tq_slider.generic.type = MTYPE_SLIDER;
	s_tq_slider.generic.x = 0;
	s_tq_slider.generic.y = 40;
	s_tq_slider.generic.name = "texture quality";
	s_tq_slider.minvalue = 0;
	s_tq_slider.maxvalue = 3;
	s_tq_slider.curvalue = 3 - gl_picmip->value;

	s_defaults_action.generic.type = MTYPE_ACTION;
	s_defaults_action.generic.name = "reset to defaults";
	s_defaults_action.generic.x = 0;
	s_defaults_action.generic.y = 60;
	s_defaults_action.generic.callback = ResetDefaults;

	s_apply_action.generic.type = MTYPE_ACTION;
	s_apply_action.generic.name = "apply";
	s_apply_action.generic.x = 0;
	s_apply_action.generic.y = 70;
	s_apply_action.generic.callback = ApplyChanges;

	Menu_AddItem (&s_video_menu, &s_mode_list);
	Menu_AddItem (&s_video_menu, &s_screensize_slider);
	Menu_AddItem (&s_video_menu, &s_brightness_slider);
	Menu_AddItem (&s_video_menu, &s_fs_box);
	Menu_AddItem (&s_video_menu, &s_tq_slider);
	Menu_AddItem (&s_video_menu, &s_defaults_action);
	Menu_AddItem (&s_video_menu, &s_apply_action);

	Menu_Center (&s_video_menu);
	s_video_menu.x -= 8;
}

void VID_MenuDraw (void)
{
	int w, h;

	re.DrawGetPicSize (&w, &h, "m_banner_video");
	re.DrawPic (viddef.width / 2 - w / 2, viddef.height / 2 - 110, "m_banner_video");

	Menu_AdjustCursor (&s_video_menu, 1);
	Menu_Draw (&s_video_menu);
}

const char *VID_MenuKey (int key)
{
	menuframework_s *m = &s_video_menu;
	static const char *sound = "misc/menu1.wav";

	switch (key)
	{
	case K_ESCAPE:
		ApplyChanges (0);
		return NULL;
	case K_KP_UPARROW:
	case K_UPARROW:
		m->cursor--;
		Menu_AdjustCursor (m, -1);
		break;
	case K_KP_DOWNARROW:
	case K_DOWNARROW:
		m->cursor++;
		Menu_AdjustCursor (m, 1);
		break;
	case K_KP_LEFTARROW:
	case K_LEFTARROW:
		Menu_SlideItem (m, -1);
		break;
	case K_KP_RIGHTARROW:
	case K_RIGHTARROW:
		Menu_SlideItem (m, 1);
		break;
	case K_KP_ENTER:
	case K_ENTER:
		if (!Menu_SelectItem (m))
			ApplyChanges (NULL);
		break;
	}

	return sound;
}
