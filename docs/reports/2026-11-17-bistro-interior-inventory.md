# Bistro interior inventory and room survey

Source: `Assets/Scenes/Amazon Lumberyard Bistro/Interior/interior.obj`.
Read-only source survey for the categorized Meta interior physics stage; no runtime
or physics accuracy experiment was run.

## Object ownership and categories

The OBJ contains **1,133 authored groups**, **570,555 position vertices** and
**2,009 unique (group, material) sections**. There are 552 groups with multiple
materials. Material sections remain render submeshes, not separate rigid bodies.
The importer may add material section suffixes to SourceNodeName; partitioning
must normalize those sections back to the authored object before counting bodies.

The two CookieJar objects each have separate glass/body OBJ groups:
`CookieJar_Glass` + `CookieJar`, and `CookieJar_Glass__2_` + `CookieJar__2_`.
Their matching spatial bounds confirm the pairs. Merge each pair into one body,
preserving all four material sections per jar. This leaves **1,131 logical groups**,
including **974 movable props** and **157 excluded static groups**.

| Category | Logical objects | Name families | First dynamic preset |
| --- | ---: | --- | --- |
| Glass | 256 | Glass_Water/Wine/Beer/Detailed, Trolley_paris_glass_water/wine | Low |
| Plates | 83 | Paris_Plate_, Plates_, Trolley_paris_plate_ | Medium |
| Cutlery | 107 | Cutlery_, Trolley_paris_cutlery_ | High |
| Bottles | 304 | Paris_LiquorBottle_, Paris_liquorbottle_, Cooler_*_paris_liquorbottle_ | Epic |
| Furniture | 58 | 10 tables, 32 chairs, 7 barstools, 3 coat racks, 5 wine coolers, 1 bar cart | Epic |
| Fabric/proxies | 36 | 32 Cotton_Placemat_, Cooler_Cloth, Doormat_, 2 Paris_Rubber_Bar_Mat_ | Epic |
| Baskets | 15 | WickerBasket | Epic |
| Other movable | 115 | 62 napkin holders, 46 coasters, 2 toffee jars, 2 cookie jars, 2 flower pots, 1 cash register | Epic |
| Excluded static | 157 | Building, curtains, lights/fans, paintings, radiators, fixed beer taps | Never |

The excluded groups are 116 `paris_building_01_interior_`, 3 curtains, 11 lights,
4 fan parts, 15 paintings, 6 radiators and 2 beer taps. These are explicit static
families; any future unrecognized family must remain static and be reported.
Use strict `Paris_Table_` family matching, not the substring `table`.
Characters, generated UI/painting surfaces, reflection preview spheres and exterior
geometry are not members of this interior movable inventory.

## Preset counts

Waves register exactly the dynamic props. No multiplication or fraction sampling
applies in the categorized mode.

| Preset | Dynamic props | Static inactive props | Wave participants |
| --- | ---: | ---: | ---: |
| Off | 0 | 974 | 0 |
| Low | 256 | 718 | 256 |
| Medium | 339 | 635 | 339 |
| High | 446 | 528 | 446 |
| Epic | 974 | 0 | 974 |

The existing integration adds one interior room/counter static body and one
exterior walkway static body; with that ownership retained, total rigid bodies
are 976 and total static body counts are respectively **976, 720, 637, 530, 2**.
The 157 excluded OBJ groups are rendered in the static batch covered by those
architectural proxies; they are not 157 additional rigid bodies. Runtime logs
must confirm these implementation totals after scene loading.

## Room survey

`BistroStaticCollision::BuildRoom()` already has a closed wall chain around the
angled dining facade and narrow rear wing. In its plan frame (world yaw -28.5°),
the facade runs from (-241,-155) through (-160,-94), (-80,-34), (0,26), (80,70),
(980,-38) to (1003,-86). The rear chain returns through the stepped rear edge and
wing down to z=-1316, then closes at (-241,-155). No wall segment contains an
intentional door/window opening: the current invisible shell already seals them
for props while leaving the rendered doors/windows and free camera untouched.
It is not a single AABB around the terrace.

A direct source-coordinate survey, transformed into that frame, places **all
movable authored group centres inside this polygon**. Their combined vertex bounds
are x=[-233.550,1003.358], y=[38.854,344.916], z=[-1261.947,47.140] cm. The movable
categories' vertical extents are below the ceiling at y=402.78 cm. The Low glass
range is x=[-224.218,994.032], y=[56.250,251.918], z=[-835.977,32.270] cm.

The existing floor is y=39.622, raised floor y=51.803, ceiling y=402.78 cm. Floor
slabs extend 36 cm down, ceilings 20 cm up; the facade is covered by short floor
and ceiling strips, and counter/step spans preserve the room's complex shape.
Walls use half-thickness 15 cm and outward offset 10 cm, leaving their inner face
5 cm inside the nominal survey contour. This does not intersect the outermost Low
glass x extent at the rear wall (inner face x=998), but some Epic furniture/prop
vertices approach or slightly cross the surveyed wall contour. Thin doormat source
vertices reach 0.768 cm below the nominal floor. Analytic proxies may begin with
small penetrations there and settle; this survey does not certify contact accuracy.

The existing wave speed cap is 3000 cm/s (50 cm per 60 Hz tick), lift 500 cm/s,
scatter 2100 cm/s after 0.3 s. Keep CCD enabled for dynamic bodies and retain the
existing shell thickness, physics substeps and contact settings. Do not substitute
periodic teleportation for containment.

## Limits

This is a source inventory and geometric survey, not a runtime preset matrix or
long soak. Exact body counts depend on preserving the stated grouping and static
proxy ownership; the implementation's startup logs are the final runtime evidence.
A short Low smoke cannot establish long-term containment or Epic contact behavior.
