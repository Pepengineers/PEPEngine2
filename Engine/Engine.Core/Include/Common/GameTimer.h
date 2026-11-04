#pragma once

class GameTimer
{
private:
	double _secondsPerCount;
	double _timeDelta;

	__int64 _baseTime;
	__int64 _pausedTime;
	__int64 _stopTime;
	__int64 _prevTime;
	__int64 _currTime;

	bool _bStopped;

public:
	GameTimer ();

	float TotalTime () const; // in seconds
	float DeltaTime () const; // in seconds

	void Reset (); // Call before message loop.
	void Start (); // Call when unpaused.
	void Stop ();  // Call when paused.
	void Tick (double fixedDeltaSeconds = -1); // Optional fixed frame step; TotalTime stays wall-clock.
};