// g_waves.c -- "Nemesis" wave-survival mode.
//
// Escalating waves of Quake 2 monsters spawn around the player. Every few
// waves a buffed NEMESIS boss appears, hunts the player, and returns stronger
// each time it is defeated. Runs in deathmatch mode (players respawn, DM maps
// are a clean canvas); the wave_active flag keeps monsters from self-deleting.
//
// Start it by typing "nemesis" in the console during a deathmatch game.

#include "g_local.h"

qboolean	wave_active;	// checked by monster_start() in g_monster.c

#define WS_COUNTDOWN	0
#define WS_ACTIVE		1

typedef struct
{
	qboolean	active;
	int			state;
	int			number;			// current wave
	int			to_spawn;		// monsters left to spawn this wave
	float		next_time;		// countdown / next spawn timestamp
	qboolean	alive_last;		// player alive last frame (death detection)
	edict_t		*nemesis;
	int			nemesis_level;	// how many times the nemesis has been beaten
	int			best_wave;
} wavestate_t;

static wavestate_t	wave;

// monster rosters by difficulty
static char *roster_easy[]  = { "monster_soldier_light", "monster_soldier" };
static char *roster_med[]   = { "monster_soldier_ss", "monster_infantry", "monster_gunner", "monster_parasite" };
static char *roster_hard[]  = { "monster_berserk", "monster_gladiator", "monster_mutant", "monster_chick" };
static char *roster_elite[] = { "monster_gunner", "monster_medic", "monster_gladiator", "monster_tank" };

static char *WavePickClass (int w)
{
	int	r = rand() % 100;

	if (w <= 2)
		return roster_easy[rand() % 2];
	if (w <= 5)
		return (r < 55) ? roster_easy[rand() % 2] : roster_med[rand() % 4];
	if (w <= 9)
		return (r < 35) ? roster_med[rand() % 4] : roster_hard[rand() % 4];
	return (r < 45) ? roster_hard[rand() % 4] : roster_elite[rand() % 4];
}

// ------------------------------------------------------------------ helpers

static edict_t *WaveFindPlayer (void)
{
	int	i;

	for (i = 1; i <= maxclients->value; i++)
	{
		edict_t	*e = &g_edicts[i];
		if (e->inuse && e->client && e->health > 0 && e->deadflag == DEAD_NO)
			return e;
	}
	return NULL;
}

// any connected client, alive or dead
static edict_t *WaveAnyClient (void)
{
	int	i;

	for (i = 1; i <= maxclients->value; i++)
	{
		edict_t	*e = &g_edicts[i];
		if (e->inuse && e->client)
			return e;
	}
	return NULL;
}

static int WaveCountMonsters (void)
{
	int		i, n = 0;
	edict_t	*e;

	for (i = (int)maxclients->value + 1; i < globals.num_edicts; i++)
	{
		e = &g_edicts[i];
		if (e->inuse && (e->svflags & SVF_MONSTER) && e->health > 0
			&& !(e->svflags & SVF_DEADMONSTER))
			n++;
	}
	return n;
}

// find an open spot to spawn a monster: prefer a distant deathmatch spawn
// point, else fall back to a traced position offset from the player
static qboolean WavePickSpot (edict_t *player, vec3_t out)
{
	edict_t	*spot = NULL, *far_spots[64], *any_spots[64];
	int		nfar = 0, nany = 0, tries;

	while ((spot = G_Find (spot, FOFS(classname), "info_player_deathmatch")) != NULL)
	{
		vec3_t	d;
		if (nany < 64)
			any_spots[nany++] = spot;
		VectorSubtract (spot->s.origin, player->s.origin, d);
		if (VectorLength (d) > 250 && nfar < 64)
			far_spots[nfar++] = spot;
	}

	if (nfar)
	{
		VectorCopy (far_spots[rand() % nfar]->s.origin, out);
		return true;
	}
	if (nany)
	{
		VectorCopy (any_spots[rand() % nany]->s.origin, out);
		return true;
	}

	// no DM spawn points: try random spots in open air around the player
	for (tries = 0; tries < 16; tries++)
	{
		float	ang = (rand() % 360) * (M_PI / 180.0f);
		float	dist = 200 + (rand() % 300);
		vec3_t	p, end;
		trace_t	tr;

		p[0] = player->s.origin[0] + cos(ang) * dist;
		p[1] = player->s.origin[1] + sin(ang) * dist;
		p[2] = player->s.origin[2] + 32;

		if (gi.pointcontents (p) & MASK_SOLID)
			continue;

		VectorCopy (p, end);
		end[2] -= 512;
		tr = gi.trace (p, NULL, NULL, end, player, MASK_SOLID);
		if (tr.fraction == 1.0f || tr.startsolid)
			continue;

		VectorCopy (tr.endpos, out);
		out[2] += 24;
		return true;
	}
	return false;
}

