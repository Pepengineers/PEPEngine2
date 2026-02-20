#pragma once

class GameTimer
{
public:
	GameTimer();

	float TotalTime()const; // in seconds
	float DeltaTime()const; // in seconds

	void Reset(); // Call before message loop.
	void Start(); // Call when unpaused.
	void Stop();  // Call when paused.
	void Tick();  // Call every frame.

private:
	double SecondsPerCount;
	double TimeDelta;

	__int64 BaseTime;
	__int64 PausedTime;
	__int64 StopTime;
	__int64 PrevTime;
	__int64 CurrTime;

	bool Stopped;
};