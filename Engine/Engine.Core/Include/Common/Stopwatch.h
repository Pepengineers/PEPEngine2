#pragma once

#include <chrono>

class Stopwatch
{
    std::chrono::steady_clock::time_point start_time;

public:
    // Constructor starts the stopwatch automatically
    Stopwatch() : start_time(std::chrono::steady_clock::now())
    {
    }

    // Restarts the stopwatch
    void Reset()
    {
        start_time = std::chrono::steady_clock::now();
    }

    // Gets the elapsed time without stopping
    double GetElapsed() const
    {
        auto end_time = std::chrono::steady_clock::now();
        std::chrono::duration<double> elapsed = end_time - start_time;
        return elapsed.count();
    }
};
