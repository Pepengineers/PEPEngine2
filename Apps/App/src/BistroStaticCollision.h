#pragma once

#include "App.Base/Physics/PhysicsTypes.h"
#include "Engine.Core/Types/MeshTypes.h"
#include <algorithm>
#include <cmath>
#include <iterator>
#include <map>
#include <numeric>
#include <set>
#include <stdexcept>
#include <tuple>

namespace BistroStaticCollision
{
    using DirectX::SimpleMath::Vector3;
    using DirectX::SimpleMath::Quaternion;
    using DirectX::SimpleMath::Matrix;

    struct Result
    {
        std::vector<PhysicsCompoundChild> Boxes;
        std::shared_ptr<PhysicsConvexCompound> FloorPrisms = std::make_shared<PhysicsConvexCompound>();
        std::shared_ptr<PhysicsTriangleMesh> Remainder = std::make_shared<PhysicsTriangleMesh>();
        size_t SourceTriangles = 0;
        size_t BoxedTriangles = 0;
        size_t PrismTriangles = 0;
        size_t SliverBoxes = 0;
        size_t PrismMerges = 0;
        size_t SeamBridges = 0;
    };

    namespace Detail
    {
        using Point = std::array<int64_t, 3>;
        using Point2 = std::pair<int64_t, int64_t>;
        using Face = std::array<Point, 3>;
        struct Plane
        {
            Vector3 Normal;
            float LargestArea = 0;
            std::vector<size_t> Triangles;
        };
        struct Rectangle { int64_t Left, Bottom, Right, Top; };