static void WaveFacePlayer (edict_t *m, edict_t *player)
{
	vec3_t	dir;
	VectorSubtract (player->s.origin, m->s.origin, dir);
	m->s.angles[YAW] = vectoyaw (dir);
}

static edict_t *WaveSpawnMonster (edict_t *player, char *classname)
{
	vec3_t	spot;
	edict_t	*m;

	if (!WavePickSpot (player, spot))
	{
		gi.dprintf ("WAVE: no spawn spot for %s\n", classname);
		return NULL;
	}

	m = G_Spawn ();
	m->classname = classname;			// literal, persists
	VectorCopy (spot, m->s.origin);
	m->s.origin[2] += 16;				// nudge up so they drop cleanly to floor

	memset (&st, 0, sizeof(st));		// no leftover spawn-temp keys
	m->spawnflags = 0;

	// Each monster's SP_ function bails out if deathmatch is set. ED_CallSpawn
	// is synchronous, so briefly clear deathmatch across the spawn call, then
	// restore it (players still respawn as normal on later frames).
	{
		float	saved_dm = deathmatch->value;
		deathmatch->value = 0;
		ED_CallSpawn (m);
		deathmatch->value = saved_dm;
	}
	if (!m->inuse)
	{
		gi.dprintf ("WAVE: %s freed itself after spawn\n", classname);
		return NULL;
	}

	WaveFacePlayer (m, player);

	// clear anything already occupying the spot (the picker can return the
	// same point twice in one wave; a monster stuck inside another never
	// pathed and its wave could never clear) -- id's triggered-spawn idiom.
	// KillBox traces with no passent, so unlink m first or the trace
	// startsolids against m's own box and the telefrag kills m itself
	// (id only ever KillBoxes entities that are not yet linked/solid).
	m->s.origin[2] += 1;
	gi.unlinkentity (m);
	KillBox (m);
	gi.linkentity (m);

	// The spawn function scheduled *monster_start_go for next frame; if we
	// aggroed first, its stand()/pausetime=1e8 would cancel the FoundTarget
	// one frame later, and with enemy already set FindTarget short-circuits
	// ("client == self->enemy -> return true") without ever re-aggroing --
	// the monster would stand frozen until its pain animation woke it. Run
	// the start think now, then aggro, matching monster_triggered_spawn.
	if (m->think)
		m->think (m);
	if (!m->inuse)
		return NULL;

	m->enemy = player;
	if (m->monsterinfo.run)
		FoundTarget (m);

	return m;
}

static void WaveSpawnNemesis (edict_t *player)
{
	edict_t	*m = WaveSpawnMonster (player, "monster_tank");

	if (!m)
		return;

	// buff: scale health with how many times it has already been beaten
	m->health = m->health * (2 + wave.nemesis_level);
	m->max_health = m->health;
	m->s.effects |= EF_COLOR_SHELL;
	m->s.renderfx |= RF_SHELL_RED;		// menacing red glow

	wave.nemesis = m;

	gi.bprintf (PRINT_HIGH, "\n*** THE NEMESIS HAS RETURNED (level %d) ***\n", wave.nemesis_level + 1);
	if (player->client)
		gi.centerprintf (player, "THE NEMESIS APPROACHES");
}

static void Wave_ArmPlayer (edict_t *ent)
{
	int		i;
	gitem_t	*it;

	if (!ent->client)
		return;

	ent->health = ent->max_health = 100;

	for (i = 0; i < game.num_items; i++)
	{
		it = itemlist + i;
		if (!it->pickup)
			continue;
		if (it->flags & IT_WEAPON)
			ent->client->pers.inventory[i] = 1;
		else if (it->flags & IT_AMMO)
			Add_Ammo (ent, it, 1000);
	}

	// body armor
	it = FindItem ("Body Armor");
	if (it)
		ent->client->pers.inventory[ITEM_INDEX(it)] = 150;

	// hand them something better than the blaster
	it = FindItem ("Chaingun");
	if (it)
	{
		ent->client->newweapon = it;
		ChangeWeapon (ent);
	}
}

// ------------------------------------------------------------------ wave flow

