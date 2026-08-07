import callable_data;
import Scene.MaterialParams;
import Scene.MaterialParamsBuffer;

// Opacity test callable for UsdPreviewSurface
// This callable only checks opacity and doesn't perform full BSDF evaluation
[shader("callable")]
void opacity_test_preview_surface(inout OpacityTestCallableData data)
{
    // Read parameters from buffer
    MaterialParams params_blob =
        materialParamsBuffer[data.material_params_index];
    PreviewSurfaceMaterialParams params;

    // Reinterpret buffer data as PreviewSurfaceMaterialParams
    params = reinterpret<PreviewSurfaceMaterialParams>(params_blob);

    // Test opacity based on opacity mode
    // opacityMode: 0 = default, 1 = opaque, 2 = mask

    data.isOpaque = (params.opacity > 0.25f);
}
