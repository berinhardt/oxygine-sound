#include "SoundInstance.h"
#include "SoundPlayer.h"
#include "Sound.h"
#include "Channel.h"

namespace oxygine
{
    SoundInstance::SoundInstance(): _player(0),
        _channel(0),
        _startTime(0),
        _fadeInMS(0),
        _fadeOutMS(0),
        _state(FadingIn),
        _volume(1.0f),
        _startFadeIn(0),
        _startFadeOut(0)
    {

    }

    SoundInstance::~SoundInstance()
    {

    }

    void SoundInstance::finished()
    {
        _channel = 0;

        //callback would called on fadeout
        if (_fadeOutMS)
            return;

        if (_cbDone)
        {
            SoundEvent ev(SoundEvent::EVENT_DONE, this);
            _cbDone(&ev);
        }
    }

    void SoundInstance::aboutDone()
    {
        if (_cbAboutDone)
        {
            //Channel
            //Event ev(Event::)
            SoundEvent ev(SoundEvent::EVENT_ABOUT_DONE, this);
            _cbAboutDone(&ev);
        }
    }

    void SoundInstance::resume()
    {
        if (!_channel)
            return;
        _channel->resume();
    }

    void SoundInstance::stop()
    {
        if (!_channel)
            return;
        _channel->stop();
        _channel = 0;
    }

    void SoundInstance::fadeOut(int fadeOutMS, bool stop)
    {
        _startFadeOut = _player->getTime() - _startTime;
        _state = stop ? FadingOut : FadingOutPause;
        _fadeOutMS = fadeOutMS;
    }

    void SoundInstance::fadeIn(int fadeInMS)
    {
        OX_ASSERT(_channel);
        OX_ASSERT(_state == Paused);
        if (!_channel)
            return;

        _channel->resume();
        _startFadeIn = _player->getTime() - _startTime;
        _fadeInMS = fadeInMS;
        _state = FadingIn;
    }

    bool SoundInstance::isPlaying()
    {
        if (_channel)
            return true;

        return false;
    }

    float SoundInstance::getVolume() const
    {
        return _volume;
    }

    float SoundInstance::getPitch() const
    {
        return _desc.pitch;
    }

    void SoundInstance::setVolume(float v)
    {
        OX_ASSERT(v >= 0 && v <= 1.0f);
        _volume = v;

        if (_channel)
            _channel->setVolume(_volume);
    }

    void SoundInstance::setCoord(const Vector2& pos, float z)
    {
        if (_channel)
            _channel->setCoord(pos.x, pos.y, z);
    }

    void SoundInstance::setPitch(float v)
    {
        _desc.pitch = v;

        if (_channel)
            _channel->setPitch(_volume);
    }

    void SoundInstance::seek(int tm)
    {
        if (_channel)
            _channel->seek(tm);
    }

    void SoundInstance::update()
    {
        //OX_ASSERT(_channel);
        if (!_channel)
            return;

        int duration = _desc.sound->getDuration();
        unsigned int globalTime = _player->getTime();

        int soundTime = globalTime - _startTime;

        switch (_state)
        {
            case Paused:
            {

            }
            break;

            case FadingIn:
            {
                float p = (soundTime - _startFadeIn) / (float)_fadeInMS;

                if (p >= 1.0f)
                {
                    _state = Normal;
                    p = 1.0;
                }

                //printf("fade in\n");
                float volume = p * _volume;
                _channel->setVolume(volume);
            }
            break;

            case Normal:
            {
                if (!_desc.looping && soundTime >= _startFadeOut && duration != _startFadeOut)
                {
                    _state = FadingOut;
                    if (_cbDone)
                    {
                        Event ev(Event::COMPLETE);
                        _cbDone(&ev);
                    }
                }
            }
            break;

            case FadingOutPause:
            case FadingOut:
            {
                float p = (soundTime - _startFadeOut) / (float)_fadeOutMS;

                if (p > 1.0)
                {
                    if (_state == FadingOutPause)
                    {
                        _channel->setVolume(0);
                        _channel->pause();
                        _state = Paused;
                        _startFadeOut = duration;
                    }
                    else
                    {
                        _channel->stop();
                        finished();
                    }
                }
                else
                {
                    float volume = (1.0f - p) * _volume;
                    _channel->setVolume(volume);
                }
            }
            break;
        }
    }

    Channel* SoundInstance::getChannel() const
    {
        return _channel;
    }

    int SoundInstance::getDuration()const
    {
        if (!_desc.sound)
            return 0;

        return _desc.sound->getDuration();
    }

    int SoundInstance::getPosition() const
    {
        if (_channel)
            return 0;
        return _channel->getPosition();
    }
}