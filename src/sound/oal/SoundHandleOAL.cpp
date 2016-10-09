#include "SoundHandleOAL.h"
#include "SoundSystemOAL.h"
#include "StaticSoundHandleOAL.h"
#include "StreamingSoundHandleOAL.h"

namespace oxygine
{
	void OAL_CHECK();

	SoundSystemOAL* SoundHandleOAL::ss()
	{
		return (SoundSystemOAL*)SoundSystem::get();
	}

	SoundHandleOAL::SoundHandleOAL() : _alSource(0), _pos(0)//, _stream(0)
	{
	}

	void SoundHandleOAL::_init()
	{

	}

	void SoundHandleOAL::_restore()
	{
		alSourcef(_alSource, AL_GAIN, _volume);
		OAL_CHECK();

		alSourcef(_alSource, AL_PITCH, _pitch);
		OAL_CHECK();
	}

	void SoundHandleOAL::_play()
	{
		if (_alSource)
			_stop();

		_alSource = ss()->getSource();
		_restore();


		_xplay();

		alSourcei(_alSource, AL_BYTE_OFFSET, _pos);
		OAL_CHECK();

		alSourcePlay(_alSource);
		OAL_CHECK();
	}

	void SoundHandleOAL::_resume()
	{
		if (_alSource)
			return;

		_alSource = ss()->getSource();

		_restore();

		_xresume();

		alSourcei(_alSource, AL_BYTE_OFFSET, _pos);
		OAL_CHECK();

		alSourcePlay(_alSource);
		OAL_CHECK();
	}

	void SoundHandleOAL::_pause()
	{
		if (!_alSource)
			return;

		alGetSourcei(_alSource, AL_BYTE_OFFSET, &_pos);
		OAL_CHECK();

		alSourceStop(_alSource);
		OAL_CHECK();

		_xpause();

		//_stream->pause(this);


		alSourcei(_alSource, AL_LOOPING, AL_FALSE);

		ss()->freeSource(_alSource);
		_alSource = 0;
		_state = paused;
	}

	void SoundHandleOAL::_stop()
	{
		if (_alSource)
		{
			alSourceStop(_alSource);
			OAL_CHECK();
			ss()->freeSource(_alSource);
		}

		_xstop();

		_alSource = 0;
		_state = stopped;
		_pos = 0;
	}

	void SoundHandleOAL::_update()
	{
		if (!_alSource)
			return;

		_xupdate();

		//_stream->update(this);
	}

	void SoundHandleOAL::_updateVolume()
	{
		if (!_alSource)
			return;
		alSourcef(_alSource, AL_GAIN, _volume);
	}

	void SoundHandleOAL::_updatePitch()
	{
		if (!_alSource)
			return;
		alSourcef(_alSource, AL_PITCH, _pitch);
	}

	void SoundHandleOAL::_updateLoop()
	{
		if (!_alSource)
			return;
		_xupdateLoop();
	}

	void SoundHandleOAL::_setPosition(int tm)
	{
		_xsetPosition(tm);
	}

	void SoundHandleOAL::_ended()
	{
		_state = ended;
		ss()->freeSource(_alSource);
		_alSource = 0;
	}


	SoundHandleOAL* SoundHandleOAL::create(Sound* snd_)
	{
		SoundOAL* snd = (SoundOAL*)snd_;

		SoundHandleOAL* s;
		if (snd->getAlBuffer())
			s = new StaticSoundHandleOAL(snd->getAlBuffer());
		else
			s = new StreamingOggSoundHandleOAL(snd);

		s->_duration = snd->getDuration();

		return s;
	}
}