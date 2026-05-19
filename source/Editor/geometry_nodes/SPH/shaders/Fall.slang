
RWBuffer<float3> positions : register(u0);
RWBuffer<float3> velocities : register(u1);


[numthreads(32, 1, 1)]
void main(int2 i_globalIdx : SV_DispatchThreadID)
{
    positions[i_globalIdx.x] += velocities[i_globalIdx.x];
    velocities[i_globalIdx.x] = float3(-0.0001, 0, 0);
}



[numthreads(32, 1, 1)]
void main2(int2 i_globalIdx : SV_DispatchThreadID)
{
    positions[i_globalIdx.x] += velocities[i_globalIdx.x];
    velocities[i_globalIdx.x] = float3(-0.0001, 0, 0);
}