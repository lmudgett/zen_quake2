// vid_menu.c -- the in-game Video options menu for the SDL/GL3 client.
// Trimmed from win32/vid_menu.c: one renderer, no software/3Dfx driver
// picker; extended with the gl3 renderer's modern-effect settings. Almost
// everything applies live -- only settings that change what gets loaded
// (hi-res textures, bump maps) force a renderer restart.

#include "../client/client.h"
#include "../client/qmenu.h"

extern cvar_t *vid_ref;
extern cvar_t *vid_fullscreen;
extern cvar_t *vid_gamma;
extern cvar_t *scr_viewsize;

static cvar_t *gl_mode;
static cvar_t *gl_2dscale;
static cvar_t *gl_msaa;
static cvar_t *gl_anisotropy;
static cvar_t *gl_renderscale;
static cvar_t *gl_bloom;
static cvar_t *gl_dynamic;
static cvar_t *gl_bump;
static cvar_t *gl_shadows;
static cvar_t *gl_retexture;
static cvar_t *gl_wateralpha;

extern void M_ForceMenuOff (void);
extern void M_PopMenu (void);

static menuframework_s	s_video_menu;
static menulist_s		s_mode_list;
static menulist_s		s_fs_box;
static menuslider_s		s_screensize_slider;
static menuslider_s		s_brightness_slider;
static menulist_s		s_uiscale_list;
static menulist_s		s_msaa_list;
static menulist_s		s_aniso_list;
static menulist_s		s_rscale_list;
static menuslider_s		s_bloom_slider;
static menulist_s		s_dlight_list;
static menulist_s		s_bump_list;
static menulist_s		s_shadows_box;
static menulist_s		s_retexture_box;
static menuslider_s		s_water_slider;
static menuaction_s		s_defaults_action;
static menuaction_s		s_apply_action;

// spin-control value tables
static const int	msaa_values[]   = { 0, 2, 4, 8 };
static const int	aniso_values[]  = { 1, 2, 4, 8, 16 };
static const float	rscale_values[] = { 0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f };

static int NearestIndex (float v, const float *tab, int n)
{
	int		i, best = 0;
	float	d, bestd = 1e30f;

	for (i = 0; i < n; i++)
	{
		d = (float)fabs (tab[i] - v);
		if (d < bestd)
		{
			bestd = d;
			best = i;
		}
	}
	return best;
}

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

static void BloomCallback (void *s)
{
	menuslider_s *slider = (menuslider_s *)s;
	Cvar_SetValue ("gl_bloom", slider->curvalue / 10.0f);
}

static void WaterCallback (void *s)
{
	menuslider_s *slider = (menuslider_s *)s;
	Cvar_SetValue ("gl_wateralpha", slider->curvalue / 10.0f);
}

static void ResetDefaults (void *unused)
{
	VID_MenuInit ();
}

