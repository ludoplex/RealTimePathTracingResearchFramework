// https://developer.nvidia.com/rtx/raytracing/dxr/DX12-Raytracing-tutorial-Part-2
struct HitInfo {
	float4 color_dist;
};

// Attributes output by the raytracing when hitting a surface,
// here the barycentric coordinates
struct Attributes {
	float2 bary;
};

// Raytracing output texture, accessed as a UAV
RWTexture2D<float4> output : register(u0);

// Raytracing acceleration structure, accessed as a SRV
RaytracingAccelerationStructure scene : register(t0);

// View params buffer
cbuffer ViewParams : register(b0) {
	float4 cam_pos;
	float4 cam_du;
	float4 cam_dv;
	float4 cam_dir_top_left;
}

float linear_to_srgb(float x) {
	if (x <= 0.0031308f) {
		return 12.92f * x;
	}
	return 1.055f * pow(x, 1.f / 2.4f) - 0.055f;
}

[shader("raygeneration")] 
void RayGen() {
	// Initialize the ray payload
	HitInfo payload;
	payload.color_dist = float4(0, 0, 0, 1);

	// Get the location within the dispatched 2D grid of work items
	// (often maps to pixels, so this could represent a pixel coordinate).
	uint2 pixel = DispatchRaysIndex().xy;
	float2 dims = float2(DispatchRaysDimensions().xy);

	// The example traces an orthographic view essentially (or NDC "projection")
	float2 d = pixel / dims;
	RayDesc ray;
	ray.Origin = cam_pos.xyz;
	ray.Direction = normalize(d.x * cam_du.xyz + d.y * cam_dv.xyz + cam_dir_top_left.xyz);
	ray.TMin = 0;
	ray.TMax = 1e20f;

	TraceRay(scene, 0, 0xff, 0, 0, 0, ray, payload);

	output[pixel] = float4(linear_to_srgb(payload.color_dist.r),
		linear_to_srgb(payload.color_dist.g),
		linear_to_srgb(payload.color_dist.b), 1.f);
}

[shader("miss")]
void Miss(inout HitInfo payload : SV_RayPayload) {
	payload.color_dist = float4(0, 0, 0, 0);
}

[shader("closesthit")] 
void ClosestHit(inout HitInfo payload, Attributes attrib) {
	float3 barycoords = float3(1.f - attrib.bary.x - attrib.bary.y,
		attrib.bary.x, attrib.bary.y);
	payload.color_dist = float4(barycoords, RayTCurrent());
}

