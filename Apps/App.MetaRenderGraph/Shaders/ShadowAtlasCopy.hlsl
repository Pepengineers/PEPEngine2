// Layout-only copy of the real U4 depth results into the legacy lighting views.
cbuffer CopyRegion : register(b0) { uint Size, OriginX, OriginY, Slice; };
Texture2D<float> SourceDepth : register(t0);
RWTexture2DArray<float> Destination : register(u0);
[numthreads(8,8,1)]
void CS(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= Size)) return;
    Destination[uint3(id.x + OriginX, id.y + OriginY, Slice)] = SourceDepth[id.xy];
}