static void ApplyChanges (void *unused)
{
	float	gamma = (0.8f - (s_brightness_slider.curvalue / 10.0f - 0.5f)) + 0.5f;
	int		old_retexture = (int)gl_retexture->value;
	int		old_bump = (int)gl_bump->value;

	Cvar_SetValue ("vid_gamma", gamma);
	Cvar_SetValue ("vid_fullscreen", s_fs_box.curvalue);
	Cvar_SetValue ("gl_mode", s_mode_list.curvalue);
	Cvar_SetValue ("gl_2dscale", s_uiscale_list.curvalue);
	Cvar_SetValue ("gl_msaa", msaa_values[s_msaa_list.curvalue]);
	Cvar_SetValue ("gl_anisotropy", aniso_values[s_aniso_list.curvalue]);
	Cvar_SetValue ("gl_renderscale", rscale_values[s_rscale_list.curvalue]);
	Cvar_SetValue ("gl_bloom", s_bloom_slider.curvalue / 10.0f);
	Cvar_SetValue ("gl_dynamic", s_dlight_list.curvalue);
	Cvar_SetValue ("gl_bump", s_bump_list.curvalue);
	Cvar_SetValue ("gl_shadows", s_shadows_box.curvalue);
	Cvar_SetValue ("gl_retexture", s_retexture_box.curvalue);
	Cvar_SetValue ("gl_wateralpha", s_water_slider.curvalue / 10.0f);

	// everything above applies live except what changes LOADED assets:
	// hi-res overrides and (newly enabled) bump maps need a texture reload
	if ((int)gl_retexture->value != old_retexture
		|| ((int)gl_bump->value && !old_bump))
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
	static const char *yesno_names[]   = { "no", "yes", 0 };
	static const char *uiscale_names[] = { "auto", "1x", "2x", "3x", "4x", 0 };
	static const char *msaa_names[]    = { "off", "2x", "4x", "8x", 0 };
	static const char *aniso_names[]   = { "off", "2x", "4x", "8x", "16x", 0 };
	static const char *rscale_names[]  = { "50%", "75%", "100%", "125%", "150%", "200%", 0 };
	static const char *dlight_names[]  = { "off", "classic", "per-pixel", 0 };
	static const char *bump_names[]    = { "off", "packs only", "auto", 0 };
	int	i, y = 0;

	if (!gl_mode)			gl_mode = Cvar_Get ("gl_mode", "3", CVAR_ARCHIVE);
	if (!scr_viewsize)		scr_viewsize = Cvar_Get ("viewsize", "100", CVAR_ARCHIVE);
	if (!gl_2dscale)		gl_2dscale = Cvar_Get ("gl_2dscale", "0", CVAR_ARCHIVE);
	if (!gl_msaa)			gl_msaa = Cvar_Get ("gl_msaa", "4", CVAR_ARCHIVE);
	if (!gl_anisotropy)		gl_anisotropy = Cvar_Get ("gl_anisotropy", "8", CVAR_ARCHIVE);
	if (!gl_renderscale)	gl_renderscale = Cvar_Get ("gl_renderscale", "1", CVAR_ARCHIVE);
	if (!gl_bloom)			gl_bloom = Cvar_Get ("gl_bloom", "0.4", CVAR_ARCHIVE);
	if (!gl_dynamic)		gl_dynamic = Cvar_Get ("gl_dynamic", "2", 0);
	if (!gl_bump)			gl_bump = Cvar_Get ("gl_bump", "2", CVAR_ARCHIVE);
	if (!gl_shadows)		gl_shadows = Cvar_Get ("gl_shadows", "1", CVAR_ARCHIVE);
	if (!gl_retexture)		gl_retexture = Cvar_Get ("gl_retexture", "1", CVAR_ARCHIVE);
	if (!gl_wateralpha)		gl_wateralpha = Cvar_Get ("gl_wateralpha", "0.75", CVAR_ARCHIVE);

	s_video_menu.x = viddef.width * 0.50f;
	s_video_menu.nitems = 0;

	s_mode_list.generic.type = MTYPE_SPINCONTROL;
	s_mode_list.generic.name = "video mode";
	s_mode_list.generic.x = 0;
	s_mode_list.generic.y = y; y += 10;
	s_mode_list.itemnames = resolutions;
	s_mode_list.curvalue = gl_mode->value;

	s_fs_box.generic.type = MTYPE_SPINCONTROL;
	s_fs_box.generic.x = 0;
	s_fs_box.generic.y = y; y += 10;
	s_fs_box.generic.name = "fullscreen";
	s_fs_box.itemnames = yesno_names;
	s_fs_box.curvalue = vid_fullscreen->value;

	s_screensize_slider.generic.type = MTYPE_SLIDER;
	s_screensize_slider.generic.x = 0;
	s_screensize_slider.generic.y = y; y += 10;
	s_screensize_slider.generic.name = "screen size";
	s_screensize_slider.minvalue = 3;
	s_screensize_slider.maxvalue = 12;
	s_screensize_slider.generic.callback = ScreenSizeCallback;
	s_screensize_slider.curvalue = scr_viewsize->value / 10;

	s_brightness_slider.generic.type = MTYPE_SLIDER;
	s_brightness_slider.generic.x = 0;
	s_brightness_slider.generic.y = y; y += 10;
	s_brightness_slider.generic.name = "brightness";
	s_brightness_slider.generic.callback = BrightnessCallback;
	s_brightness_slider.minvalue = 5;
	s_brightness_slider.maxvalue = 13;
	s_brightness_slider.curvalue = (1.3f - vid_gamma->value + 0.5f) * 10;

	s_uiscale_list.generic.type = MTYPE_SPINCONTROL;
	s_uiscale_list.generic.x = 0;
	s_uiscale_list.generic.y = y; y += 10;
	s_uiscale_list.generic.name = "ui scale";
	s_uiscale_list.itemnames = uiscale_names;
	i = (int)gl_2dscale->value;
	s_uiscale_list.curvalue = (i >= 0 && i <= 4) ? i : 0;

	s_msaa_list.generic.type = MTYPE_SPINCONTROL;
	s_msaa_list.generic.x = 0;
	s_msaa_list.generic.y = y; y += 10;
	s_msaa_list.generic.name = "antialiasing";
	s_msaa_list.itemnames = msaa_names;
	s_msaa_list.curvalue = 0;
	for (i = 0; i < 4; i++)
		if ((int)gl_msaa->value >= msaa_values[i])
			s_msaa_list.curvalue = i;

	s_aniso_list.generic.type = MTYPE_SPINCONTROL;
	s_aniso_list.generic.x = 0;
	s_aniso_list.generic.y = y; y += 10;
	s_aniso_list.generic.name = "anisotropic filter";
	s_aniso_list.itemnames = aniso_names;
	s_aniso_list.curvalue = 0;
	for (i = 0; i < 5; i++)
		if ((int)gl_anisotropy->value >= aniso_values[i])
			s_aniso_list.curvalue = i;

	s_rscale_list.generic.type = MTYPE_SPINCONTROL;
	s_rscale_list.generic.x = 0;
	s_rscale_list.generic.y = y; y += 10;
	s_rscale_list.generic.name = "render scale";
	s_rscale_list.itemnames = rscale_names;
	s_rscale_list.curvalue = NearestIndex (gl_renderscale->value, rscale_values, 6);

	s_bloom_slider.generic.type = MTYPE_SLIDER;
	s_bloom_slider.generic.x = 0;
	s_bloom_slider.generic.y = y; y += 10;
	s_bloom_slider.generic.name = "bloom";
	s_bloom_slider.generic.callback = BloomCallback;
	s_bloom_slider.minvalue = 0;
	s_bloom_slider.maxvalue = 10;
	s_bloom_slider.curvalue = gl_bloom->value * 10;

	s_dlight_list.generic.type = MTYPE_SPINCONTROL;
	s_dlight_list.generic.x = 0;
	s_dlight_list.generic.y = y; y += 10;
	s_dlight_list.generic.name = "dynamic lights";
	s_dlight_list.itemnames = dlight_names;
	i = (int)gl_dynamic->value;
	s_dlight_list.curvalue = (i >= 0 && i <= 2) ? i : 2;

	s_bump_list.generic.type = MTYPE_SPINCONTROL;
	s_bump_list.generic.x = 0;
	s_bump_list.generic.y = y; y += 10;
	s_bump_list.generic.name = "bump mapping";
	s_bump_list.itemnames = bump_names;
	i = (int)gl_bump->value;
	s_bump_list.curvalue = (i >= 0 && i <= 2) ? i : 2;

	s_shadows_box.generic.type = MTYPE_SPINCONTROL;
	s_shadows_box.generic.x = 0;
	s_shadows_box.generic.y = y; y += 10;
	s_shadows_box.generic.name = "entity shadows";
	s_shadows_box.itemnames = yesno_names;
	s_shadows_box.curvalue = gl_shadows->value ? 1 : 0;

	s_retexture_box.generic.type = MTYPE_SPINCONTROL;
	s_retexture_box.generic.x = 0;
	s_retexture_box.generic.y = y; y += 10;
	s_retexture_box.generic.name = "hi-res textures";
	s_retexture_box.itemnames = yesno_names;
	s_retexture_box.curvalue = gl_retexture->value ? 1 : 0;

	s_water_slider.generic.type = MTYPE_SLIDER;
	s_water_slider.generic.x = 0;
	s_water_slider.generic.y = y; y += 10;
	s_water_slider.generic.name = "water opacity";
	s_water_slider.generic.callback = WaterCallback;
	s_water_slider.minvalue = 2;
	s_water_slider.maxvalue = 10;
	s_water_slider.curvalue = gl_wateralpha->value * 10;

	y += 10;
	s_defaults_action.generic.type = MTYPE_ACTION;
	s_defaults_action.generic.name = "reset to defaults";
	s_defaults_action.generic.x = 0;
	s_defaults_action.generic.y = y; y += 10;
	s_defaults_action.generic.callback = ResetDefaults;

	s_apply_action.generic.type = MTYPE_ACTION;
	s_apply_action.generic.name = "apply";
	s_apply_action.generic.x = 0;
	s_apply_action.generic.y = y;
	s_apply_action.generic.callback = ApplyChanges;

	Menu_AddItem (&s_video_menu, &s_mode_list);
	Menu_AddItem (&s_video_menu, &s_fs_box);
	Menu_AddItem (&s_video_menu, &s_screensize_slider);
	Menu_AddItem (&s_video_menu, &s_brightness_slider);
	Menu_AddItem (&s_video_menu, &s_uiscale_list);
	Menu_AddItem (&s_video_menu, &s_msaa_list);
	Menu_AddItem (&s_video_menu, &s_aniso_list);
	Menu_AddItem (&s_video_menu, &s_rscale_list);
	Menu_AddItem (&s_video_menu, &s_bloom_slider);
	Menu_AddItem (&s_video_menu, &s_dlight_list);
	Menu_AddItem (&s_video_menu, &s_bump_list);
	Menu_AddItem (&s_video_menu, &s_shadows_box);
	Menu_AddItem (&s_video_menu, &s_retexture_box);
	Menu_AddItem (&s_video_menu, &s_water_slider);
	Menu_AddItem (&s_video_menu, &s_defaults_action);
	Menu_AddItem (&s_video_menu, &s_apply_action);

	Menu_Center (&s_video_menu);
	s_video_menu.x -= 8;
}

void VID_MenuDraw (void)
{
	int w, h;

	re.DrawGetPicSize (&w, &h, "m_banner_video");
	re.DrawPic (viddef.width / 2 - w / 2, s_video_menu.y - 30, "m_banner_video");

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
