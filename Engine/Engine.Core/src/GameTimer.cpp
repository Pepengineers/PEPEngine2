#include <windows.h>
#include <Engine.Core/GameTimer.h>

GameTimer::GameTimer()
: SecondsPerCount(0.0), TimeDelta(-1.0), BaseTime(0),
  PausedTime(0), PrevTime(0), CurrTime(0), Stopped(false)
{
	__int64 countsPerSec;
	QueryPerformanceFrequency((LARGE_INTEGER*)&countsPerSec);
	SecondsPerCount = 1.0 / (double)countsPerSec;
}

// Returns the total time elapsed since Reset() was called, NOT counting any
// time when the clock is stopped.
float GameTimer::TotalTime()const
{
	// If we are stopped, do not count the time that has passed since we stopped.
	// Moreover, if we previously already had a pause, the distance 
	// mStopTime - mBaseTime includes paused time, which we do not want to count.
	// To correct this, we can subtract the paused time from mStopTime:  
	//
	//                     |<--paused time-->|
	// ----*---------------*-----------------*------------*------------*------> time
	//  mBaseTime       mStopTime        startTime     mStopTime    mCurrTime

	if(Stopped)
	{
		return (float)(((StopTime - PausedTime)-BaseTime)*SecondsPerCount);
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
		return (float)(((CurrTime-PausedTime)-BaseTime)*SecondsPerCount);
	}
}

float GameTimer::DeltaTime()const
{
	return (float)TimeDelta;
}

void GameTimer::Reset()
{
	__int64 currTime;
	QueryPerformanceCounter((LARGE_INTEGER*)&currTime);

	BaseTime = currTime;
	PrevTime = currTime;
	StopTime = 0;
	Stopped  = false;
}

void GameTimer::Start()
{
	__int64 startTime;
	QueryPerformanceCounter((LARGE_INTEGER*)&startTime);


	// Accumulate the time elapsed between stop and start pairs.
	//
	//                     |<-------d------->|
	// ----*---------------*-----------------*------------> time
	//  mBaseTime       mStopTime        startTime     

	if(Stopped)
	{
		PausedTime += (startTime - StopTime);	

		PrevTime = startTime;
		StopTime = 0;
		Stopped  = false;
	}
}

void GameTimer::Stop()
{
	if(!Stopped)
	{
		__int64 currTime;
		QueryPerformanceCounter((LARGE_INTEGER*)&currTime);

		StopTime = currTime;
		Stopped  = true;
	}
}

void GameTimer::Tick()
{
	if( Stopped )
	{
		TimeDelta = 0.0;
		return;
	}

	__int64 currTime;
	QueryPerformanceCounter((LARGE_INTEGER*)&currTime);
	CurrTime = currTime;

	// Time difference between this frame and the previous.
	TimeDelta = (CurrTime - PrevTime)*SecondsPerCount;

	// Prepare for next frame.
	PrevTime = CurrTime;

	// Force nonnegative.  The DXSDK's CDXUTTimer mentions that if the 
	// processor goes into a power save mode or we get shuffled to another
	// processor, then mDeltaTime can be negative.
	if(TimeDelta < 0.0) TimeDelta = 0.0;
}