        inline Point Weld(const Vector3& p)
        {
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) ||
                (std::max)({std::abs(p.x), std::abs(p.y), std::abs(p.z)}) > 1e7f)
                throw std::invalid_argument("Invalid Bistro static collision vertex");
            return {std::llround(double(p.x) * 100), std::llround(double(p.y) * 100), std::llround(double(p.z) * 100)};
        }
        inline Vector3 Position(const Point& p)
        { return {float(p[0] * .01), float(p[1] * .01), float(p[2] * .01)}; }
        inline bool RaisedFloorHeight(float y) { return std::abs(y-51.803f)<=1.f; }

        // Prism vertices are the ordered support polygon followed by its extruded copy.
        inline bool VerticalFootprint(const std::vector<Vector3>& hull, std::vector<Point>& footprint)
        {
            if (hull.size()<6 || hull.size()%2) return false;
            const size_t count=hull.size()/2;
            const auto top=Weld(hull[0]), bottom=Weld(hull[count]);
            if (top[1]==bottom[1]) return false;
            for (size_t i=0;i<count;++i)
            {
                const auto a=Weld(hull[i]), b=Weld(hull[i+count]);
                if (a[1]!=top[1] || b[1]!=bottom[1] || a[0]!=b[0] || a[2]!=b[2]) return false;
                footprint.push_back(a);
            }
            return true;
        }

        inline double CrossXZ(const Point& a, const Point& b, const Point& c)
        { return double(b[0]-a[0])*(c[2]-a[2])-double(b[2]-a[2])*(c[0]-a[0]); }

        inline double AreaXZ(const std::vector<Point>& polygon)
        {
            double area=0;
            for (size_t i=1;i+1<polygon.size();++i) area+=CrossXZ(polygon[0],polygon[i],polygon[i+1]);
            return area;
        }

        inline bool MergePrismPair(const std::vector<Vector3>& a, const std::vector<Point>& ap,
            const std::vector<Vector3>& b, const std::vector<Point>& bp, std::vector<Vector3>& merged)
        {
            if (ap[0][1]!=bp[0][1] || Weld(a[ap.size()])[1]!=Weld(b[bp.size()])[1] || AreaXZ(ap)*AreaXZ(bp)<=0)
                return false;
            std::map<Point,Vector3> points;
            for (size_t i=0;i<ap.size();++i) points.emplace(ap[i],a[i]);
            for (size_t i=0;i<bp.size();++i) points.emplace(bp[i],b[i]);
            std::vector<Point> boundary;
            for (const auto& [p,position] : points)
            {
                while (boundary.size()>1 && CrossXZ(boundary[boundary.size()-2],boundary.back(),p)<=0) boundary.pop_back();
                boundary.push_back(p);
            }
            const size_t lower=boundary.size();
            for (auto it=std::next(points.rbegin());it!=points.rend();++it)
            {
                while (boundary.size()>lower && CrossXZ(boundary[boundary.size()-2],boundary.back(),it->first)<=0) boundary.pop_back();
                boundary.push_back(it->first);
            }
            boundary.pop_back();
            if (boundary.size()<3 || boundary.size()>16) return false;
            // Weld units are 0.01 cm; twice one square centimeter is 20,000 units.
            // Opposite shared edges keep both convex inputs on different sides, so their areas cannot overlap.
            if (std::abs(std::abs(AreaXZ(boundary))-std::abs(AreaXZ(ap))-std::abs(AreaXZ(bp)))>20000) return false;
            if (AreaXZ(ap)<0) std::reverse(boundary.begin(),boundary.end());
            const float extrusion=a[ap.size()].y-a[0].y;
            for (const auto& point : boundary) merged.push_back(points.at(point));
            for (const auto& point : boundary) merged.push_back(points.at(point)+Vector3(0,extrusion,0));
            return true;
        }

        inline size_t MergeFloorPrisms(std::vector<std::vector<Vector3>>& hulls)
        {
            size_t merges=0;
            bool changed=true;
            while (changed)
            {
                changed=false;
                std::map<std::pair<Point,Point>,size_t> edges;
                std::vector<std::vector<Point>> footprints(hulls.size());
                for (size_t i=0;i<hulls.size();++i)
                {
                    auto& polygon=footprints[i];
                    if (!VerticalFootprint(hulls[i],polygon)) continue;
                    for (size_t edge=0;edge<polygon.size();++edge)
                    {
                        const auto a=polygon[edge], b=polygon[(edge+1)%polygon.size()];
                        const auto opposite=edges.find({b,a});
                        if (opposite!=edges.end() && opposite->second!=i)
                        {
                            const auto other=opposite->second;
                            std::vector<Vector3> merged;
                            if (MergePrismPair(hulls[i],polygon,hulls[other],footprints[other],merged))
                            {
                                hulls[i]=std::move(merged); hulls[other].clear();
                                ++merges; changed=true; break;
                            }
                        }
                        edges.emplace(std::make_pair(a,b),i);
                    }
                    if (changed) break;
                }
            }
            hulls.erase(std::remove_if(hulls.begin(),hulls.end(),[](const auto& hull) { return hull.empty(); }),hulls.end());
            return merges;
        }

        inline size_t BridgeFloorPrisms(std::vector<std::vector<Vector3>>& hulls)
        {
            struct Surface { Vector3 Center; float Extrusion=0; bool Valid=false; };
            const size_t count=hulls.size();
            std::vector<Surface> surfaces(count);
            for (size_t i=0;i<count;++i)
            {
                const auto& hull=hulls[i];
                if (hull.size()<6 || hull.size()%2) continue;
                const size_t vertices=hull.size()/2;
                auto& surface=surfaces[i];
                surface.Extrusion=hull[vertices].y-hull[0].y;
                bool base=true, raised=true, vertical=surface.Extrusion<0;
                for (size_t j=0;j<vertices;++j)
                {
                    base &= std::abs(hull[j].y-39.622f)<=1.f;
                    raised &= RaisedFloorHeight(hull[j].y);
                    const auto delta=hull[j+vertices]-hull[j];
                    vertical &= std::abs(delta.x)<.01f && std::abs(delta.z)<.01f && std::abs(delta.y-surface.Extrusion)<.01f;
                    surface.Center+=hull[j]/float(vertices);
                }
                surface.Valid=(base || raised) && vertical;
            }
            std::map<std::pair<Point,Point>,size_t> edges;
            std::set<std::pair<Point,Point>> bridged;
            for (size_t i=0;i<count;++i)
            {
                if (!surfaces[i].Valid) continue;
                // Copy before appending: hulls may reallocate while bridges are added.
                const auto hull=hulls[i];
                const size_t vertices=hull.size()/2;
                for (size_t j=0;j<vertices;++j)
                {
                    const auto a=hull[j], b=hull[(j+1)%vertices];
                    const auto ak=Weld(a), bk=Weld(b);
                    const auto opposite=edges.find({bk,ak});
                    edges.emplace(std::make_pair(ak,bk),i);
                    if (opposite==edges.end() || opposite->second==i) continue;
                    const auto other=opposite->second;
                    if (std::abs(surfaces[i].Extrusion-surfaces[other].Extrusion)>.01f) continue;
                    const auto key=ak<bk ? std::make_pair(ak,bk) : std::make_pair(bk,ak);
                    if (bridged.count(key)) continue;
                    const auto midpoint=(a+b)*.5f;
                    const Vector3 edge(b.x-a.x,0,b.z-a.z);
                    if (edge.LengthSquared()<.0001f) continue;
                    const auto interior=[&](const Vector3& center)
                    {
                        const auto delta=center-midpoint;
                        const float projection=std::abs(delta.Dot(edge)/edge.LengthSquared());
                        return midpoint+delta*(projection>.25f ? .25f/projection : 1.f);
                    };
                    const auto c=interior(surfaces[i].Center), d=interior(surfaces[other].Center);
                    if ((std::max)({a.y,b.y,c.y,d.y})-(std::min)({a.y,b.y,c.y,d.y})>1.f) continue;
                    // Both halves of this solid diamond lie inside a convex neighbor; it covers the
                    // internal side-contact seam without extending the floor footprint or closing holes.
                    std::vector<Vector3> bridge{a,d,b,c};
                    for (size_t k=0;k<4;++k) bridge.push_back(bridge[k]+Vector3(0,surfaces[i].Extrusion,0));
                    hulls.push_back(std::move(bridge));
                    bridged.insert(key);
                }
            }
            return hulls.size()-count;
        }

        inline bool Rectangles(const std::vector<Face>& faces, const Vector3& normal,
            std::vector<Rectangle>& rectangles, Vector3& origin, Vector3& u, Vector3& v)
        {
            struct EdgeUse { unsigned Count = 0; int Direction = 0; };
            std::map<std::pair<Point, Point>, EdgeUse> edges;
            for (auto face : faces)
            {
                if ((Position(face[1])-Position(face[0])).Cross(Position(face[2])-Position(face[0])).Dot(normal) < 0)
                    std::swap(face[1], face[2]);
                for (unsigned i = 0; i < 3; ++i)
                {
                    auto a = face[i], b = face[(i+1)%3];
                    if (a == b) return false;
                    const int direction = a < b ? 1 : -1;
                    if (direction < 0) std::swap(a,b);
                    auto& edge = edges[{a,b}];
                    if (++edge.Count > 2) return false;
                    edge.Direction += direction;
                }
            }
            std::vector<std::pair<Point, Point>> boundary;
            std::map<Point, std::pair<unsigned, unsigned>> degrees;
            float longest = 0;
            for (const auto& [edge, use] : edges)
            {
                if (use.Count == 2) { if (use.Direction != 0) return false; continue; }
                auto a = edge.first, b = edge.second;
                if (use.Direction < 0) std::swap(a,b);
                ++degrees[a].first; ++degrees[b].second;
                boundary.push_back({a,b});
                auto delta = Position(b)-Position(a);
                delta -= normal * delta.Dot(normal);
                if (delta.LengthSquared() > longest)
                {
                    longest = delta.LengthSquared();
                    origin = Position(a); u = delta;
                }
            }
            if (longest <= 0 || boundary.empty()) return false;
            for (const auto& [point, degree] : degrees)
                if (degree.first != 1 || degree.second != 1) return false;
            u.Normalize(); v = normal.Cross(u); v.Normalize();
            const auto project = [&](const Point& point)
            {
                const auto p = Position(point)-origin;
                return Point2{std::llround(double(p.Dot(u))*10), std::llround(double(p.Dot(v))*10)};
            };
            std::vector<std::pair<Point2, Point2>> outline;
            std::set<int64_t> rows;
            for (const auto& edge : boundary)
            {
                const auto a = project(edge.first), b = project(edge.second);
                if (a == b || (a.first != b.first && a.second != b.second)) return false;
                outline.push_back({a,b});
                rows.insert(a.second); rows.insert(b.second);
            }
            // shortcut: intricate outlines stay meshes; use a sweep tree if these become a measured bottleneck.
            if (rows.size() * boundary.size() > 4000000) return false;
            double triangleArea2 = 0;
            for (const auto& face : faces)
            {
                const auto a=project(face[0]), b=project(face[1]), c=project(face[2]);
                triangleArea2 += std::abs(double(b.first-a.first)*(c.second-a.second)-double(b.second-a.second)*(c.first-a.first));
            }
            std::map<std::pair<int64_t,int64_t>, size_t> previous;
            double rectangleArea2 = 0;
            for (auto row=rows.begin(), next=row; row!=rows.end() && ++next!=rows.end(); row=next)
            {
                const double middle = (double(*row)+*next)*.5;
                std::map<int64_t,int> crossings;
                for (const auto& [a,b] : outline)
                    if (a.first == b.first && middle > (std::min)(a.second,b.second) && middle < (std::max)(a.second,b.second))
                        crossings[a.first] += b.second < a.second ? 1 : -1;
                std::map<std::pair<int64_t,int64_t>, size_t> current;
                int winding = 0;
                for (auto edge=crossings.begin(), right=edge; edge!=crossings.end(); edge=right)
                {
                    winding += edge->second;
                    if (winding < 0) return false;
                    right = std::next(edge);
                    if (right == crossings.end()) { if (winding != 0) return false; break; }
                    if (!winding) continue;
                    const auto span=std::make_pair(edge->first,right->first);
                    rectangleArea2 += 2*double(span.second-span.first)*(*next-*row);
                    const auto existing=previous.find(span);
                    if (existing != previous.end())
                    {
                        rectangles[existing->second].Top=*next;
                        current.emplace(span,existing->second);
                    }
                    else
                    {
                        current.emplace(span,rectangles.size());
                        rectangles.push_back({span.first,*row,span.second,*next});
                    }
                }
                previous=std::move(current);
            }
            return !rectangles.empty() && std::abs(triangleArea2-rectangleArea2) <= (std::max)(1.0,triangleArea2*1e-9);
        }
    }

    inline Result Build(const Engine::Core::Mesh& mesh, const std::vector<uint32_t>& selected)
    {
        using namespace Detail;
        Result result;
        // Rounded independent floor pieces leave valleys along their internal contact seams.
        result.FloorPrisms->ConvexRadiusMeters=0;
        std::map<std::array<int64_t,4>, Plane> planes;
        std::vector<std::array<uint32_t,3>> triangles;
        for (const auto index : selected)
        {
            if (index >= mesh.GetSubMeshCount()) throw std::invalid_argument("Invalid Bistro static submesh");
            const auto& part=mesh.GetSubMesh(index);
            if (part.Indices.size()%3 || part.Vertices.size()>UINT32_MAX-result.Remainder->Vertices.size())
                throw std::invalid_argument("Invalid Bistro static triangle count");
            const auto offset=static_cast<uint32_t>(result.Remainder->Vertices.size());
            for (const auto& vertex : part.Vertices) { Weld(vertex.Position); result.Remainder->Vertices.push_back(vertex.Position); }
            for (size_t i=0; i<part.Indices.size(); i+=3)
            {
                std::array<uint32_t,3> triangle;
                for (unsigned j=0; j<3; ++j)
                {
                    if (part.Indices[i+j]>=part.Vertices.size()) throw std::invalid_argument("Invalid Bistro static triangle index");
                    triangle[j]=offset+part.Indices[i+j];
                }
                triangles.push_back(triangle);
                const auto a=result.Remainder->Vertices[triangle[0]], b=result.Remainder->Vertices[triangle[1]], c=result.Remainder->Vertices[triangle[2]];
                auto normal=(b-a).Cross(c-a);
                const float area=normal.Length();
                if (!std::isfinite(area)) throw std::invalid_argument("Bistro static triangle overflow");
                if (area<=.0001f) continue;
                normal/=area;
                const bool lowFloor=normal.y>.5f && RaisedFloorHeight(a.y) && RaisedFloorHeight(b.y) && RaisedFloorHeight(c.y);
                if (std::abs(normal.y)>.99999f)
                {
                    if (!lowFloor || (std::max)({a.y,b.y,c.y})-(std::min)({a.y,b.y,c.y})<=.01f) normal=Vector3::UnitY;
                }
                else if (lowFloor) {} // Preserve the authored slope of low floor bevels.
                else if (std::abs(normal.y)<.0001f)
                {
                    normal.y=0; normal.Normalize();
                    if (normal.x<0 || (normal.x==0 && normal.z<0)) normal=-normal;
                }
                else continue;
                const std::array<int64_t,4> key{std::llround(normal.x*1000),std::llround(normal.y*1000),
                    std::llround(normal.z*1000),std::llround(double(normal.Dot(a))*10)};
                auto& plane=planes[key];
                if (area>plane.LargestArea) { plane.Normal=normal; plane.LargestArea=area; }
                plane.Triangles.push_back(triangles.size()-1);
            }
        }
        result.SourceTriangles=triangles.size();
        std::vector<uint8_t> replacement(triangles.size(),0);
        for (const auto& [key,plane] : planes)
        {
            std::map<Face,size_t> unique;
            std::vector<Face> faces;
            std::vector<size_t> representatives;
            std::vector<std::vector<size_t>> duplicates;
            for (const auto index : plane.Triangles)
            {
                Face face;
                for (unsigned j=0;j<3;++j) face[j]=Weld(result.Remainder->Vertices[triangles[index][j]]);
                std::sort(face.begin(),face.end());
                const auto [found,inserted]=unique.emplace(face,faces.size());
                const auto& t=triangles[index];
                const bool upward=(result.Remainder->Vertices[t[1]]-result.Remainder->Vertices[t[0]]).Cross(
                    result.Remainder->Vertices[t[2]]-result.Remainder->Vertices[t[0]]).y>0;
                if (inserted)
                {
                    faces.push_back(face); representatives.push_back(index); duplicates.emplace_back();
                }
                else if (upward) representatives[found->second]=index;
                duplicates[found->second].push_back(index);
            }

            std::vector<size_t> parents(faces.size());
            std::iota(parents.begin(),parents.end(),size_t(0));
            const auto root=[&](size_t index)
            {
                while (parents[index]!=index)
                {
                    parents[index]=parents[parents[index]];
                    index=parents[index];
                }
                return index;
            };
            std::map<std::pair<Point,Point>,size_t> edgeOwners;
            for (size_t i=0;i<faces.size();++i) for (unsigned edge=0;edge<3;++edge)
            {
                auto a=faces[i][edge], b=faces[i][(edge+1)%3];
                if (b<a) std::swap(a,b);
                const auto [owner,inserted]=edgeOwners.emplace(std::make_pair(a,b),i);
                if (!inserted) parents[root(i)]=root(owner->second);
            }
            std::map<size_t,std::vector<size_t>> patches;
            for (size_t i=0;i<faces.size();++i) patches[root(i)].push_back(i);
            for (const auto& [patchRoot,patch] : patches)
            {
                std::vector<Face> patchFaces;
                bool up=false, down=false, raisedFloorHeight=true, coplanar=true;
                const auto planePoint=result.Remainder->Vertices[triangles[representatives[patch.front()]][0]];
                double area=0, upArea=0, downArea=0;
                for (const auto index : patch)
                {
                    patchFaces.push_back(faces[index]);
                    const auto& t=triangles[representatives[index]];
                    for (const auto vertex : t)
                    {
                        const auto& point=result.Remainder->Vertices[vertex];
                        raisedFloorHeight &= RaisedFloorHeight(point.y);
                        coplanar &= std::abs((point-planePoint).Dot(plane.Normal))<=.01f;
                    }
                    const auto normal=(result.Remainder->Vertices[t[1]]-result.Remainder->Vertices[t[0]]).Cross(
                        result.Remainder->Vertices[t[2]]-result.Remainder->Vertices[t[0]]);
                    up |= normal.y>0; down |= normal.y<0;
                    area+=normal.Length()*.5;
                    if (normal.y>0) upArea+=normal.Length()*.5;
                    else if (normal.y<0) downArea+=normal.Length()*.5;
                }
                bool repairOrientation=false;
                if (plane.Normal.y>.5f && up && down)
                {
                    // Near-collinear winding noise must not turn a whole floor into a one-sided mesh.
                    const bool dominantUp=upArea>downArea;
                    repairOrientation=(std::min)(upArea,downArea)<=1.0;
                    for (const auto index : patch)
                    {
                        const auto& t=triangles[representatives[index]];
                        const auto a=result.Remainder->Vertices[t[0]], b=result.Remainder->Vertices[t[1]], c=result.Remainder->Vertices[t[2]];
                        const auto normal=(b-a).Cross(c-a);
                        if ((normal.y>0)==dominantUp) continue;
                        const float longest=(std::max)({(b-a).Length(),(c-b).Length(),(a-c).Length()});
                        if (longest<=0 || normal.Length()/longest>.01f) repairOrientation=false;
                    }
                    // Repair only minority area <=1 cm squared and altitude below the weld resolution.
                    // Substantive floor/ceiling disagreement retains authored mesh sidedness.
                    if (!repairOrientation) continue;
                    up=dominantUp; down=!dominantUp;
                }
                std::vector<Rectangle> rectangles;
                Vector3 origin,u,v;
                uint8_t kind=0;
                if (coplanar && !(raisedFloorHeight && plane.Normal.y>.5f && (plane.Normal.x!=0 || plane.Normal.z!=0)) &&
                    Rectangles(patchFaces,plane.Normal,rectangles,origin,u,v))
                {
                    const Matrix basis(u.x,u.y,u.z,0,v.x,v.y,v.z,0,plane.Normal.x,plane.Normal.y,plane.Normal.z,0,0,0,0,1);
                    const auto rotation=Quaternion::CreateFromRotationMatrix(basis);
                    for (const auto& rect : rectangles)
                    {
                        const auto center=origin+u*float((rect.Left+rect.Right)*.05)+v*float((rect.Bottom+rect.Top)*.05);
                        const bool slab = plane.Normal.y>.5f && (origin.y < 60 || origin.y > 390);
                        const float halfThickness = slab ? 10.f : 2.5f;
                        const auto offset=plane.Normal.y>.5f ? plane.Normal*(up ? -halfThickness : halfThickness) : Vector3::Zero;
                        result.Boxes.push_back({BoxShapeDesc{{float((rect.Right-rect.Left)*.05),float((rect.Top-rect.Bottom)*.05),halfThickness},0},center+offset,rotation});
                    }
                    kind=1;
                }
                else if (plane.Normal.y>.5f && (area>=10000 || (raisedFloorHeight && up && !down)))
                {
                    for (const auto index : patch)
                    {
                        const auto& t=triangles[representatives[index]];
                        auto a=result.Remainder->Vertices[t[0]], b=result.Remainder->Vertices[t[1]], c=result.Remainder->Vertices[t[2]];
                        auto normal=(b-a).Cross(c-a);
                        if (normal.LengthSquared()<=.00000001f) continue;
                        if (repairOrientation && ((normal.y>0)!=up)) { std::swap(b,c); normal=-normal; }
                        const float area2=normal.Length();
                        normal.Normalize();
                        const float halfThickness = a.y < 60 || a.y > 390 ? 10.f : 2.5f;
                        const auto thickness=raisedFloorHeight && up ? Vector3(0,-2*halfThickness,0) : normal*(-2*halfThickness);
                        const std::array<Vector3,3> points{a,b,c};
                        unsigned longestEdge=0;
                        for (unsigned edge=1;edge<3;++edge)
                            if ((points[(edge+1)%3]-points[edge]).LengthSquared()>
                                (points[(longestEdge+1)%3]-points[longestEdge]).LengthSquared()) longestEdge=edge;
                        auto axis=points[(longestEdge+1)%3]-points[longestEdge];
                        const float length=axis.Length(), altitude=area2/length;
                        uint8_t triangleKind=2;
                        if (altitude<.5f && (!raisedFloorHeight || normal.y>.99999f))
                        {
                            // Authored submillimeter floor slivers fail convex-hull cooking. A narrow box
                            // keeps their support, with at most 0.5 cm extra footprint and no missing seam.
                            axis/=length;
                            const auto side=normal.Cross(axis);
                            const Matrix basis(axis.x,axis.y,axis.z,0,side.x,side.y,side.z,0,
                                normal.x,normal.y,normal.z,0,0,0,0,1);
                            const float width=(std::max)(.1f,altitude);
                            const auto center=points[longestEdge]+axis*(length*.5f)+side*(altitude*.5f)+thickness*.5f;
                            result.Boxes.push_back({BoxShapeDesc{{length*.5f,width*.5f,halfThickness},0},center,
                                Quaternion::CreateFromRotationMatrix(basis)});
                            ++result.SliverBoxes;
                            triangleKind=1;
                        }
                        else result.FloorPrisms->Hulls.push_back({a,b,c,a+thickness,b+thickness,c+thickness});
                        for (const auto source : duplicates[index]) replacement[source]=triangleKind;
                    }
                }
                if (kind) for (const auto index : patch)
                    for (const auto source : duplicates[index]) replacement[source]=kind;
            }
        }
        for (size_t i=0;i<triangles.size();++i)
            if (replacement[i]==1) ++result.BoxedTriangles;
            else if (replacement[i]==2) ++result.PrismTriangles;
            else result.Remainder->Triangles.push_back(triangles[i]);
        std::vector<uint32_t> remap(result.Remainder->Vertices.size(),UINT32_MAX);
        std::vector<Vector3> vertices;
        vertices.reserve((std::min)(result.Remainder->Vertices.size(),result.Remainder->Triangles.size()*3));
        for (auto& triangle : result.Remainder->Triangles) for (auto& index : triangle)
        {
            auto& mapped=remap[index];
            if (mapped==UINT32_MAX)
            {
                mapped=static_cast<uint32_t>(vertices.size());
                vertices.push_back(result.Remainder->Vertices[index]);
            }
            index=mapped;
        }
        result.Remainder->Vertices=std::move(vertices);
        result.PrismMerges=MergeFloorPrisms(result.FloorPrisms->Hulls);
        result.SeamBridges=BridgeFloorPrisms(result.FloorPrisms->Hulls);
        return result;
    }
}
