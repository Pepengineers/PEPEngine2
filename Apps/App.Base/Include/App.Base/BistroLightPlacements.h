#pragma once

#include "Engine.RendererDX12/D3DHelpers.h"

enum class BistroFixtureKind { Point, Spot };

struct BistroLightPlacement
{
    const char* Name;
    BistroFixtureKind Kind;
    Vector3 Position;
    Vector3 Direction;
    float Range;
    float Intensity;
};

// Original Bistro OBJ coordinates (+Y up); positions clear opaque bulb and shade geometry.
inline const BistroLightPlacement BistroLightPlacements[] = {
    // Exterior MASTER_Light_Bulb connected components; 2 units below each bulb.
    {"Paris_Building_01_paris_building_01_bottom_44", BistroFixtureKind::Point, {361.331000f, 410.190000f, 279.902000f}, {0.0f, -1.0f, 0.0f}, 500.0f, 150.0f},
    {"Paris_Building_01_paris_building_01_bottom_45", BistroFixtureKind::Point, {560.745500f, 410.190000f, 358.775500f}, {0.0f, -1.0f, 0.0f}, 500.0f, 150.0f},
    {"Paris_Building_01_paris_building_01_bottom_46", BistroFixtureKind::Point, {778.120000f, 410.190000f, 444.752500f}, {0.0f, -1.0f, 0.0f}, 500.0f, 150.0f},
    {"Paris_Building_01_paris_building_01_bottom_47", BistroFixtureKind::Point, {-117.550500f, 410.188000f, -426.036500f}, {0.0f, -1.0f, 0.0f}, 500.0f, 150.0f},
    {"Paris_Building_01_paris_building_01_bottom_48", BistroFixtureKind::Point, {-6.614500f, 410.189000f, -631.797000f}, {0.0f, -1.0f, 0.0f}, 500.0f, 150.0f},
    {"Paris_Building_01_paris_building_01_bottom_49", BistroFixtureKind::Point, {95.155500f, 410.189000f, -820.556500f}, {0.0f, -1.0f, 0.0f}, 500.0f, 150.0f},
    {"Paris_Building_01_paris_building_01_bottom_67", BistroFixtureKind::Point, {131.869500f, 410.190000f, 189.602500f}, {0.0f, -1.0f, 0.0f}, 500.0f, 150.0f},
    {"Paris_Building_01_paris_building_01_bottom_68", BistroFixtureKind::Point, {212.188500f, 410.189000f, -1037.623500f}, {0.0f, -1.0f, 0.0f}, 500.0f, 150.0f},
    {"Paris_Building_01_paris_building_01_bottom_158", BistroFixtureKind::Point, {-178.991000f, 266.331000f, -222.308000f}, {0.0f, -1.0f, 0.0f}, 500.0f, 150.0f},
    {"Paris_Building_01_paris_building_01_bottom_159", BistroFixtureKind::Point, {-42.617000f, 267.739000f, 76.784500f}, {0.0f, -1.0f, 0.0f}, 500.0f, 150.0f},

    // 28 exterior streetlight bulbs; 2 units below the lower metal rim to clear opaque glass/housing.
    {"Paris_Streetlight_01a", BistroFixtureKind::Point, {-433.326000f, 409.493000f, -419.017000f}, {0.0f, -1.0f, 0.0f}, 650.0f, 180.0f},
    {"Paris_Streetlight_01a__2_", BistroFixtureKind::Point, {373.428000f, 409.996000f, -2125.876000f}, {0.0f, -1.0f, 0.0f}, 650.0f, 180.0f},
    {"Paris_Streetlight_01a__3_", BistroFixtureKind::Point, {-2081.920000f, 409.996000f, -1823.962000f}, {0.0f, -1.0f, 0.0f}, 650.0f, 180.0f},
    {"Paris_Streetlight_01a__4_", BistroFixtureKind::Point, {-965.266500f, 409.996000f, 208.815500f}, {0.0f, -1.0f, 0.0f}, 650.0f, 180.0f},
    {"Paris_Streetlight_01a__5_", BistroFixtureKind::Point, {-208.779000f, 409.996000f, 465.133000f}, {0.0f, -1.0f, 0.0f}, 650.0f, 180.0f},
    {"Paris_Streetlight_01a__6_", BistroFixtureKind::Point, {-175.614500f, 409.996000f, 1004.081500f}, {0.0f, -1.0f, 0.0f}, 650.0f, 180.0f},
    {"Paris_Streetlight_01a__7_", BistroFixtureKind::Point, {781.491500f, 409.996000f, 865.956500f}, {0.0f, -1.0f, 0.0f}, 650.0f, 180.0f},
    {"Paris_Streetlight_01a__8_", BistroFixtureKind::Point, {2158.014500f, 409.996000f, 1882.062000f}, {0.0f, -1.0f, 0.0f}, 650.0f, 180.0f},
    {"Paris_Streetlight_01a__9_", BistroFixtureKind::Point, {3330.580000f, 409.996000f, 2409.981000f}, {0.0f, -1.0f, 0.0f}, 650.0f, 180.0f},
    {"Paris_Streetlight_01a__10_", BistroFixtureKind::Point, {3879.824000f, 409.996000f, 3394.735000f}, {0.0f, -1.0f, 0.0f}, 650.0f, 180.0f},
    {"Paris_Streetlight_01a__11_", BistroFixtureKind::Point, {4932.883000f, 409.996000f, 3435.792000f}, {0.0f, -1.0f, 0.0f}, 650.0f, 180.0f},
    {"Paris_Streetlight_01a__12_", BistroFixtureKind::Point, {3510.323000f, 409.996000f, 1832.195000f}, {0.0f, -1.0f, 0.0f}, 650.0f, 180.0f},
    {"Paris_StreetLight_01B", BistroFixtureKind::Point, {-1339.951500f, 372.467000f, 121.300000f}, {0.0f, -1.0f, 0.0f}, 650.0f, 180.0f},
    {"Paris_StreetLight_01B__2_", BistroFixtureKind::Point, {-1725.812000f, 372.467000f, -378.602500f}, {0.0f, -1.0f, 0.0f}, 650.0f, 180.0f},
    {"Paris_StreetLight_01B__3_", BistroFixtureKind::Point, {-2017.024000f, 372.467000f, -718.796000f}, {0.0f, -1.0f, 0.0f}, 650.0f, 180.0f},
    {"Paris_StreetLight_01B__4_", BistroFixtureKind::Point, {-2458.580000f, 371.137000f, -1180.908000f}, {0.0f, -1.0f, 0.0f}, 650.0f, 180.0f},
    {"Paris_StreetLight_01B__5_", BistroFixtureKind::Point, {-2851.466000f, 298.162000f, -1719.422000f}, {0.0f, -1.0f, 0.0f}, 650.0f, 180.0f},
    {"Paris_StreetLight_01B__6_", BistroFixtureKind::Point, {-2689.168000f, 298.162000f, -2106.046000f}, {0.0f, -1.0f, 0.0f}, 650.0f, 180.0f},
    {"Paris_StreetLight_01B__7_", BistroFixtureKind::Point, {-1773.058500f, 372.793000f, -1494.189000f}, {0.0f, -1.0f, 0.0f}, 650.0f, 180.0f},
    {"Paris_StreetLight_01B__8_", BistroFixtureKind::Point, {-1193.674500f, 372.467000f, -961.884500f}, {0.0f, -1.0f, 0.0f}, 650.0f, 180.0f},
    {"Paris_StreetLight_01B__9_", BistroFixtureKind::Point, {-843.237500f, 375.138000f, -1141.376000f}, {0.0f, -1.0f, 0.0f}, 650.0f, 180.0f},
    {"Paris_StreetLight_01B__10_", BistroFixtureKind::Point, {-212.128500f, 391.431000f, -1828.256500f}, {0.0f, -1.0f, 0.0f}, 650.0f, 180.0f},
    {"Paris_StreetLight_01B__11_", BistroFixtureKind::Point, {1802.555500f, 416.735000f, -4156.665000f}, {0.0f, -1.0f, 0.0f}, 650.0f, 180.0f},
    {"Paris_StreetLight_01B__12_", BistroFixtureKind::Point, {1765.658000f, 416.735000f, -3440.353000f}, {0.0f, -1.0f, 0.0f}, 650.0f, 180.0f},
    {"Paris_StreetLight_01B__13_", BistroFixtureKind::Point, {898.219000f, 423.911000f, -2008.212500f}, {0.0f, -1.0f, 0.0f}, 650.0f, 180.0f},
    {"Paris_StreetLight_01B__14_", BistroFixtureKind::Point, {3000.853000f, 374.075000f, 1350.240000f}, {0.0f, -1.0f, 0.0f}, 650.0f, 180.0f},
    {"Paris_StreetLight_01B__15_", BistroFixtureKind::Point, {2446.246000f, 429.276000f, 2316.705000f}, {0.0f, -1.0f, 0.0f}, 650.0f, 180.0f},
    {"Paris_StreetLight_01B__16_", BistroFixtureKind::Point, {2076.290000f, 374.075000f, 1275.617500f}, {0.0f, -1.0f, 0.0f}, 650.0f, 180.0f},

    // Exterior Spotlight_Glass connected lenses; 2 units along the actual outward lens normal.
    {"Spotlights_1_1", BistroFixtureKind::Spot, {-2189.932215f, 865.290893f, -878.310719f}, {-0.258108f, 0.939696f, 0.224390f}, 1100.0f, 450.0f},
    {"Spotlights_1_2", BistroFixtureKind::Spot, {-2563.252611f, 865.290929f, -1309.125749f}, {-0.258056f, 0.939714f, 0.224375f}, 1100.0f, 450.0f},
    {"Spotlights_1_3", BistroFixtureKind::Spot, {1231.021500f, 340.951000f, -2508.879000f}, {0.0f, -1.0f, 0.0f}, 1100.0f, 450.0f},
    {"Spotlights_2_1", BistroFixtureKind::Spot, {983.025000f, 339.634000f, -2093.812500f}, {0.0f, -1.0f, 0.0f}, 1100.0f, 450.0f},
    {"Spotlights_3_1", BistroFixtureKind::Spot, {-1242.096198f, 866.258986f, 229.178911f}, {-0.258099f, 0.939743f, 0.224205f}, 1100.0f, 450.0f},
    {"Spotlights_4_1", BistroFixtureKind::Spot, {-1615.682222f, 866.258943f, -205.051162f}, {-0.250111f, 0.939722f, 0.233169f}, 1100.0f, 450.0f},
    {"Spotlights_5_1", BistroFixtureKind::Spot, {-1841.664159f, 865.290992f, -585.871024f}, {-0.254080f, 0.939746f, 0.228738f}, 1100.0f, 450.0f},

    // Eight strings, eight connected emissive bulbs each; separate lights even when materials repeat.
    {"paris_stringlights_01a8_Red_1", BistroFixtureKind::Point, {269.314500f, 398.968000f, -945.381000f}, {0.0f, -1.0f, 0.0f}, 350.0f, 35.0f},
    {"paris_stringlights_01a8_Blue_1", BistroFixtureKind::Point, {-516.464500f, 570.747000f, -1658.131000f}, {0.0f, -1.0f, 0.0f}, 350.0f, 35.0f},
    {"paris_stringlights_01a8_Blue_2", BistroFixtureKind::Point, {-76.587500f, 431.524000f, -1255.428000f}, {0.0f, -1.0f, 0.0f}, 350.0f, 35.0f},
    {"paris_stringlights_01a8_Green_1", BistroFixtureKind::Point, {38.002000f, 413.645000f, -1152.029000f}, {0.0f, -1.0f, 0.0f}, 350.0f, 35.0f},
    {"paris_stringlights_01a8_Pink_1", BistroFixtureKind::Point, {-190.160000f, 456.598000f, -1358.484500f}, {0.0f, -1.0f, 0.0f}, 350.0f, 35.0f},
    {"paris_stringlights_01a8_Orange_1", BistroFixtureKind::Point, {-300.488000f, 487.891000f, -1459.169500f}, {0.0f, -1.0f, 0.0f}, 350.0f, 35.0f},
    {"paris_stringlights_01a8_White_1", BistroFixtureKind::Point, {-409.375000f, 525.982000f, -1559.197000f}, {0.0f, -1.0f, 0.0f}, 350.0f, 35.0f},
    {"paris_stringlights_01a8_White_2", BistroFixtureKind::Point, {153.298500f, 402.848000f, -1048.722500f}, {0.0f, -1.0f, 0.0f}, 350.0f, 35.0f},
    {"paris_stringlights_01a7_Red_1", BistroFixtureKind::Point, {-398.662000f, 402.277000f, -970.279500f}, {0.0f, -1.0f, 0.0f}, 350.0f, 35.0f},
    {"paris_stringlights_01a7_Blue_1", BistroFixtureKind::Point, {-762.246500f, 464.884000f, -1234.719000f}, {0.0f, -1.0f, 0.0f}, 350.0f, 35.0f},
    {"paris_stringlights_01a7_Blue_2", BistroFixtureKind::Point, {-149.644500f, 393.773000f, -786.914000f}, {0.0f, -1.0f, 0.0f}, 350.0f, 35.0f},
    {"paris_stringlights_01a7_Green_1", BistroFixtureKind::Point, {96.381000f, 412.834000f, -603.955000f}, {0.0f, -1.0f, 0.0f}, 350.0f, 35.0f},
    {"paris_stringlights_01a7_Pink_1", BistroFixtureKind::Point, {-519.168000f, 416.382000f, -1058.355000f}, {0.0f, -1.0f, 0.0f}, 350.0f, 35.0f},
    {"paris_stringlights_01a7_Orange_1", BistroFixtureKind::Point, {-641.215000f, 437.353000f, -1147.127000f}, {0.0f, -1.0f, 0.0f}, 350.0f, 35.0f},
    {"paris_stringlights_01a7_Orange_2", BistroFixtureKind::Point, {-275.176000f, 394.583000f, -879.576500f}, {0.0f, -1.0f, 0.0f}, 350.0f, 35.0f},
    {"paris_stringlights_01a7_White_1", BistroFixtureKind::Point, {-23.890500f, 400.074000f, -693.617000f}, {0.0f, -1.0f, 0.0f}, 350.0f, 35.0f},
    {"paris_stringlights_01a6_Red_1", BistroFixtureKind::Point, {-442.248000f, 376.678000f, -669.502000f}, {0.0f, -1.0f, 0.0f}, 350.0f, 35.0f},
    {"paris_stringlights_01a6_Blue_1", BistroFixtureKind::Point, {-1100.092000f, 505.251000f, -1029.564500f}, {0.0f, -1.0f, 0.0f}, 350.0f, 35.0f},
    {"paris_stringlights_01a6_Blue_2", BistroFixtureKind::Point, {-841.299000f, 426.313000f, -890.623500f}, {0.0f, -1.0f, 0.0f}, 350.0f, 35.0f},
    {"paris_stringlights_01a6_Blue_3", BistroFixtureKind::Point, {-175.822500f, 391.055000f, -517.250000f}, {0.0f, -1.0f, 0.0f}, 350.0f, 35.0f},
    {"paris_stringlights_01a6_Green_1", BistroFixtureKind::Point, {-971.650500f, 461.238000f, -961.183500f}, {0.0f, -1.0f, 0.0f}, 350.0f, 35.0f},
    {"paris_stringlights_01a6_Green_2", BistroFixtureKind::Point, {-305.972500f, 379.251000f, -592.080500f}, {0.0f, -1.0f, 0.0f}, 350.0f, 35.0f},
    {"paris_stringlights_01a6_Pink_1", BistroFixtureKind::Point, {-578.187500f, 384.001000f, -745.767500f}, {0.0f, -1.0f, 0.0f}, 350.0f, 35.0f},
    {"paris_stringlights_01a6_White_1", BistroFixtureKind::Point, {-711.654500f, 400.800000f, -819.719000f}, {0.0f, -1.0f, 0.0f}, 350.0f, 35.0f},
    {"paris_stringlights_01a5_Red_1", BistroFixtureKind::Point, {-253.277000f, 367.390000f, -219.986500f}, {0.0f, -1.0f, 0.0f}, 350.0f, 35.0f},
    {"paris_stringlights_01a5_Blue_1", BistroFixtureKind::Point, {-1334.806500f, 423.281000f, 156.103500f}, {0.0f, -1.0f, 0.0f}, 350.0f, 35.0f},
    {"paris_stringlights_01a5_Blue_2", BistroFixtureKind::Point, {-724.166000f, 346.979000f, -63.052500f}, {0.0f, -1.0f, 0.0f}, 350.0f, 35.0f},
    {"paris_stringlights_01a5_Green_1", BistroFixtureKind::Point, {-567.342000f, 346.328000f, -116.469000f}, {0.0f, -1.0f, 0.0f}, 350.0f, 35.0f},
    {"paris_stringlights_01a5_Pink_1", BistroFixtureKind::Point, {-880.566000f, 355.299000f, -8.642500f}, {0.0f, -1.0f, 0.0f}, 350.0f, 35.0f},
    {"paris_stringlights_01a5_Orange_1", BistroFixtureKind::Point, {-1033.257500f, 370.644000f, 45.617500f}, {0.0f, -1.0f, 0.0f}, 350.0f, 35.0f},
    {"paris_stringlights_01a5_White_1", BistroFixtureKind::Point, {-1184.777500f, 393.377000f, 100.601500f}, {0.0f, -1.0f, 0.0f}, 350.0f, 35.0f},
    {"paris_stringlights_01a5_White_2", BistroFixtureKind::Point, {-410.367500f, 353.183000f, -168.820500f}, {0.0f, -1.0f, 0.0f}, 350.0f, 35.0f},
    {"paris_stringlights_01a4_Red_1", BistroFixtureKind::Point, {-286.898500f, 368.573000f, 258.689500f}, {0.0f, -1.0f, 0.0f}, 350.0f, 35.0f},
    {"paris_stringlights_01a4_Blue_1", BistroFixtureKind::Point, {-905.586000f, 439.003000f, 696.104000f}, {0.0f, -1.0f, 0.0f}, 350.0f, 35.0f},
    {"paris_stringlights_01a4_Blue_2", BistroFixtureKind::Point, {-661.699500f, 382.764000f, 523.167500f}, {0.0f, -1.0f, 0.0f}, 350.0f, 35.0f},
    {"paris_stringlights_01a4_Blue_3", BistroFixtureKind::Point, {-37.506000f, 406.745000f, 83.367000f}, {0.0f, -1.0f, 0.0f}, 350.0f, 35.0f},
    {"paris_stringlights_01a4_Green_1", BistroFixtureKind::Point, {-784.439500f, 406.229000f, 610.193500f}, {0.0f, -1.0f, 0.0f}, 350.0f, 35.0f},
    {"paris_stringlights_01a4_Green_2", BistroFixtureKind::Point, {-159.289000f, 383.334000f, 168.912000f}, {0.0f, -1.0f, 0.0f}, 350.0f, 35.0f},
    {"paris_stringlights_01a4_Pink_1", BistroFixtureKind::Point, {-414.404000f, 363.773000f, 348.519500f}, {0.0f, -1.0f, 0.0f}, 350.0f, 35.0f},
    {"paris_stringlights_01a4_White_1", BistroFixtureKind::Point, {-539.734000f, 368.707000f, 436.978500f}, {0.0f, -1.0f, 0.0f}, 350.0f, 35.0f},
    {"paris_stringlights_01a3_Red_1", BistroFixtureKind::Point, {88.207000f, 405.515000f, 171.115000f}, {0.0f, -1.0f, 0.0f}, 350.0f, 35.0f},
    {"paris_stringlights_01a3_Blue_1", BistroFixtureKind::Point, {-535.230500f, 530.034000f, 1037.613500f}, {0.0f, -1.0f, 0.0f}, 350.0f, 35.0f},
    {"paris_stringlights_01a3_Blue_2", BistroFixtureKind::Point, {-189.162000f, 417.896000f, 544.928000f}, {0.0f, -1.0f, 0.0f}, 350.0f, 35.0f},
    {"paris_stringlights_01a3_Green_1", BistroFixtureKind::Point, {-97.711000f, 406.800000f, 419.826000f}, {0.0f, -1.0f, 0.0f}, 350.0f, 35.0f},
    {"paris_stringlights_01a3_Pink_1", BistroFixtureKind::Point, {-279.267500f, 436.140000f, 670.215000f}, {0.0f, -1.0f, 0.0f}, 350.0f, 35.0f},
    {"paris_stringlights_01a3_Orange_1", BistroFixtureKind::Point, {-366.305000f, 460.666000f, 793.115000f}, {0.0f, -1.0f, 0.0f}, 350.0f, 35.0f},
    {"paris_stringlights_01a3_White_1", BistroFixtureKind::Point, {-451.744000f, 492.010000f, 915.736500f}, {0.0f, -1.0f, 0.0f}, 350.0f, 35.0f},
    {"paris_stringlights_01a3_White_2", BistroFixtureKind::Point, {-5.287000f, 402.726000f, 295.209000f}, {0.0f, -1.0f, 0.0f}, 350.0f, 35.0f},
    {"paris_stringlights_01a2_Red_1", BistroFixtureKind::Point, {290.189500f, 340.401000f, 600.640500f}, {0.0f, -1.0f, 0.0f}, 350.0f, 35.0f},
    {"paris_stringlights_01a2_Blue_1", BistroFixtureKind::Point, {32.674000f, 374.829000f, 1179.033000f}, {0.0f, -1.0f, 0.0f}, 350.0f, 35.0f},
    {"paris_stringlights_01a2_Blue_2", BistroFixtureKind::Point, {134.675500f, 337.541000f, 950.010000f}, {0.0f, -1.0f, 0.0f}, 350.0f, 35.0f},
    {"paris_stringlights_01a2_Blue_3", BistroFixtureKind::Point, {392.957000f, 381.904000f, 369.871000f}, {0.0f, -1.0f, 0.0f}, 350.0f, 35.0f},
    {"paris_stringlights_01a2_Green_1", BistroFixtureKind::Point, {83.318500f, 352.265000f, 1065.115000f}, {0.0f, -1.0f, 0.0f}, 350.0f, 35.0f},
    {"paris_stringlights_01a2_Green_2", BistroFixtureKind::Point, {342.855500f, 357.688000f, 482.348000f}, {0.0f, -1.0f, 0.0f}, 350.0f, 35.0f},
    {"paris_stringlights_01a2_Pink_1", BistroFixtureKind::Point, {237.422000f, 331.411000f, 719.197000f}, {0.0f, -1.0f, 0.0f}, 350.0f, 35.0f},
    {"paris_stringlights_01a2_White_1", BistroFixtureKind::Point, {185.396500f, 330.608000f, 836.013500f}, {0.0f, -1.0f, 0.0f}, 350.0f, 35.0f},
    {"paris_stringlights_01a1_Red_1", BistroFixtureKind::Point, {790.363500f, 410.112000f, 536.252000f}, {0.0f, -1.0f, 0.0f}, 350.0f, 35.0f},
    {"paris_stringlights_01a1_Blue_1", BistroFixtureKind::Point, {460.590000f, 575.318000f, 1352.414000f}, {0.0f, -1.0f, 0.0f}, 350.0f, 35.0f},
    {"paris_stringlights_01a1_Blue_2", BistroFixtureKind::Point, {645.595500f, 446.863000f, 894.464500f}, {0.0f, -1.0f, 0.0f}, 350.0f, 35.0f},
    {"paris_stringlights_01a1_Green_1", BistroFixtureKind::Point, {693.609500f, 428.758000f, 775.627000f}, {0.0f, -1.0f, 0.0f}, 350.0f, 35.0f},
    {"paris_stringlights_01a1_Pink_1", BistroFixtureKind::Point, {597.929500f, 470.932000f, 1012.439500f}, {0.0f, -1.0f, 0.0f}, 350.0f, 35.0f},
    {"paris_stringlights_01a1_Orange_1", BistroFixtureKind::Point, {551.554500f, 500.098000f, 1127.191500f}, {0.0f, -1.0f, 0.0f}, 350.0f, 35.0f},
    {"paris_stringlights_01a1_White_1", BistroFixtureKind::Point, {505.718500f, 534.943000f, 1240.619000f}, {0.0f, -1.0f, 0.0f}, 350.0f, 35.0f},
    {"paris_stringlights_01a1_White_2", BistroFixtureKind::Point, {741.888500f, 416.556000f, 656.215000f}, {0.0f, -1.0f, 0.0f}, 350.0f, 35.0f},

    // Exterior Lantern map_Ke UV region; 2 units below each shade opening.
    {"Paris_Lantern_01A", BistroFixtureKind::Point, {26.640500f, 227.326000f, -726.442500f}, {0.0f, -1.0f, 0.0f}, 450.0f, 120.0f},
    {"Paris_Lantern_01A1", BistroFixtureKind::Point, {-109.380500f, 227.232000f, -474.039500f}, {0.0f, -1.0f, 0.0f}, 450.0f, 120.0f},
    {"Paris_Lantern_01A2", BistroFixtureKind::Point, {172.516500f, 227.029000f, 215.805500f}, {0.0f, -1.0f, 0.0f}, 450.0f, 120.0f},
    {"Paris_Lantern_01A3", BistroFixtureKind::Point, {450.699000f, 227.030000f, 325.833500f}, {0.0f, -1.0f, 0.0f}, 450.0f, 120.0f},
    {"Paris_Lantern_01A4", BistroFixtureKind::Point, {726.788500f, 227.031000f, 435.034000f}, {0.0f, -1.0f, 0.0f}, 450.0f, 120.0f},

    // Interior MASTER_Interior_01_Paris_Lantern map_Ke UV region; below each shade opening.
    {"paris_building_01_interior_63", BistroFixtureKind::Point, {722.376000f, 248.922000f, -278.580000f}, {0.0f, -1.0f, 0.0f}, 450.0f, 120.0f},
    {"paris_building_01_interior_105", BistroFixtureKind::Point, {879.740500f, 248.922000f, -192.013000f}, {0.0f, -1.0f, 0.0f}, 450.0f, 120.0f},
    {"paris_building_01_interior_106", BistroFixtureKind::Point, {1037.195000f, 248.922000f, -105.397500f}, {0.0f, -1.0f, 0.0f}, 450.0f, 120.0f},
    {"paris_building_01_interior_107", BistroFixtureKind::Point, {662.390500f, 248.922000f, -420.725500f}, {0.0f, -1.0f, 0.0f}, 450.0f, 120.0f},

    // Four Paris_Ceiling_Light map_Ke disks; point and downlight below each lower shade rim.
    {"Paris_Ceiling_Light_01_Bulb", BistroFixtureKind::Point, {611.445500f, 292.996000f, -849.156000f}, {0.0f, -1.0f, 0.0f}, 550.0f, 150.0f},
    {"Paris_Ceiling_Light_01_Downlight", BistroFixtureKind::Spot, {611.445500f, 292.996000f, -849.156000f}, {0.0f, -1.0f, 0.0f}, 1000.0f, 400.0f},
    {"Paris_Ceiling_Light_02_Bulb", BistroFixtureKind::Point, {424.949500f, 307.563000f, -493.348000f}, {0.0f, -1.0f, 0.0f}, 550.0f, 150.0f},
    {"Paris_Ceiling_Light_02_Downlight", BistroFixtureKind::Spot, {424.949500f, 307.563000f, -493.348000f}, {0.0f, -1.0f, 0.0f}, 1000.0f, 400.0f},
    {"Paris_Ceiling_Light_03_Bulb", BistroFixtureKind::Point, {500.105500f, 307.563000f, -50.352000f}, {0.0f, -1.0f, 0.0f}, 550.0f, 150.0f},
    {"Paris_Ceiling_Light_03_Downlight", BistroFixtureKind::Spot, {500.105500f, 307.563000f, -50.352000f}, {0.0f, -1.0f, 0.0f}, 1000.0f, 400.0f},
    {"Paris_Ceiling_Light_04_Bulb", BistroFixtureKind::Point, {798.378500f, 307.563000f, 100.140500f}, {0.0f, -1.0f, 0.0f}, 550.0f, 150.0f},
    {"Paris_Ceiling_Light_04_Downlight", BistroFixtureKind::Spot, {798.378500f, 307.563000f, 100.140500f}, {0.0f, -1.0f, 0.0f}, 1000.0f, 400.0f},

    // Seven Paris_Wall_Bulb_Light components; 2 units below the bulb surface.
    {"Paris_Wall_Light_Interior_01", BistroFixtureKind::Point, {191.902500f, 249.558000f, -811.006500f}, {0.0f, -1.0f, 0.0f}, 450.0f, 100.0f},
    {"Paris_Wall_Light_Interior_02", BistroFixtureKind::Point, {50.266000f, 249.558000f, -551.061000f}, {0.0f, -1.0f, 0.0f}, 450.0f, 100.0f},
    {"Paris_Wall_Light_Interior_03", BistroFixtureKind::Point, {-94.535000f, 249.558000f, -284.018000f}, {0.0f, -1.0f, 0.0f}, 450.0f, 100.0f},
    {"Paris_Wall_Light_Interior_04", BistroFixtureKind::Point, {41.186000f, 249.558000f, 55.167500f}, {0.0f, -1.0f, 0.0f}, 450.0f, 100.0f},
    {"Paris_Wall_Light_Interior_05", BistroFixtureKind::Point, {335.847500f, 249.558000f, 194.410000f}, {0.0f, -1.0f, 0.0f}, 450.0f, 100.0f},
    {"Paris_Wall_Light_Interior_06", BistroFixtureKind::Point, {617.428500f, 249.558000f, 305.030500f}, {0.0f, -1.0f, 0.0f}, 450.0f, 100.0f},
    {"Paris_Wall_Light_Interior_02A", BistroFixtureKind::Point, {773.119000f, 241.921000f, -463.082500f}, {0.0f, -1.0f, 0.0f}, 450.0f, 100.0f},

    // Two ceiling fan bottom glass globes identified by diffuse UV island; map_Ke image is absent.
    {"Paris_CeilingFan_01A_Base_mesh_01__2_", BistroFixtureKind::Point, {639.938000f, 351.970000f, 88.496000f}, {0.0f, -1.0f, 0.0f}, 450.0f, 120.0f},
    {"Paris_CeilingFan_01A_Base_mesh_01", BistroFixtureKind::Point, {340.090500f, 351.970000f, -97.992500f}, {0.0f, -1.0f, 0.0f}, 450.0f, 120.0f},

};
