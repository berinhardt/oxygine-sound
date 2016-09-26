#pragma once
#include "core/Object.h"
#include "Channel.h"
#include "EventDispatcher.h"
#include "Event.h"

namespace oxygine
{
    class SoundPlayer;
    class Channel;

    class SoundHandle;

    DECLARE_SMART(SoundInstance, spSoundInstance);
    class SoundInstance: public Object
    {
    public:
        class SoundEvent: public Event
        {
        public:
            enum
            {
                EVENT_ABOUT_DONE = makefourcc('S', 'I', 'A', 'D'),
                EVENT_DONE = makefourcc('S', 'I', 'D', 'N')
            };
            SoundEvent(eventType tp, SoundInstance* ins): Event(tp), instance(ins) {}
            SoundInstance* instance;
        };
        SoundInstance(SoundHandle*);
        ~SoundInstance();

        enum State
        {
            FadingIn,
            FadingOutStop,
            FadingOutPause,
            Normal,
            Paused,
            Stopped,
        };

        void play();
        void pause();
        void resume();

        /**SoundInstance shouldn't be used after "stop"*/
        void stop();


        void fadeOut(int fadeOutMS);
        /**Event::COMPLETE won't be dispatched*/
        void fadeOutPause(int fadeOutMS);

        /**could be used after fadeOut with stop=false*/
        void fadeIn(int fadeInMS);

        Channel*    getChannel() const;
        int         getDuration() const;
        int         getPosition() const;
        float       getVolume() const;
        float       getPitch() const;
        State       getState() const { return _state; }
        const sound_desc& getDesc() const {return _desc;}
        bool        isPlaying() const;
        bool        isPaused() const;
        bool        isFadingOut() const;

        void    setVolume(float v);
        void    setCoord(const Vector2& pos, float z = 0);
        void    setPitch(float v);
        void    setLoop(bool loop);
        void    seek(int tm);

        /**called when sound done*/
        void    setDoneCallback(EventCallback cb) {_cbDone = cb;}
        void    setAboutDoneCallback(EventCallback cb) {_cbAboutDone = cb;}

    protected:

        friend class SoundPlayer;
        void finished();
        void update();
        void aboutDone();

    private:
        SoundPlayer* _player;
        EventCallback _cbDone;
        EventCallback _cbAboutDone;

        SoundHandle* _handle;
        Channel* _channel;
        sound_desc _desc;

        bool _finished;

        float _volume;//primary volume
        unsigned int _startTime;

        int _startFadeIn;
        int _startFadeOut;

        int _fadeInMS;
        int _fadeOutMS;


        State _state;
    };
}