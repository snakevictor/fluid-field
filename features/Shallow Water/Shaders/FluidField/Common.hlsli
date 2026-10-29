#ifndef FLUIDFIELD_COMMON_HLSLI
#define FLUIDFIELD_COMMON_HLSLI

// Fluid Field grid addressing: GridRes x GridRes cells of CellSize world units, centred on the player.
// Toroidal: world cell c always lives in storage texel c mod GridRes, so scrolling only changes GridMin.
// Solver passes work in logical coordinates (0..GridRes-1 from GridMin, the south-west corner; +x east,
// +y north); storage coordinates are only for loads and stores.

#define FLUIDFIELD_GROUP_SIZE 8

cbuffer FluidFieldCB : register(b0)
{
	int2 GridMin;         // World cell of logical texel (0, 0)
	int2 PrevGridMin;     // GridMin of the previous step
	uint GridRes;         // Texels per side, a power of two
	float CellSize;       // World units per texel
	float DeltaTime;      // Seconds simulated by this step
	float VelocityDecay;  // Multiplier applied to advected velocity this step
	uint HistoryValid;    // 0 right after a reset: every texel starts at rest
	uint EmitterCount;    // Entries used in the emitter buffer
	uint2 FluidFieldPad0;
};

/** @brief Positive modulo by GridRes, valid for negative operands. GridRes is a power of two. */
int2 WrapIndex(int2 a)
{
	return a & int(GridRes - 1);
}

int2 StorageToLogical(uint2 storage)
{
	return WrapIndex(int2(storage) - GridMin);
}

uint2 LogicalToStorage(int2 logical)
{
	return uint2(WrapIndex(GridMin + logical));
}

bool IsInsideGrid(int2 logical)
{
	return all(logical >= 0) && all(logical < int(GridRes));
}

/** @brief Loads a texel by logical coordinate, clamping to the grid edge (zero-gradient boundary). */
float2 LoadClamped(Texture2D<float2> tex, int2 logical)
{
	return tex[LogicalToStorage(clamp(logical, 0, int(GridRes) - 1))];
}

/** @brief Loads a texel by logical coordinate, returning 0 outside the grid (open boundary). */
float LoadOrZero(Texture2D<float> tex, int2 logical)
{
	return IsInsideGrid(logical) ? tex[LogicalToStorage(logical)] : 0.0;
}

/** @brief Bilinear sample at a logical position in texel units (texel centres at +0.5). */
float2 SampleBilinear(Texture2D<float2> tex, float2 position)
{
	float2 f = position - 0.5;
	float2 base = floor(f);
	float2 t = f - base;
	int2 i = int2(base);

	float2 v00 = LoadClamped(tex, i);
	float2 v10 = LoadClamped(tex, i + int2(1, 0));
	float2 v01 = LoadClamped(tex, i + int2(0, 1));
	float2 v11 = LoadClamped(tex, i + int2(1, 1));

	return lerp(lerp(v00, v10, t.x), lerp(v01, v11, t.x), t.y);
}

#endif  // FLUIDFIELD_COMMON_HLSLI
