// The native renderer's entry points for Intel's XeGTAO (XeGTAO.h and
// XeGTAO.hlsli, unmodified from https://github.com/GameTechDev/XeGTAO, MIT;
// see README.md). In the place of the sample's vaGTAO.hlsl: the same passes,
// with Vulkan bindings and no engine includes. Compiled by
// ../compile_xegtao.py; used by --xegtao (passes.cpp, ApplyXeGTAO).
//
// Every kernel shares one descriptor set layout (set 0), so each resource
// has its own binding:
//   1 the view's depth (BuildDepthMotion) (R32G32F; .r is the depth as drawn)
//   2 a point sampler, clamped
//   3-7 the working depth's MIPs 0-4, written (r32f)
//   8 the working depth, read (all MIPs)
//   9 the normals, read; 10 the normals, written (r32ui, 11_11_10)
//   11 the AO term, written by the main pass; 12 the edges, written (r32f)
//   13 the AO term, read by a denoise pass; 14 the edges, read
//   15 a denoise pass's output (r32ui)
// The constants (GTAOConstants, 96 bytes) are push constants.

#ifndef XE_GTAO_USE_HALF_FLOAT_PRECISION
#define XE_GTAO_USE_HALF_FLOAT_PRECISION 0
#endif

// From the sample's vaShaderCore.h (shader side).
#define VA_SATURATE saturate

#include "XeGTAO.h"

[[vk::push_constant]] cbuffer GTAOConstantBuffer
{
    GTAOConstants g_GTAOConsts;
}

#include "XeGTAO.hlsli"

[[vk::binding(1, 0)]] Texture2D<float>          g_srcRawDepth;
[[vk::binding(2, 0)]] SamplerState              g_samplerPointClamp;
[[vk::binding(3, 0)]] RWTexture2D<lpfloat>      g_outWorkingDepthMIP0;
[[vk::binding(4, 0)]] RWTexture2D<lpfloat>      g_outWorkingDepthMIP1;
[[vk::binding(5, 0)]] RWTexture2D<lpfloat>      g_outWorkingDepthMIP2;
[[vk::binding(6, 0)]] RWTexture2D<lpfloat>      g_outWorkingDepthMIP3;
[[vk::binding(7, 0)]] RWTexture2D<lpfloat>      g_outWorkingDepthMIP4;
[[vk::binding(8, 0)]] Texture2D<lpfloat>        g_srcWorkingDepth;
[[vk::binding(9, 0)]] Texture2D<uint>           g_srcNormalmap;
[[vk::binding(10, 0)]] RWTexture2D<uint>        g_outNormalmap;
[[vk::binding(11, 0)]] RWTexture2D<uint>        g_outWorkingAOTerm;
[[vk::binding(12, 0)]] RWTexture2D<unorm float> g_outWorkingEdges;
[[vk::binding(13, 0)]] Texture2D<uint>          g_srcWorkingAOTerm;
[[vk::binding(14, 0)]] Texture2D<lpfloat>       g_srcWorkingEdges;
[[vk::binding(15, 0)]] RWTexture2D<uint>        g_outFinalAOTerm;

// The normals made from depth (CSGenerateNormals), packed 11_11_10.
lpfloat3 LoadNormal( int2 pos )
{
    uint packedInput = g_srcNormalmap.Load( int3( pos, 0 ) ).x;
    float3 unpackedOutput = XeGTAO_R11G11B10_UNORM_to_FLOAT3( packedInput );
    return (lpfloat3)normalize( unpackedOutput * 2.0.xxx - 1.0.xxx );
}

// A Hilbert curve driving R2, made in place (no lookup texture), as the
// sample's. temporalIndex is 0: there's no TAA to spread the noise over
// frames.
lpfloat2 SpatioTemporalNoise( uint2 pixCoord, uint temporalIndex )
{
    uint index = HilbertIndex( pixCoord.x, pixCoord.y );
    index += 288 * ( temporalIndex % 64 );
    return lpfloat2( frac( 0.5 + index * float2( 0.75487766624669276005, 0.5698402909980532659114 ) ) );
}

