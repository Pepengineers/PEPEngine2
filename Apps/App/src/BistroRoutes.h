#pragma once

#include "App.Base/Systems/SplineFollowSystem.h"
#include <algorithm>
#include <array>
#include <random>
#include <stdexcept>
#include <utility>
#include <vector>

namespace BistroRoutes
{
    struct Cursor { size_t Edge = 0; float Distance = 0; bool Forward = true; float Waiting = 0; };

    struct Edge
    {
        size_t From, To;
        std::array<Vector3, 33> Points;
        std::array<float, 33> Distances;
        float Length() const { return Distances.back(); }
    };

    inline float HorizontalDistanceSquared(Vector3 a, Vector3 b)
    {
        a.y = b.y = 0;
        return Vector3::DistanceSquared(a, b);
    }

    class Network
    {
    public:
        Network(std::vector<Vector3> nodes, std::vector<std::pair<size_t, size_t>> links, float separation)
            : Nodes(std::move(nodes)), Incident(Nodes.size()), Separation(separation)
        {
            for (const auto& [from, to] : links)
            {
                Incident.at(from).push_back(Edges.size());
                Incident.at(to).push_back(Edges.size());
                Edges.push_back({from, to, {}, {}});
            }
            const auto tangent = [&](size_t node, size_t toward)
            {
                auto direction = Nodes[toward] - Nodes[node];
                const float length = direction.Length();
                if (Incident[node].size() == 2)
                    for (const auto index : Incident[node])
                    {
                        const auto& edge = Edges[index];
                        const auto other = edge.From == node ? edge.To : edge.From;
                        if (other != toward) direction = Nodes[toward] - Nodes[other];
                    }
                direction.Normalize();
                return direction * (length / 3);
            };
            for (auto& edge : Edges)
            {
                const auto a = Nodes[edge.From], b = Nodes[edge.To];
                const auto controlA = a + tangent(edge.From, edge.To);
                const auto controlB = b + tangent(edge.To, edge.From);
                for (size_t i = 0; i < edge.Points.size(); ++i)
                {
                    edge.Points[i] = EvaluateBezier(a, controlA, controlB, b, static_cast<float>(i) / 32);
                    edge.Distances[i] = i ? edge.Distances[i - 1] + Vector3::Distance(edge.Points[i - 1], edge.Points[i]) : 0;
                }
                if (edge.Length() <= 0) throw std::invalid_argument("Empty Bistro route segment");
            }
            for (size_t edge = 0; edge < Edges.size(); ++edge)
                for (float distance = separation; distance < Edges[edge].Length(); distance += separation * 1.5f)
                {
                    Cursor slot{edge, distance};
                    bool free = true;
                    for (const auto& existing : Spawns)
                        free &= HorizontalDistanceSquared(Position(slot), Position(existing)) >= separation * separation;
                    if (free) Spawns.push_back(slot);
                }
        }

        Vector3 Position(const Cursor& cursor) const
        {
            const auto& edge = Edges.at(cursor.Edge);
            const float distance = std::clamp(cursor.Distance, 0.0f, edge.Length());
            const auto index = static_cast<size_t>(std::lower_bound(edge.Distances.begin(), edge.Distances.end(), distance) - edge.Distances.begin());
            if (!index) return edge.Points.front();
            const float fraction = (distance - edge.Distances[index - 1]) / (edge.Distances[index] - edge.Distances[index - 1]);
            return Vector3::Lerp(edge.Points[index - 1], edge.Points[index], fraction);
        }

        void Advance(Cursor& cursor, float distance, std::mt19937& random) const
        {
            while (distance > 0)
            {
                const auto& edge = Edges[cursor.Edge];
                const float remaining = cursor.Forward ? edge.Length() - cursor.Distance : cursor.Distance;
                if (distance <= remaining)
                {
                    cursor.Distance += cursor.Forward ? distance : -distance;
                    return;
                }
                distance -= remaining;
                const size_t junction = cursor.Forward ? edge.To : edge.From;
                std::vector<size_t> choices;
                for (const auto next : Incident[junction])
                    if (next != cursor.Edge || Incident[junction].size() == 1) choices.push_back(next);
                cursor.Edge = choices[std::uniform_int_distribution<size_t>(0, choices.size() - 1)(random)];
                cursor.Forward = Edges[cursor.Edge].From == junction;
                cursor.Distance = cursor.Forward ? 0 : Edges[cursor.Edge].Length();
            }
        }

        std::vector<Vector3> Nodes;
        std::vector<Edge> Edges;
        std::vector<std::vector<size_t>> Incident;
        std::vector<Cursor> Spawns;
        float Separation;
    };

    inline const Network& People()
    {
        // shortcut: heights approximate uneven paving; use ground projection when precise foot contact is needed.
        static const Network network({
            {1400, 27.762f,-4500},
            {1650, 27.762f,-4000},
            {1550,23.426f,-3500},
            {1250,23.932f,-3000},
            {950,22.581f,-2400},
            {350,24.794f,-1650},
            {-250,24.897f,-1100},
            {-550,24.243f,-600},
            {-650,23.096f,-100},
            {-450,23.875f,500},
            {0,26.858f,850},
            {850,26.886f,1250},
            {1800,23.026f,1600},
            {2800,22.779f,2000},
            {3500,20.426f,2300},
            {3600,20.169f,1900},
            {4300, 27.762f,1000},
            {5500, 27.762f,900},
            {6200, 27.762f,2200},
            {5800, 27.762f,3700},
            {4300, 27.762f,3800},
            {-1300,31.944f,-1200},
            {-2200, 27.762f,-2000},
            {-2700, 27.762f,-2400},
            {3300, 27.762f,2700},
            {5800, 27.762f,2900},
            {3200, 27.762f,3400}},
            {{0,1}, {1,2}, {2,3}, {3,4}, {4,5}, {5,6}, {6,7}, {7,8}, {8,9}, {9,10}, {10,11}, {11,12}, {12,13}, {13,14}, {14,15}, {15,16}, {16,17}, {17,18}, {18,25}, {25,19}, {19,20}, {20,26}, {26,24}, {24,14}, {6,21}, {21,22}, {22,23}, {13,15}}, 110);
        return network;
    }

    inline const Network& Foxes()
    {
        static const Network network({
            {110, 39.722f,-120},
            {260, 39.722f,-320},
            {340, 39.722f,-530},
            {420, 39.722f,-650},
            {550, 39.722f,-820},
            {675, 39.722f,-1000},
            {520, 39.722f,-470},
            {450, 39.722f,-300},
            {760, 39.722f,-110},
            {1000, 39.722f,40},
            {650, 39.722f,130},
            {850, 39.722f,200},
            {400, 39.722f,20}},
            {{0,1}, {1,2}, {2,3}, {3,4}, {4,5}, {2,6}, {1,6}, {6,7}, {7,8}, {8,9}, {0,12}, {12,10}, {10,11}}, 80);
        return network;
    }
}
