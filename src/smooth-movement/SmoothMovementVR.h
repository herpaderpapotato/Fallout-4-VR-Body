#pragma once

#include <deque>

#include "common/CommonUtils.h"

namespace frik
{
    class SmoothMovementVR
    {
    public:
        SmoothMovementVR()
        {
            QueryPerformanceFrequency(&_hpcFrequency);
            QueryPerformanceCounter(&_prevTime);
        }

        void reset();
        void onFrameUpdate();
#ifdef FRIK_FLAT // C17: the offset this frame applied (newPos - curPos; 0 after a reset) without the HMD height offset:
                 // fo4flatvr adds it to the camera it builds (flat has no PlayerWorldNode under the camera)
        const RE::NiPoint3& flatOffset() const { return _flatOffset; }
#endif

    private:
        void resetState();
        void seedCurrentPosition(const RE::NiPoint3& curPos);
        void resetFrameTimer();
        static void clearAppliedPlayerWorldOffset();

        RE::NiPoint3 smoothedValue(const RE::NiPoint3& curPos, const RE::NiPoint3& prevPos);

        bool _notMoving = false;
        std::deque<RE::NiPoint3> _lastPositions;
        RE::NiPoint3 _smoothedPos{ 0.0f, 0.0f, 0.0f };
        bool _hasSmoothedPosition = false;
        float _lastAppliedLocalX = 0;
        float _lastAppliedLocalY = 0;

        LARGE_INTEGER _hpcFrequency;
        LARGE_INTEGER _prevTime;
        float _frameTime = 0;
#ifdef FRIK_FLAT // C17
        RE::NiPoint3 _flatOffset{ 0.0f, 0.0f, 0.0f };
#endif
    };
}
