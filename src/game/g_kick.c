// g_kick.c -- boot kick: a bindable melee move (console/bind "kick") that
// plays a leg view animation, damages and shoves whatever the boot lands on,
// and can punt buttons and barrels.
//
// The kick borrows the view model for its short animation: Think_Weapon is
// frozen while client->kicking is set, Kick_Update (ClientEndServerFrame)
// drives ps.gunindex/gunframe from the kick clock and restores the weapon's
// own state when done, so the current weapon resumes exactly where it was.
//
// Author: Len Mudgett

#include "g_local.h"

#define KICK_MODEL		"models/weapons/v_kick/tris.md2"
#define KICK_FRAMES		6		// md2 frames, one per server frame (0.6s)
#define KICK_IMPACT		2		// frame on which the boot lands
#define KICK_COOLDOWN	10		// server frames from one kick to the next
#define KICK_RANGE		92.0f	// reach from the eye, world units
#define KICK_DAMAGE		25
#define KICK_KNOCKBACK	180		// the shove is the point of the move

/*
=================
Kick_Impact

The boot lands: fat box trace out from the eye; damage + upward-tilted
shove for anything damageable (monsters, players, barrels, exploding
walls), a use-trigger for buttons, a dull thud for the world.
=================
*/
static void Kick_Impact (edict_t *ent)
{
	vec3_t		start, forward, up, end, dir;
	vec3_t		mins = {-6, -6, -6}, maxs = {6, 6, 6};
	trace_t		tr;

	AngleVectors (ent->client->v_angle, forward, NULL, up);
	VectorCopy (ent->s.origin, start);
	start[2] += ent->viewheight;
	VectorMA (start, KICK_RANGE, forward, end);

	// a fat trace so the boot connects like a boot, not a needle
	tr = gi.trace (start, mins, maxs, end, ent, MASK_SHOT);
	if (tr.fraction == 1.0)
		return;			// whiffed

	if (tr.ent && tr.ent->takedamage)
	{
		VectorMA (forward, 0.3f, up, dir);
		VectorNormalize (dir);
		T_Damage (tr.ent, ent, ent, dir, tr.endpos, vec3_origin,
			KICK_DAMAGE, KICK_KNOCKBACK, 0, MOD_HIT);
		gi.sound (ent, CHAN_AUTO, gi.soundindex ("mutant/thud1.wav"), 1, ATTN_NORM, 0);
	}
	else if (tr.ent && tr.ent->use && strcmp (tr.ent->classname, "func_button") == 0)
	{	// kick the button in
		tr.ent->use (tr.ent, ent, ent);
		gi.sound (ent, CHAN_AUTO, gi.soundindex ("mutant/thud1.wav"), 1, ATTN_NORM, 0);
	}
	else
	{	// booted the world
		gi.sound (ent, CHAN_AUTO, gi.soundindex ("tank/thud.wav"), 1, ATTN_NORM, 0);
	}

	// impact feedback: a small upward view punch
	ent->client->kick_angles[PITCH] -= 2.0f;
}

/*
=================
Cmd_Kick_f

"kick" client command (bind a key to "kick").
=================
*/
void Cmd_Kick_f (edict_t *ent)
{
	gclient_t	*client = ent->client;

	if (ent->health <= 0 || ent->deadflag)
		return;
	if (client->resp.spectator)
		return;
	if (client->kicking || level.framenum < client->kick_wait)
		return;
	if (client->pers.weapon && client->weaponstate != WEAPON_READY)
		return;			// don't yank a firing/reloading/switching weapon

	client->kicking = true;
	// +1: client commands are processed before G_RunFrame increments
	// level.framenum, so the first Kick_Update runs on framenum+1 -- without
	// this the animation would skip frame 0, and the client's "gunframe 0 =
	// just switched, don't lerp" guard would never fire (stale-oldframe pop)
	client->kick_start = level.framenum + 1;
	client->kick_wait = level.framenum + KICK_COOLDOWN;
	client->kick_saved_gunframe = client->ps.gunframe;
	// sexed exertion grunt -- resolves to the player model's own voice
	// (player/male/jump1.wav for the marine), already precached by worldspawn
	gi.sound (ent, CHAN_VOICE, gi.soundindex ("*jump1.wav"), 1, ATTN_NORM, 0);
}

/*
=================
Kick_Update

Called every server frame from ClientEndServerFrame (before
SV_CalcViewOffset, so the impact's kick_angles land the same frame).
Drives the leg animation over the weapon's view model slot.
=================
*/
void Kick_Update (edict_t *ent)
{
	gclient_t	*client = ent->client;
	int			frame;

	if (!client->kicking)
		return;

	if (ent->health <= 0 || ent->deadflag)
	{	// died mid-kick; the weapon-away logic owns the gun now
		client->kicking = false;
		return;
	}

	frame = level.framenum - client->kick_start;
	if (frame < 0)
		return;			// starts next frame; leave the weapon alone until then
	if (frame >= KICK_FRAMES)
	{	// done: hand the view back to the real weapon, exactly as it was
		client->kicking = false;
		client->ps.gunindex = client->pers.weapon ?
			gi.modelindex (client->pers.weapon->view_model) : 0;
		client->ps.gunframe = client->kick_saved_gunframe;
		return;
	}

	if (frame == KICK_IMPACT)
		Kick_Impact (ent);

	client->ps.gunindex = gi.modelindex (KICK_MODEL);
	client->ps.gunframe = frame;
}
