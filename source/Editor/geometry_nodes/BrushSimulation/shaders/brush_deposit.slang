// Brush deposit compute shader
// Takes curve positions and emits particles where brush contacts paper
// TODO: Implement particle deposition algorithm

[numthreads(256, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    // Placeholder: to be implemented with fluid simulation model
}
