#pragma once

#include "../src/BistroStaticCollision.h"
#include <limits>

template<class Check>
inline void CheckBistroStaticCollision(Check&& require)
{
    using namespace DirectX::SimpleMath;
    using namespace Engine::Core;
    using BistroStaticCollision::Build;
    const auto addQuad=[](SubMesh& part, Vector3 a, Vector3 b, Vector3 c, Vector3 d)
    {
        const auto base=static_cast<uint32_t>(part.Vertices.size());
        for (const auto p : {a,b,c,d}) { Vertex vertex; vertex.Position=p; part.Vertices.push_back(vertex); }
        part.Indices.insert(part.Indices.end(),{base,base+1,base+2,base,base+2,base+3});
    };
    const auto contains=[](const BistroStaticCollision::Result& shape, Vector3 point)
    {
        for (const auto& child : shape.Boxes)
        {
            Quaternion inverse; child.Rotation.Conjugate(inverse);
            const auto p=Vector3::Transform(point-child.Position,inverse);
            const auto half=std::get<BoxShapeDesc>(child.Shape).HalfExtent;
            if (std::abs(p.x)<=half.x+.01f && std::abs(p.y)<=half.y+.01f && std::abs(p.z)<=half.z+.01f) return true;
        }
        for (const auto& hull : shape.FloorPrisms->Hulls)
        {
            const size_t count=hull.size()/2;
            const auto a=hull[0], b=hull[1], c=hull[2];
            auto normal=(b-a).Cross(c-a); normal.Normalize();
            const float depth=(point-a).Dot(normal);
            if (depth>.01f || depth < (hull[count]-a).Dot(normal)-.01f) continue;
            const auto extrusion=hull[count]-a;
            const auto p=point-extrusion*(depth/extrusion.Dot(normal));
            bool inside=true;
            for (size_t i=0;i<count;++i)
                inside &= (hull[(i+1)%count]-hull[i]).Cross(p-hull[i]).Dot(normal)>=-.01f;
            if (inside) return true;
        }
        return false;
    };
    for (float yaw : {0.f, .49741884f})
    {
        const auto rotation=Quaternion::CreateFromYawPitchRoll(yaw,0,0);
        const Vector3 offset(1000,40,-500);
        const auto point=[&](float x,float y) { return Vector3::Transform({x,y,0},rotation)+offset; };
        SubMesh wall;
        // Matching seams are split at the lintel height; the bottom center is an actual opening.
        addQuad(wall,point(0,0),point(100,0),point(100,200),point(0,200));
        addQuad(wall,point(200,0),point(300,0),point(300,200),point(200,200));
        addQuad(wall,point(0,200),point(100,200),point(100,300),point(0,300));
        addQuad(wall,point(100,200),point(200,200),point(200,300),point(100,300));
        addQuad(wall,point(200,200),point(300,200),point(300,300),point(200,300));
        const auto shape=Build(Mesh({wall},{}),{0});
        require(shape.BoxedTriangles==10 && shape.Remainder->Triangles.empty(),"Rectangular wall with doorway was not boxed");
        require(contains(shape,point(50,100)) && contains(shape,point(250,100)) && contains(shape,point(150,250)),
            "Wall boxes lost jamb or lintel support");
        require(!contains(shape,point(150,100)),"Wall box filled the doorway");
        require(!contains(shape,point(150,199)),"Wall box narrowed the doorway height");
    }
    SubMesh floor;
    // Two slabs stay disconnected, and thickness is behind the upward-facing surface.
    addQuad(floor,{0,20,0},{0,20,100},{100,20,100},{100,20,0});
    addQuad(floor,{200,20,0},{200,20,100},{300,20,100},{300,20,0});
    auto slabs=Build(Mesh({floor},{}),{0});
    require(slabs.Boxes.size()==2 && slabs.BoxedTriangles==4,"Separate floor slabs were not boxed independently");
    for (const auto& child : slabs.Boxes)
        require(std::get<BoxShapeDesc>(child.Shape).ConvexRadiusMeters==0,
            "Static floor boxes introduced rounded seams between adjacent supports");
    require(contains(slabs,{50,19,50}) && !contains(slabs,{50,21,50}),"Floor thickness moved the supporting surface");
    require(!contains(slabs,{150,19,50}),"Floor boxes bridged an authored gap");
    auto duplicate=floor;
    for (size_t i=0;i<floor.Indices.size();i+=3)
        duplicate.Indices.insert(duplicate.Indices.end(),{floor.Indices[i],floor.Indices[i+2],floor.Indices[i+1]});
    auto deduplicated=Build(Mesh({duplicate},{}),{0});
    require(deduplicated.Boxes.size()==2 && deduplicated.BoxedTriangles==8 && !contains(deduplicated,{50,21,50}),
        "Opposite-winding duplicates made double floor boxes");
    for (size_t i=0;i<floor.Indices.size();i+=3) std::swap(floor.Indices[i+1],floor.Indices[i+2]);
    const auto ceiling=Build(Mesh({floor},{}),{0});
    require(contains(ceiling,{50,21,50}) && !contains(ceiling,{50,19,50}),"Ceiling thickness moved its underside");

    SubMesh overlap;
    addQuad(overlap,{0,0,0},{100,0,0},{100,100,0},{0,100,0});
    addQuad(overlap,{50,50,0},{150,50,0},{150,150,0},{50,150,0});
    auto overlapping=Build(Mesh({overlap},{}),{0});
    require(overlapping.Boxes.size()==2 && overlapping.BoxedTriangles==4 &&
        contains(overlapping,{25,25,0}) && contains(overlapping,{125,125,0}) &&
        !contains(overlapping,{25,125,0}) && !contains(overlapping,{125,25,0}),
        "Independent overlapping panels changed the authored surface union");
    SubMesh triangle;
    for (const auto point : {Vector3(0,0,0),Vector3(100,0,0),Vector3(0,100,0)})
    { Vertex vertex; vertex.Position=point; triangle.Vertices.push_back(vertex); }
    triangle.Indices={0,1,2};
    auto irregular=Build(Mesh({triangle},{}),{0});
    require(irregular.Boxes.empty() && irregular.Remainder->Triangles.size()==1 && irregular.Remainder->Vertices.size()==3,
        "Nonrectangular boundary was filled or lost instead of retained as a mesh");
    auto mixed=triangle;
    addQuad(mixed,{200,0,0},{300,0,0},{300,100,0},{200,100,0});
    const auto independent=Build(Mesh({mixed},{}),{0});
    require(independent.Boxes.size()==1 && independent.BoxedTriangles==2 && independent.Remainder->Triangles.size()==1,
        "Unrelated irregular geometry prevented a rectangular wall from becoming a box");

    SubMesh angledFloor;
    for (const auto point : {Vector3(0,20,0),Vector3(0,20,200),Vector3(200,20,0),
        Vector3(400,20,0),Vector3(400,20,200),Vector3(600,20,0)})
    { Vertex vertex; vertex.Position=point; angledFloor.Vertices.push_back(vertex); }
    angledFloor.Indices={0,1,2,3,4,5};
    const auto prisms=Build(Mesh({angledFloor},{}),{0});
    require(prisms.FloorPrisms->ConvexRadiusMeters==0,
        "Static floor prisms introduced rounded seams between adjacent supports");
    require(prisms.Boxes.empty() && prisms.FloorPrisms->Hulls.size()==2 && prisms.PrismTriangles==2 &&
        prisms.Remainder->Triangles.empty(),"Nonrectangular floor patches did not gain solid thickness");
    require(contains(prisms,{30,19,30}) && contains(prisms,{430,16,30}) && !contains(prisms,{30,21,30}) &&
        contains(prisms,{30,1,30}) && !contains(prisms,{30,-1,30}),"Floor prism moved or incorrectly thickened the supporting surface");
    require(!contains(prisms,{150,19,150}) && !contains(prisms,{300,19,30}),
        "Floor prisms filled the diagonal cutout or bridged a gap");
    auto duplicatePrisms=angledFloor;
    duplicatePrisms.Indices.insert(duplicatePrisms.Indices.end(),{0,2,1,3,5,4});
    const auto uniquePrisms=Build(Mesh({duplicatePrisms},{}),{0});
    require(uniquePrisms.FloorPrisms->Hulls.size()==2 && uniquePrisms.PrismTriangles==4 && !contains(uniquePrisms,{30,21,30}),
        "Opposite-winding floor triangles created duplicate prisms");
    angledFloor.Indices={0,2,1,3,5,4};
    const auto ceilingPrisms=Build(Mesh({angledFloor},{}),{0});
    require(ceilingPrisms.FloorPrisms->Hulls.size()==2 && contains(ceilingPrisms,{30,21,30}) && !contains(ceilingPrisms,{30,19,30}),
        "Nonrectangular ceiling prisms extruded below the authored surface");
    SubMesh sliverFloor;
    for (const auto point : {Vector3(0,20,0),Vector3(100,20,0),Vector3(0,20,200),Vector3(50,20,-.05f)})
    { Vertex vertex; vertex.Position=point; sliverFloor.Vertices.push_back(vertex); }
    sliverFloor.Indices={0,2,1,0,1,3};
    const auto sliverSupport=Build(Mesh({sliverFloor},{}),{0});
    require(sliverSupport.SliverBoxes==1 && sliverSupport.BoxedTriangles==1 && sliverSupport.PrismTriangles==1 &&
        sliverSupport.Remainder->Triangles.empty(),"Thin authored floor seam was dropped or kept as an uncookable prism");
    require(contains(sliverSupport,{50,19,-.025f}) && !contains(sliverSupport,{50,21,-.025f}) &&
        !contains(sliverSupport,{50,19,-.2f}),"Floor sliver box lost support or exceeded the narrow footprint allowance");

    SubMesh noisyFloor;
    for (const auto point : {Vector3(0,40,0),Vector3(100,40,0),Vector3(0,40,200),Vector3(50,40,-.0001f)})
    { Vertex vertex; vertex.Position=point; noisyFloor.Vertices.push_back(vertex); }
    noisyFloor.Indices={0,2,1,0,3,1};
    const auto repairedFloor=Build(Mesh({noisyFloor},{}),{0});
    require(repairedFloor.Remainder->Triangles.empty() && repairedFloor.PrismTriangles==1 && repairedFloor.SliverBoxes==1,
        "One inverted numerical sliver disabled solid support for the entire floor");
    require(contains(repairedFloor,{20,39,20}) && contains(repairedFloor,{50,39,-.00005f}) &&
        !contains(repairedFloor,{20,41,20}) && !contains(repairedFloor,{50,41,-.00005f}),
        "Winding repair moved the supporting surface or extruded the floor sliver upward");
    for (const auto& hull : repairedFloor.FloorPrisms->Hulls)
        require((hull[1]-hull[0]).Cross(hull[2]-hull[0]).y>0 && hull[hull.size()/2].y<hull[0].y,
            "Repaired floor prism did not retain upward support and downward extrusion");
    noisyFloor.Vertices[3].Position.z=-100;
    const auto ambiguousFloor=Build(Mesh({noisyFloor},{}),{0});
    require(ambiguousFloor.Boxes.empty() && ambiguousFloor.FloorPrisms->Hulls.empty() &&
        ambiguousFloor.Remainder->Triangles.size()==2,
        "Substantial mixed floor winding was incorrectly treated as numerical noise");

    SubMesh floorBevel;
    for (const auto point : {Vector3(619.48f,51.803f,-603.037f),Vector3(618.901f,51.583f,-603.352f),
        Vector3(543.179f,51.583f,-462.66f)})
    { Vertex vertex; vertex.Position=point; floorBevel.Vertices.push_back(vertex); }
    floorBevel.Indices={0,1,2};
    const auto solidBevel=Build(Mesh({floorBevel},{}),{0});
    require(solidBevel.PrismTriangles==1 && solidBevel.FloorPrisms->Hulls.size()==1 && solidBevel.Remainder->Triangles.empty(),
        "Small sloped raised-floor bevel was left as a thin mesh");
    const auto& bevelHull=solidBevel.FloorPrisms->Hulls.front();
    for (size_t i=0;i<3;++i)
        require(bevelHull[i]==floorBevel.Vertices[i].Position && bevelHull[i+3]==bevelHull[i]-Vector3(0,20,0),
            "Raised-floor extrusion changed the authored top or XZ footprint");
    const auto bevelCenter=(bevelHull[0]+bevelHull[1]+bevelHull[2])/3;
    require(contains(solidBevel,bevelCenter-Vector3(0,.05f,0)) && !contains(solidBevel,bevelCenter+Vector3(0,.05f,0)),
        "Sloped floor prism did not support the exact authored surface");
    for (const float decorHeight : {49.6f,54.7f})
    {
        auto nearbyDecor=floorBevel;
        for (auto& vertex : nearbyDecor.Vertices) vertex.Position.y+=decorHeight-51.803f;
        const auto preservedDecor=Build(Mesh({nearbyDecor},{}),{0});
        require(preservedDecor.FloorPrisms->Hulls.empty() && preservedDecor.Remainder->Triangles.size()==1,
            "Raised-floor bevel handling expanded to nearby low decorative geometry");
    }
    for (auto& vertex : floorBevel.Vertices) vertex.Position.y+=100;
    const auto decorativeBevel=Build(Mesh({floorBevel},{}),{0});
    require(decorativeBevel.FloorPrisms->Hulls.empty() && decorativeBevel.Remainder->Triangles.size()==1,
        "Low floor bevel rule spread to a high decorative panel");

    const auto prism=[](std::initializer_list<Vector3> top)
    {
        std::vector<Vector3> hull(top);
        for (const auto& point : top) hull.push_back(point-Vector3(0,20,0));
        return hull;
    };
    BistroStaticCollision::Result bridgeSource;
    auto& bridgeHulls=bridgeSource.FloorPrisms->Hulls;
    bridgeHulls={prism({{0,39.622f,0},{0,39.622f,100},{100,39.622f,100},{100,39.622f,0}}),
        prism({{100,39.622f,0},{100,39.622f,100},{200,39.622f,100},{200,39.622f,0}}),
        prism({{300,39.622f,0},{300,39.622f,100},{400,39.622f,100},{400,39.622f,0}})};
    require(BistroStaticCollision::Detail::BridgeFloorPrisms(bridgeHulls)==1 && bridgeHulls.size()==4,
        "Floor seam bridge was missing, recursive or crossed a disconnected gap");
    BistroStaticCollision::Result bridgeOnly;
    bridgeOnly.FloorPrisms->Hulls={bridgeHulls.back()};
    require(contains(bridgeOnly,{100,39,50}) && contains(bridgeOnly,{99,39,50}) && contains(bridgeOnly,{101,39,50}) &&
        !contains(bridgeOnly,{100,40,50}) && !contains(bridgeSource,{250,39,50}),
        "Solid seam bridge lost support on either side or enlarged the authored floor footprint");
    bridgeHulls={prism({{0,39.622f,0},{100,39.622f,0},{-1000,39.622f,-100}}),
        prism({{100,39.622f,0},{0,39.622f,0},{1000,39.622f,100}})};
    const auto originalObtuse=bridgeHulls;
    require(BistroStaticCollision::Detail::BridgeFloorPrisms(bridgeHulls)==1,
        "Obtuse neighboring floor polygons did not receive a seam bridge");
    const auto obtuseBridge=bridgeHulls.back();
    bridgeSource.FloorPrisms->Hulls=originalObtuse;
    for (size_t i=0;i<4;++i)
    {
        const auto& a=obtuseBridge[i];
        const auto& b=obtuseBridge[(i+1)%4];
        const auto& c=obtuseBridge[(i+2)%4];
        require((b-a).Cross(c-b).y>0 && contains(bridgeSource,a) && contains(bridgeSource,(a+b)*.5f),
            "Obtuse seam bridge became concave or extended outside its neighbors");
    }
    bridgeHulls={prism({{0,39.622f,0},{0,39.622f,100},{100,39.622f,100}}),
        prism({{0,51.803f,0},{100,51.803f,100},{100,51.803f,0}})};
    require(BistroStaticCollision::Detail::BridgeFloorPrisms(bridgeHulls)==0,"Seam bridge joined different floor levels");

    BistroStaticCollision::Result mergedFloor;
    auto& mergedHulls=mergedFloor.FloorPrisms->Hulls;
    mergedHulls={prism({{0,40,0},{0,40,100},{100,40,100}}),prism({{0,40,0},{100,40,100},{100,40,0}})};
    require(BistroStaticCollision::Detail::MergeFloorPrisms(mergedHulls)==1 && mergedHulls.size()==1 && mergedHulls[0].size()==8,
        "Adjacent floor triangles retained an internal convex seam");
    require(contains(mergedFloor,{50,39,50}) && !contains(mergedFloor,{50,41,50}) && !contains(mergedFloor,{150,39,50}),
        "Merged floor prism changed support height or footprint");
    mergedHulls={prism({{0,40,0},{0,40,100},{100,40,100},{100,40,0}}),
        prism({{100,40,0},{100,40,100},{200,40,100},{200,40,0}}),
        prism({{0,40,100},{0,40,200},{100,40,200},{100,40,100}})};
    BistroStaticCollision::Detail::MergeFloorPrisms(mergedHulls);
    require(mergedHulls.size()==2 && contains(mergedFloor,{50,39,150}) && contains(mergedFloor,{150,39,50}) &&
        !contains(mergedFloor,{150,39,150}),"Convex merging filled a concave floor corner");
    mergedHulls={prism({{0,40,0},{100,40,100},{100,40,0}}),prism({{0,40,0},{-20,40,-10},{100,40,100}})};
    require(BistroStaticCollision::Detail::MergeFloorPrisms(mergedHulls)==0 && !contains(mergedFloor,{0,39,-1}),
        "Shared-edge triangle merging filled a concavity outside the authored union");
    mergedHulls={prism({{0,40,0},{0,40,100},{100,40,0}}),prism({{200,40,0},{200,40,100},{300,40,0}})};
    require(BistroStaticCollision::Detail::MergeFloorPrisms(mergedHulls)==0 && mergedHulls.size()==2 &&
        !contains(mergedFloor,{150,39,20}),"Prism merging bridged disconnected floor patches");
    mergedHulls={prism({{0,40,0},{0,40,100},{100,40,100}}),prism({{0,50,0},{100,50,100},{100,50,0}})};
    require(BistroStaticCollision::Detail::MergeFloorPrisms(mergedHulls)==0,"Prism merging joined different floor heights");
    triangle.Vertices[1].Position.x=std::numeric_limits<float>::quiet_NaN();
    bool rejected=false;
    try { Build(Mesh({triangle},{}),{0}); } catch (const std::invalid_argument&) { rejected=true; }
    require(rejected,"Static collision accepted a nonfinite source vertex");
    triangle.Vertices[1].Position.x=100;
    triangle.Indices[2]=20; rejected=false;
    try { Build(Mesh({triangle},{}),{0}); } catch (const std::invalid_argument&) { rejected=true; }
    require(rejected,"Static collision accepted an invalid source triangle index");
}