static void Wave_ClearMonsters (void)
{
	int		i;
	edict_t	*e;

	for (i = (int)maxclients->value + 1; i < globals.num_edicts; i++)
	{
		e = &g_edicts[i];
		if (e->inuse && (e->svflags & SVF_MONSTER))
			G_FreeEdict (e);
	}
	wave.nemesis = NULL;
}

static void Wave_StartNext (edict_t *player)
{
	wave.number++;
	wave.state = WS_ACTIVE;
	wave.to_spawn = 3 + wave.number * 2;
	if (wave.to_spawn > 24)
		wave.to_spawn = 24;
	wave.next_time = level.time;		// spawn first one immediately

	gi.bprintf (PRINT_HIGH, "\n======  WAVE %d  ======\n", wave.number);
	if (player->client)
		gi.centerprintf (player, "WAVE %d", wave.number);

	// a nemesis every 3rd wave
	if ((wave.number % 3) == 0)
		WaveSpawnNemesis (player);
}

// called from G_RunFrame every server frame
void Wave_RunFrame (void)
{
	edict_t	*player, *any;
	qboolean	alive;
	int		remaining;

	if (!wave.active)
		return;

	any = WaveAnyClient ();
	if (!any)
		return;						// nobody in the game yet

	player = WaveFindPlayer ();
	alive = (player != NULL);

	// --- death / respawn transitions ---
	if (wave.alive_last && !alive)
	{
		gi.bprintf (PRINT_HIGH, "\n%s fell at wave %d.  (best: wave %d)\n",
			any->client->pers.netname, wave.number,
			wave.number > wave.best_wave ? wave.number : wave.best_wave);
		if (wave.number > wave.best_wave)
			wave.best_wave = wave.number;
		Wave_ClearMonsters ();
		wave.number = 0;
		wave.to_spawn = 0;
		wave.state = WS_COUNTDOWN;
	}
	else if (!wave.alive_last && alive)
	{
		Wave_ArmPlayer (player);
		wave.state = WS_COUNTDOWN;
		wave.next_time = level.time + 3.0f;
		gi.centerprintf (player, "GET READY\nWave 1 incoming...");
	}
	wave.alive_last = alive;

	if (!alive)
		return;						// wait for respawn

	// --- nemesis defeated? ---
	if (wave.nemesis && (!wave.nemesis->inuse || wave.nemesis->health <= 0
		|| (wave.nemesis->svflags & SVF_DEADMONSTER)))
	{
		wave.nemesis_level++;
		wave.nemesis = NULL;
		gi.bprintf (PRINT_HIGH, "\nThe Nemesis is down -- but it will return stronger!\n");
	}

	// --- stagger spawning this wave's monsters ---
	if (wave.state == WS_ACTIVE && wave.to_spawn > 0 && level.time >= wave.next_time)
	{
		WaveSpawnMonster (player, WavePickClass (wave.number));
		wave.to_spawn--;
		wave.next_time = level.time + 0.6f;
		return;
	}

	remaining = WaveCountMonsters ();

	if (wave.state == WS_ACTIVE)
	{
		if (wave.to_spawn == 0 && remaining == 0)
		{
			gi.bprintf (PRINT_HIGH, "Wave %d cleared!  Next wave in 5...\n", wave.number);
			if (wave.number > wave.best_wave)
				wave.best_wave = wave.number;
			// reward: patch up and top off ammo
			if (player->health < player->max_health)
				player->health = player->max_health;
			wave.state = WS_COUNTDOWN;
			wave.next_time = level.time + 5.0f;
		}
	}
	else	// WS_COUNTDOWN
	{
		if (level.time >= wave.next_time)
			Wave_StartNext (player);
	}
}

// console command: "nemesis"
void Wave_Start (edict_t *ent)
{
	if (!deathmatch->value)
	{
		gi.cprintf (ent, PRINT_HIGH, "Nemesis wave mode needs deathmatch: +set deathmatch 1\n");
		return;
	}

	memset (&wave, 0, sizeof(wave));
	wave.active = true;
	wave_active = true;
	wave.state = WS_COUNTDOWN;
	wave.next_time = level.time + 4.0f;
	wave.alive_last = true;

	Wave_ArmPlayer (ent);

	gi.bprintf (PRINT_HIGH, "\n%s started NEMESIS wave survival!\n", ent->client->pers.netname);
	gi.centerprintf (ent, "N E M E S I S\n\nSurvive the waves.\nFirst wave in 4...");
}