// 8x8 threads of 2x2 pixels: dispatch (width + 15) / 16, (height + 15) / 16.
[numthreads(8, 8, 1)]
void CSPrefilterDepths16x16( uint2 dispatchThreadID : SV_DispatchThreadID, uint2 groupThreadID : SV_GroupThreadID )
{
    XeGTAO_PrefilterDepths16x16( dispatchThreadID, groupThreadID, g_GTAOConsts, g_srcRawDepth, g_samplerPointClamp, g_outWorkingDepthMIP0, g_outWorkingDepthMIP1, g_outWorkingDepthMIP2, g_outWorkingDepthMIP3, g_outWorkingDepthMIP4 );
}

[numthreads(XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1)]
void CSGenerateNormals( const uint2 pixCoord : SV_DispatchThreadID )
{
    float3 viewspaceNormal = XeGTAO_ComputeViewspaceNormal( pixCoord, g_GTAOConsts, g_srcRawDepth, g_samplerPointClamp );
    g_outNormalmap[ pixCoord ] = XeGTAO_FLOAT3_to_R11G11B10_UNORM( saturate( viewspaceNormal * 0.5 + 0.5 ) );
}

// The main pass by quality: slices and steps per slice, as the sample's.
[numthreads(XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1)]
void CSGTAOLow( const uint2 pixCoord : SV_DispatchThreadID )
{
    XeGTAO_MainPass( pixCoord, 1, 2, SpatioTemporalNoise( pixCoord, g_GTAOConsts.NoiseIndex ), LoadNormal( pixCoord ), g_GTAOConsts, g_srcWorkingDepth, g_samplerPointClamp, g_outWorkingAOTerm, g_outWorkingEdges );
}

[numthreads(XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1)]
void CSGTAOMedium( const uint2 pixCoord : SV_DispatchThreadID )
{
    XeGTAO_MainPass( pixCoord, 2, 2, SpatioTemporalNoise( pixCoord, g_GTAOConsts.NoiseIndex ), LoadNormal( pixCoord ), g_GTAOConsts, g_srcWorkingDepth, g_samplerPointClamp, g_outWorkingAOTerm, g_outWorkingEdges );
}

[numthreads(XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1)]
void CSGTAOHigh( const uint2 pixCoord : SV_DispatchThreadID )
{
    XeGTAO_MainPass( pixCoord, 3, 3, SpatioTemporalNoise( pixCoord, g_GTAOConsts.NoiseIndex ), LoadNormal( pixCoord ), g_GTAOConsts, g_srcWorkingDepth, g_samplerPointClamp, g_outWorkingAOTerm, g_outWorkingEdges );
}

[numthreads(XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1)]
void CSGTAOUltra( const uint2 pixCoord : SV_DispatchThreadID )
{
    XeGTAO_MainPass( pixCoord, 9, 3, SpatioTemporalNoise( pixCoord, g_GTAOConsts.NoiseIndex ), LoadNormal( pixCoord ), g_GTAOConsts, g_srcWorkingDepth, g_samplerPointClamp, g_outWorkingAOTerm, g_outWorkingEdges );
}

// Two horizontal pixels a thread: dispatch (width + 15) / 16, (height + 7) / 8.
[numthreads(XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1)]
void CSDenoisePass( const uint2 dispatchThreadID : SV_DispatchThreadID )
{
    const uint2 pixCoordBase = dispatchThreadID * uint2( 2, 1 );
    XeGTAO_Denoise( pixCoordBase, g_GTAOConsts, g_srcWorkingAOTerm, g_srcWorkingEdges, g_samplerPointClamp, g_outFinalAOTerm, false );
}

[numthreads(XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1)]
void CSDenoiseLastPass( const uint2 dispatchThreadID : SV_DispatchThreadID )
{
    const uint2 pixCoordBase = dispatchThreadID * uint2( 2, 1 );
    XeGTAO_Denoise( pixCoordBase, g_GTAOConsts, g_srcWorkingAOTerm, g_srcWorkingEdges, g_samplerPointClamp, g_outFinalAOTerm, true );
}
