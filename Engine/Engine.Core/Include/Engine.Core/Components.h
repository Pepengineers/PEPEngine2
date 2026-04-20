#pragma once
struct TranslateComponent : ComponentTag
{
    float X = 0.0f; // SimpleMath::Vec3 is better
    float Y = 0.0f;
    float Z = 0.0f;

    TranslateComponent() = default;

    TranslateComponent(float inX, float inY, float inZ)
        : X(inX), Y(inY), Z(inZ)
    {
    }
};

struct VelocityComponent : ComponentTag
{
    float vx = 0.0f;
    float vy = 0.0f;
    float vz = 0.0f;

    VelocityComponent() = default;

    VelocityComponent(float inVx, float inVy, float inVz)
        : vx(inVx), vy(inVy), vz(inVz)
    {
    }
};

struct NameComponent : ComponentTag
{
    std::string value;

    NameComponent() = default;

    NameComponent(const std::string& inValue)
        : value(inValue)
    {
    }
};