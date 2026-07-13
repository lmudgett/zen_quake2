// snd_sdl.c -- SNDDMA_* backend on top of SDL3 audio. The portable mixer
// (snd_mix.c) paints 16-bit samples into our ring buffer (exposed as the
// global dma_t); an SDL audio-stream pull callback drains the ring to the
// device. Replaces win32/snd_win.c (DirectSound).

#include <SDL3/SDL.h>

#include "../client/client.h"
#include "../client/snd_loc.h"

static SDL_AudioStream	*stream;
static int				buffer_bytes;	// size of dma.buffer in bytes
static int				read_pos;		// device read cursor, in bytes

/*
=============
SDL_SoundCallback

Runs on SDL's audio thread. Copy whatever the mixer has painted from the
ring (wrapping at the end) into the stream. The mixer keeps painting ahead
of read_pos, so brief over/under-run just replays a little audio -- the same
tolerance the original DMA backends relied on.
=============
*/
static void SDLCALL SDL_SoundCallback (void *userdata, SDL_AudioStream *strm,
	int additional, int total)
{
	int	remaining = additional;

	if (!dma.buffer)
		return;

	while (remaining > 0)
	{
		int chunk = buffer_bytes - read_pos;
		if (chunk > remaining)
			chunk = remaining;

		SDL_PutAudioStreamData (strm, dma.buffer + read_pos, chunk);

		read_pos += chunk;
		if (read_pos >= buffer_bytes)
			read_pos = 0;
		remaining -= chunk;
	}
}

qboolean SNDDMA_Init (void)
{
	SDL_AudioSpec	spec;
	cvar_t			*s_khz;
	int				samples;

	if (!SDL_InitSubSystem (SDL_INIT_AUDIO))
	{
		Com_Printf ("SNDDMA_Init: SDL_InitSubSystem(AUDIO) failed: %s\n", SDL_GetError());
		return false;
	}

	s_khz = Cvar_Get ("s_khz", "44", CVAR_ARCHIVE);

	spec.freq = (s_khz->value >= 44) ? 44100 : (s_khz->value >= 22) ? 22050 : 11025;
	spec.format = SDL_AUDIO_S16;
	spec.channels = 2;

	// ring holds ~0.5s so the mixer has comfortable head room; power-of-two frames
	samples = 16384;
	buffer_bytes = samples * spec.channels * (SDL_AUDIO_BITSIZE(spec.format) / 8);

	dma.buffer = calloc (1, buffer_bytes);
	if (!dma.buffer)
	{
		Com_Printf ("SNDDMA_Init: out of memory\n");
		return false;
	}

	dma.channels = spec.channels;
	dma.samplebits = SDL_AUDIO_BITSIZE (spec.format);
	dma.speed = spec.freq;
	dma.samples = buffer_bytes / (dma.samplebits / 8);	// total mono samples
	dma.samplepos = 0;
	dma.submission_chunk = 1;
	read_pos = 0;

	stream = SDL_OpenAudioDeviceStream (SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
		&spec, SDL_SoundCallback, NULL);
	if (!stream)
	{
		Com_Printf ("SNDDMA_Init: SDL_OpenAudioDeviceStream failed: %s\n", SDL_GetError());
		free (dma.buffer);
		dma.buffer = NULL;
		return false;
	}

	SDL_ResumeAudioStreamDevice (stream);

	Com_Printf ("SDL3 audio: %d Hz, %d channels, %d-bit\n",
		dma.speed, dma.channels, dma.samplebits);

	return true;
}

int SNDDMA_GetDMAPos (void)
{
	// current device read position, expressed in mono samples
	if (!dma.buffer)
		return 0;
	dma.samplepos = (read_pos / (dma.samplebits / 8));
	return dma.samplepos;
}

void SNDDMA_Shutdown (void)
{
	if (stream)
	{
		SDL_DestroyAudioStream (stream);
		stream = NULL;
	}
	if (dma.buffer)
	{
		free (dma.buffer);
		dma.buffer = NULL;
	}
	SDL_QuitSubSystem (SDL_INIT_AUDIO);
}

/*
=============
SNDDMA_BeginPainting / SNDDMA_Submit

With a pull callback the device drains the ring on its own thread; we only
need to keep the callback from reading a half-written region while the mixer
paints. Lock/unlock the stream around the paint window.
=============
*/
void SNDDMA_BeginPainting (void)
{
	if (stream)
		SDL_LockAudioStream (stream);
}

void SNDDMA_Submit (void)
{
	if (stream)
		SDL_UnlockAudioStream (stream);
}

/*
=============
S_Activate

Pause/resume the audio device when the window loses/gains focus.
=============
*/
void S_Activate (qboolean active)
{
	if (!stream)
		return;
	if (active)
		SDL_ResumeAudioStreamDevice (stream);
	else
		SDL_PauseAudioStreamDevice (stream);
}
