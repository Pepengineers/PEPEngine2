// Artistic horizontal velocity field in cm/s; not a stem dynamics solver.
static const uint Stage = WIND_STAGE;
cbuffer WindParameters : register(b0)
{
    float4 Area;
    float4 Force;
    float4 Gust;
    float4 Solver;
    uint Resolution;
    uint3 Padding;
};
StructuredBuffer<float4> Input : register(t0);
StructuredBuffer<float4> Pressure : register(t1);
RWStructuredBuffer<float4> Output : register(u0);

uint Cell(int2 p)
{
    p = clamp(p, 0, int(Resolution) - 1);
    return uint(p.y) * Resolution + uint(p.x);
}
float4 Bilinear(float2 p)
{
    p = clamp(p, 0, float(Resolution - 1));
    int2 cell = int2(floor(p));
    float2 f = frac(p);
    return lerp(lerp(Input[Cell(cell)], Input[Cell(cell + int2(1, 0))], f.x),
        lerp(Input[Cell(cell + int2(0, 1))], Input[Cell(cell + 1)], f.x), f.y);
}
float2 LimitVelocity(float2 velocity)
{
    return velocity * min(1, Solver.z / max(length(velocity), 1e-6f));
}
[numthreads(8, 8, 1)]
void CS(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= Resolution)) return;
    int2 p = int2(id.xy);
    uint index = Cell(p);
    if (Stage == 0) { Output[index] = 0; return; }
    if (Stage == 5) { Output[index] = Input[index]; return; }
    float2 spacing = Area.zw * 2 / Resolution;
    if (Stage == 1)
    {
        float2 velocity = Input[index].xy;
        velocity = Bilinear(float2(p) - velocity * Gust.w / spacing).xy * exp(-Solver.y * Gust.w);
        float2 world = Area.xy + ((float2(p) + 0.5f) / Resolution * 2 - 1) * Area.zw;
        float wave = sin(6.2831853f * (Gust.x * Gust.z + dot(world, Force.xy) / Gust.y));
        float2 target = Force.xy * Force.z * (1 + Force.w * wave);
        velocity = lerp(velocity, target, 1 - exp(-Solver.x * Gust.w));
        // Closed walls: no normal flux, free tangential slip.
        if (p.x == 0 || p.x == int(Resolution) - 1) velocity.x = 0;
        if (p.y == 0 || p.y == int(Resolution) - 1) velocity.y = 0;
        Output[index] = float4(LimitVelocity(velocity), 0, 0);
    }
    else if (Stage == 2)
    {
        float divergence = (Input[Cell(p + int2(1, 0))].x - Input[Cell(p - int2(1, 0))].x) / (2 * spacing.x) +
            (Input[Cell(p + int2(0, 1))].y - Input[Cell(p - int2(0, 1))].y) / (2 * spacing.y);
        Output[index] = float4(divergence, 0, 0, 0);
    }
    else if (Stage == 3)
    {
        float2 weights = rcp(spacing * spacing);
        float x = Pressure[Cell(p + int2(1, 0))].x + Pressure[Cell(p - int2(1, 0))].x;
        float y = Pressure[Cell(p + int2(0, 1))].x + Pressure[Cell(p - int2(0, 1))].x;
        // Neumann walls; pin one cell to remove the pressure null space.
        float pressure = (x * weights.x + y * weights.y - Input[index].x) / (2 * (weights.x + weights.y));
        Output[index] = index == 0 ? 0 : float4(pressure, 0, 0, 0);
    }
    else if (Stage == 4)
    {
        float2 gradient = float2(Pressure[Cell(p + int2(1, 0))].x - Pressure[Cell(p - int2(1, 0))].x,
            Pressure[Cell(p + int2(0, 1))].x - Pressure[Cell(p - int2(0, 1))].x) / (2 * spacing);
        float2 velocity = LimitVelocity(Input[index].xy - gradient);
        if (p.x == 0 || p.x == int(Resolution) - 1) velocity.x = 0;
        if (p.y == 0 || p.y == int(Resolution) - 1) velocity.y = 0;
        Output[index] = float4(velocity, 0, 0);
    }
}
