#include <windows.h>
#include <Engine.Core/GameTimer.h>

GameTimer::GameTimer ()
	: _secondsPerCount(0.0)
	, _timeDelta(-1.0)
	, _baseTime(0)
	, _pausedTime(0)
	, _stopTime(0)
	, _prevTime(0)
	, _currTime(0)
	, _bStopped(false)
{
	__int64 CountsPerSec;
	QueryPerformanceFrequency((LARGE_INTEGER*)&CountsPerSec);
	_secondsPerCount = 1.0 / static_cast<double>(CountsPerSec);
}

// Returns the total time elapsed since Reset() was called, NOT counting any
// time when the clock is stopped.
float GameTimer::TotalTime () const
{
	// If we are stopped, do not count the time that has passed since we stopped.
	// Moreover, if we previously already had a pause, the distance 
	// mStopTime - mBaseTime includes paused time, which we do not want to count.
	// To correct this, we can subtract the paused time from mStopTime:  
	//
	//                     |<--paused time-->|
	// ----*---------------*-----------------*------------*------------*------> time
	//  mBaseTime       mStopTime        startTime     mStopTime    mCurrTime

	if (_bStopped)
	{
		return static_cast<float>(((_stopTime - _pausedTime) - _baseTime) * _secondsPerCount);
	}

	// The distance mCurrTime - mBaseTime includes paused time,
	// which we do not want to count.  To correct this, we can subtract 
	// the paused time from mCurrTime:  
	//
	//  (mCurrTime - mPausedTime) - mBaseTime 
	//
	//                     |<--paused time-->|
	// ----*---------------*-----------------*------------*------> time
	//  mBaseTime       mStopTime        startTime     mCurrTime
	
	else
	{
		return static_cast<float>(((_currTime - _pausedTime) - _baseTime) * _secondsPerCount);
	}
}

float GameTimer::DeltaTime () const
{
	return static_cast<float>(_timeDelta);
}

void GameTimer::Reset ()
{
	__int64 CurrTime;
	QueryPerformanceCounter((LARGE_INTEGER*)&CurrTime);

	_baseTime = CurrTime;
	_prevTime = CurrTime;
	_stopTime = 0;
	_bStopped = false;
}

void GameTimer::Start ()
{
	__int64 StartTime;
	QueryPerformanceCounter((LARGE_INTEGER*)&StartTime);


	// Accumulate the time elapsed between stop and start pairs.
	//
	//                     |<-------d------->|
	// ----*---------------*-----------------*------------> time
	//  mBaseTime       mStopTime        startTime     

	if (_bStopped)
	{
		_pausedTime += (StartTime - _stopTime);

		_prevTime = StartTime;
		_stopTime = 0;
		_bStopped = false;
	}
}

void GameTimer::Stop ()
{
	if (!_bStopped)
	{
		__int64 CurrTime;
		QueryPerformanceCounter((LARGE_INTEGER*)&CurrTime);

		_stopTime = CurrTime;
		_bStopped = true;
	}
}

void GameTimer::Tick ()
{
	if (_bStopped)
	{
		_timeDelta = 0.0;
		return;
	}

	__int64 CurrTime;
	QueryPerformanceCounter((LARGE_INTEGER*)&CurrTime);
	_currTime = CurrTime;

	// Time difference between this frame and the previous.
	_timeDelta = (_currTime - _prevTime) * _secondsPerCount;

	// Prepare for next frame.
	_prevTime = _currTime;

	// Force nonnegative.  The DXSDK's CDXUTTimer mentions that if the 
	// processor goes into a power save mode or we get shuffled to another
	// processor, then mDeltaTime can be negative.
	if (_timeDelta < 0.0)
	{
		_timeDelta = 0.0;
	}
}