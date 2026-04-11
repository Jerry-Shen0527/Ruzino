#include "matrix_op.h"
#include "utils/CameraParameters.h"
#include "utils/random.slangh"
import utils.ray;
import utils.Spectrum;
#include "utils/view_cb.h"

import utils.Math.MathHelpers;

RWStructuredBuffer<RayInfo> rays;
RWTexture2D<uint> random_seeds;
RWStructuredBuffer<uint2> pixel_targets;

StructuredBuffer<float3> lens_positions;

// ---[ Constant Buffers ]---
struct Constants
{
    uint2 LaunchDimensions;
    float2 displaySize;
    float displayZ;
    float focalLength;
    float pupilDiameter;
    float viewDistance;
    uint viewCount;
    float2 viewRange;
};
ConstantBuffer<Constants> constants;

// Utility function for 2D cross product
float cross_2d(float2 a, float2 b)
{
    return a.x * b.y - a.y * b.x;
}

// Calculate lens optical axis direction
float3 calc_lens_direction(float3 lensPosition, float viewDistance)
{
    // first point to (0,0,viewDistance)
    float3 viewPosition = float3(0, 0, viewDistance);
    float3 direction = normalize(viewPosition - lensPosition);

    // keep x and z unchanged. ensure a ray from lens position in the x,y direction will pass y = 0 at z = 0.
    float3 init_hit_position_at_z0 = lensPosition - direction * (lensPosition.z / direction.z);
    float3 biased_hit_position_at_z0 = init_hit_position_at_z0;
    biased_hit_position_at_z0.y = 0; // bias y to 0
    float3 newDirection = normalize(biased_hit_position_at_z0 - lensPosition);
    // return direction;
    return newDirection;
}

// Calculate distance from pixel to lens projection center
float distance_to_projection_center(float2 pixelPositionF, float3 lensPosition, float displayZ, float viewDistance)
{
    float3 opticalAxis = calc_lens_direction(lensPosition, viewDistance);
    // Project the optical axis to the display plane
    float3 projectedPosition = lensPosition + opticalAxis * (displayZ - lensPosition.z) / opticalAxis.z;
    // Now we can calculate the distance from the pixel position to the projected position.
    float2 projectedPosition2D = float2(projectedPosition.x, projectedPosition.y);
    float2 delta = pixelPositionF - projectedPosition2D;
    return length(delta);
}

// Find the closest lens to a pixel position
uint find_closest_lens(float2 pixelPositionF, StructuredBuffer<float3> lensPositions, uint lensCount, float displayZ, float viewDistance)
{
    uint closestLensIndex = 0;
    float minDistance = distance_to_projection_center(pixelPositionF, lensPositions[0], displayZ, viewDistance);

    for (uint i = 1; i < lensCount; i++)
    {
        float3 lensPosition = lensPositions[i];
        float distance = distance_to_projection_center(pixelPositionF, lensPosition, displayZ, viewDistance);
        if (distance < minDistance)
        {
            minDistance = distance;
            closestLensIndex = i;
        }
    }

    return closestLensIndex;
}

// Calculate conjugate point for a given lens and display position
float3 calculate_conjugate_point(float3 lensPosition, float3 displayPosition, float focalLength)
{
    float3 opticalAxis = calc_lens_direction(lensPosition, 0); // viewDistance not used in this context

    // Simple conjugate point calculation: intersection with z=0 plane
    float3 dir = normalize(displayPosition - lensPosition);
    float3 conjugatePoint = lensPosition - dir * (lensPosition.z / dir.z);

    return conjugatePoint;
}

// Find valid view range for a lens and conjugate point
struct ViewRange
{
    int first_in_angle;
    int last_in_angle;
};

ViewRange find_valid_view_range(float3 lensPosition, float3 conjugatePoint, uint viewCount,
                               float2 viewRange, float viewDistance, float pupilDiameter)
{
    ViewRange result;
    result.first_in_angle = -1;
    result.last_in_angle = -1;

    for (int i = 0; i < viewCount; i++)
    {
        // Calculate the view position based on the view index and range
        float viewPositionX = viewRange.x + (viewRange.y - viewRange.x) * (i / (float)(viewCount - 1));
        float viewportZ = viewDistance;

        // Check if this point is in the angle from the pupil to the conjugate point
        float lens_center_x = lensPosition.x;
        float pupil_left_x = lens_center_x - pupilDiameter / 2.0f;
        float pupil_right_x = lens_center_x + pupilDiameter / 2.0f;

        // Account for z.
        float2 pupilLeft = float2(pupil_left_x, lensPosition.z);
        float2 pupilRight = float2(pupil_right_x, lensPosition.z);
        float2 conjugatePoint2D = float2(conjugatePoint.x, conjugatePoint.z);
        float2 viewPosition = float2(viewPositionX, viewportZ);

        // Check if the view position is within the angle formed by the pupil and the conjugate point
        float2 leftVector = pupilLeft - viewPosition;
        float2 rightVector = pupilRight - viewPosition;
        float2 viewVector = conjugatePoint2D - viewPosition;
        float leftAngle = cross_2d(leftVector, viewVector);
        float rightAngle = cross_2d(rightVector, viewVector);

        if (leftAngle * rightAngle < 0)
        {
            // The view position is within the angle formed by the pupil and the conjugate point
            if (result.first_in_angle == -1)
                result.first_in_angle = i;
            result.last_in_angle = i;
        }
    }

    return result;
}

// Create a ray from view position to conjugate point
RayInfo create_ray(float3 conjugatePoint, int viewIndex, uint viewCount, float2 viewRange, float viewDistance)
{
    float viewPositionX = viewRange.x + (viewRange.y - viewRange.x) * (viewIndex / (float)(viewCount - 1));
    float3 rayOrigin = float3(viewPositionX, 0, viewDistance);

    RayInfo ray;
    ray.Origin = rayOrigin;
    ray.Direction = normalize(conjugatePoint - rayOrigin);
    ray.TMin = 0;
    ray.TMax = 1000;
    ray.throughput = RGBSpectrum(1.0f); // Initialize throughput to white

    // swap y and z for ray origin and direction
    ray.Origin.yz = ray.Origin.zy;
    ray.Origin.x *= -1.0f; // Invert y to match the coordinate system
    ray.Direction.yz = ray.Direction.zy;
    ray.Direction.x *= -1.0f; // Invert y to match the coordinate system

    return ray;
}

// Create an invalid ray when no valid view is found
RayInfo create_invalid_ray(float3 conjugatePoint, float3 lensPosition)
{
    RayInfo invalidRay;
    invalidRay.Origin = float3(0, 0, 0);
    invalidRay.Direction = normalize(conjugatePoint - lensPosition);
    invalidRay.Direction.yz = invalidRay.Direction.zy;
    invalidRay.TMin = 0;
    invalidRay.TMax = 0;
    invalidRay.throughput = RGBSpectrum(0.0f); // Set throughput to black
    return invalidRay;
}


[Differentiable]
void pixel_position_to_ray_with_fixed_lens_pos(float2 pixelPositionF, float3 lensPosition,
                             float displayZ, float focalLength, float pupilDiameter, float viewDistance,
                             uint viewCount, float2 viewRange, inout float3 rayOrigin, inout float3 rayDirection)
{
    uint lensCount;
    uint foo;

    // Step 1: Calculate conjugate point
    float3 displayPosition = float3(pixelPositionF, displayZ);
    float3 conjugatePoint = calculate_conjugate_point(lensPosition, displayPosition, focalLength);

    // Step 2: Find valid view range
    ViewRange validViewRange = find_valid_view_range(lensPosition, conjugatePoint, viewCount,
                                                   viewRange, viewDistance, pupilDiameter);

    // Step 3: Generate ray based on valid view range
    RayInfo ray;
    if (validViewRange.first_in_angle == -1)
    {
        // No valid view found
        ray = create_invalid_ray(conjugatePoint, lensPosition);
        rayOrigin = ray.Origin;
        rayDirection = ray.Direction;
    }
    else
    {
        // Sample a random view within the valid range
        float random = 0;
        int viewIndex = validViewRange.first_in_angle + (int)((validViewRange.last_in_angle - validViewRange.first_in_angle) * random);
        ray = create_ray(conjugatePoint, viewIndex, viewCount, viewRange, viewDistance);
        rayOrigin = ray.Origin;
        rayDirection = ray.Direction;
    }
}


// Convert pixel position to ray direction and generate RayInfo
RayInfo pixel_position_to_ray(float2 pixelPositionF, no_diff inout uint seed, StructuredBuffer<float3> lensPositions,
                             float displayZ, float focalLength, float pupilDiameter, float viewDistance,
                             uint viewCount, float2 viewRange)
{
    uint lensCount;
    uint foo;
    lensPositions.GetDimensions(lensCount, foo);

    uint closestLensIndex = find_closest_lens(pixelPositionF, lensPositions, lensCount, displayZ, viewDistance);

    float3 lensPosition = lensPositions[closestLensIndex];
    float3 rayOrigin, rayDirection;
    pixel_position_to_ray_with_fixed_lens_pos(pixelPositionF, lensPosition,
                             displayZ, focalLength, pupilDiameter, viewDistance, viewCount, viewRange,
                             rayOrigin, rayDirection);

    RayInfo ray;
    ray.Origin = rayOrigin;
    ray.Direction = rayDirection;
    ray.TMin = 0;
    ray.TMax = 1000;
    ray.throughput = RGBSpectrum(1.0f); // Initialize throughput to white

    return ray;
}

// Core processing function that handles the entire light field ray generation logic
[Differentiable]
RayInfo process_pixel_to_ray(uint2 launchIndex, inout no_diff uint seed, StructuredBuffer<float3> lensPositions,
                            uint2 launchDimensions, float2 displaySize, float displayZ,
                            float focalLength, float pupilDiameter, float viewDistance,
                            uint viewCount, float2 viewRange)
{
    // Step 0: Scale pixel to display size
    float2 pixelPositionF = (float2(launchIndex) - random_float2(seed) + 0.5f) * displaySize / launchDimensions - displaySize * 0.5f;

    // Convert pixel position to ray
    return pixel_position_to_ray(pixelPositionF, seed, lensPositions, displayZ, focalLength,
                               pupilDiameter, viewDistance, viewCount, viewRange);
}

[numthreads(8, 8, 1)]
void main(uint2 DTid: SV_DispatchThreadID)
{
    uint2 LaunchIndex = DTid.xy;
    uint2 LaunchDimensions = constants.LaunchDimensions;

    if (!all(LaunchIndex.xy < LaunchDimensions.xy))
        return; // Exit if the thread index is out of bounds

    uint seed = random_seeds[LaunchIndex];

    // Process pixel to generate ray using core processing function
    RayInfo ray = process_pixel_to_ray(LaunchIndex, seed, lens_positions,
                                     constants.LaunchDimensions, constants.displaySize, constants.displayZ,
                                     constants.focalLength, constants.pupilDiameter, constants.viewDistance,
                                     constants.viewCount, constants.viewRange);

    // Store results to output buffers
    uint rayIndex = LaunchIndex.x * LaunchDimensions.y + LaunchIndex.y;
    rays[rayIndex] = ray;
    pixel_targets[rayIndex] = LaunchIndex;
    random_seeds[LaunchIndex] = seed;
}
