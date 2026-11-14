// This file is part of the FidelityFX SDK.
//
// Copyright (C) 2024 Advanced Micro Devices, Inc.
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#ifndef FFX_FRAMEINTERPOLATION_H
#define FFX_FRAMEINTERPOLATION_H

#define SCTURBO_SHADOW_SWEEP 1 // SC-Turbo: see computeInterpolatedColor

struct InterpolationSourceColor
{
    FfxFloat32x3 fRaw;
    FfxFloat32x3 fLinear;
    FfxFloat32   fBilinearWeightSum;
};

InterpolationSourceColor NewInterpolationSourceColor()
{
    InterpolationSourceColor c;
    c.fRaw = FfxFloat32x3(0.0, 0.0, 0.0);
    c.fLinear = FfxFloat32x3(0.0, 0.0, 0.0);
    c.fBilinearWeightSum = 0.0;
    return c;
}

InterpolationSourceColor SampleTextureBilinear(FfxBoolean isCurrent, FfxFloat32x2 fUv, FfxFloat32x2 fMotionVector, FfxInt32x2 texSize)
{
    InterpolationSourceColor result = NewInterpolationSourceColor();

    FfxFloat32x2 fReprojectedUv = fUv + fMotionVector;
    BilinearSamplingData bilinearInfo = GetBilinearSamplingData(fReprojectedUv, texSize);

    FfxFloat32x3 fColor = FfxFloat32x3(0.0, 0.0, 0.0);
    FfxFloat32 fWeightSum = 0.0f;
    for (FfxInt32 iSampleIndex = 0; iSampleIndex < 4; iSampleIndex++) {

        const FfxInt32x2 iOffset = bilinearInfo.iOffsets[iSampleIndex];
        const FfxInt32x2 iSamplePos = bilinearInfo.iBasePos + iOffset;

        if (IsInRect(iSamplePos, InterpolationRectBase(), InterpolationRectSize()))
        {
            FfxFloat32 fWeight = bilinearInfo.fWeights[iSampleIndex];

            if (isCurrent)
                fColor += LoadCurrentBackbuffer(iSamplePos).rgb * fWeight;
            else
                fColor += LoadPreviousBackbuffer(iSamplePos).rgb * fWeight;
            fWeightSum += fWeight;
        }
    }

    //normalize colors
    fColor = (fWeightSum != 0.0f) ? fColor / fWeightSum : FfxFloat32x3(0.0f, 0.0f, 0.0f);

    result.fRaw               = fColor;
    result.fLinear            = RawRGBToLinear(fColor);
    result.fBilinearWeightSum = fWeightSum;

    return result;
}

#ifdef SCTURBO_SHADOW_SWEEP
// Brightness of the surface point at fUv (plus the vectors that line the two frames up) in the
// previous (x) and the current (y) frame; negative outside the picture.
FfxFloat32x2 SweepLuma(FfxFloat32x2 fUv, FfxFloat32x2 fPrevMv, FfxFloat32x2 fCurrMv, FfxFloat32x2 fSize)
{
    const FfxInt32x2 iPrevPos = FfxInt32x2((fUv + fPrevMv) * fSize);
    const FfxInt32x2 iCurrPos = FfxInt32x2((fUv + fCurrMv) * fSize);
    if (!IsInRect(iPrevPos, InterpolationRectBase(), InterpolationRectSize()) ||
        !IsInRect(iCurrPos, InterpolationRectBase(), InterpolationRectSize()))
    {
        return FfxFloat32x2(-1.0f, -1.0f);
    }
    const FfxFloat32x3 fLuma = FfxFloat32x3(0.299f, 0.587f, 0.114f);
    return FfxFloat32x2(dot(LoadPreviousBackbuffer(iPrevPos).rgb, fLuma), dot(LoadCurrentBackbuffer(iCurrPos).rgb, fLuma)) + 0.02f;
}

// The same averaged over a small cross, so that thin dark detail (joints between tiles) does not
// pass for shadow.
FfxFloat32x2 SweepLumaArea(FfxFloat32x2 fUv, FfxFloat32x2 fPrevMv, FfxFloat32x2 fCurrMv, FfxFloat32x2 fSize)
{
    FfxFloat32x2 fSum = FfxFloat32x2(0.0f, 0.0f);
    FfxFloat32 fCount = 0.0f;
    for (FfxInt32 i = 0; i < 9; i++)
    {
        const FfxFloat32x2 fOffset = FfxFloat32x2(FfxFloat32(i % 3) - 1.0f, FfxFloat32(i / 3) - 1.0f) * 7.0f;
        const FfxFloat32x2 fThere = SweepLuma(fUv + fOffset / fSize, fPrevMv, fCurrMv, fSize);
        if (fThere.x > 0.0f)
        {
            fSum += fThere;
            fCount += 1.0f;
        }
    }
    return fCount > 0.0f ? fSum / fCount : FfxFloat32x2(-1.0f, -1.0f);
}
#endif

void updateInPaintingWeight(inout FfxFloat32 fInPaintingWeight, FfxFloat32 fFactor)
{
    fInPaintingWeight = ffxSaturate(ffxMax(fInPaintingWeight, fFactor));
}

void computeInterpolatedColor(FfxUInt32x2 iPxPos, out FfxFloat32x3 fInterpolatedColor, inout FfxFloat32 fInPaintingWeight)
{
    const FfxFloat32x2 fUvInInterpolationRect = (FfxFloat32x2(iPxPos - InterpolationRectBase()) + 0.5f) / InterpolationRectSize();
    const FfxFloat32x2 fUvInScreenSpace       = (FfxFloat32x2(iPxPos) + 0.5f) / DisplaySize();
    const FfxFloat32x2 fLrUvInInterpolationRect = fUvInInterpolationRect * (FfxFloat32x2(RenderSize()) / GetMaxRenderSize());

    const FfxFloat32x2 fUvLetterBoxScale = FfxFloat32x2(InterpolationRectSize()) / DisplaySize();

    // game MV are top left aligned, the function scales them to render res UV
    VectorFieldEntry gameMv;
    LoadInpaintedGameFieldMv(fUvInInterpolationRect, gameMv);

    // OF is done on the back buffers which already have black bars
    VectorFieldEntry ofMv;
    SampleOpticalFlowMotionVectorField(fUvInScreenSpace, ofMv);

    // Binarize disucclusion factor
    FfxFloat32x2 fDisocclusionFactor = FfxFloat32x2(FFX_EQUAL(ffxSaturate(SampleDisocclusionMask(fLrUvInInterpolationRect).xy), FfxFloat32x2(1.0, 1.0)));

    // Lichii: the vectors scaled to the generated frame's time (were +-fMotionVector).
    InterpolationSourceColor fPrevColorGame = SampleTextureBilinear(false, fUvInScreenSpace, +gameMv.fMotionVector * PreviousFrameScale() * fUvLetterBoxScale, DisplaySize()); // Get in previous frame buffer, the color of interpolated pixel
    InterpolationSourceColor fCurrColorGame = SampleTextureBilinear(true, fUvInScreenSpace, -gameMv.fMotionVector * CurrentFrameScale() * fUvLetterBoxScale, DisplaySize()); // Get color in current framebuffer, of color of interpolated pixel

    InterpolationSourceColor fPrevColorOF = SampleTextureBilinear(false, fUvInScreenSpace, +ofMv.fMotionVector * PreviousFrameScale() * fUvLetterBoxScale, DisplaySize());
    InterpolationSourceColor fCurrColorOF = SampleTextureBilinear(true, fUvInScreenSpace, -ofMv.fMotionVector * CurrentFrameScale() * fUvLetterBoxScale, DisplaySize());

    FfxFloat32 fDisoccludedFactor = 0.0f;

    // Disocclusion logic
    {
        fDisocclusionFactor.x *= FfxFloat32(!gameMv.bPosOutside); // fDisocclusionFactor.x of 1 means the pos of interpolated pixel is within bounds of previous frame.
        fDisocclusionFactor.y *= FfxFloat32(!gameMv.bNegOutside); // fDisocclusionFactor.y of 1 means the pos of interpolated pixel is within bounds of current frame

        // Inpaint in bi-directional disocclusion areas
        updateInPaintingWeight(fInPaintingWeight, FfxFloat32(length(fDisocclusionFactor) <= FFX_FRAMEINTERPOLATION_EPSILON));

        // Lichii: blends at the generated frame's time (was 0.5, +0.5, -0.5), still all previous
        // or all current where the other one is disoccluded.
        FfxFloat32 t = InterpolationTime();
        t += (1.0f - InterpolationTime()) * (1 - (fDisocclusionFactor.x));
        t -= InterpolationTime() * (1 - (fDisocclusionFactor.y));
        // Say if fDisocclusionFactor.x is 1 and fDisocclusionFactor.y = 0, then t will be 0. fInterpolatedColor will be entirely from fPrevColorGame 
        fInterpolatedColor = ffxLerp(fPrevColorGame.fRaw, fCurrColorGame.fRaw, ffxSaturate(t));
        fDisoccludedFactor = ffxSaturate(1 - ffxMin(fDisocclusionFactor.x, fDisocclusionFactor.y));

        if (fPrevColorGame.fBilinearWeightSum == 0.0f)
        {
            fInterpolatedColor = fCurrColorGame.fRaw;
        }
        else if (fCurrColorGame.fBilinearWeightSum == 0.0f)
        {
            fInterpolatedColor = fPrevColorGame.fRaw;
        }
        if (fPrevColorGame.fBilinearWeightSum == 0 && fCurrColorGame.fBilinearWeightSum == 0)
        {
            fInPaintingWeight = 1.0f;
        }
    }

    {

        // Lichii: the generated frame's time (was 0.5).
        FfxFloat32 ofT = InterpolationTime();

        if (fPrevColorOF.fBilinearWeightSum > 0 && fCurrColorOF.fBilinearWeightSum > 0)
        {
            ofT = InterpolationTime();
        }
        else if (fPrevColorOF.fBilinearWeightSum > 0)
        {
            ofT = 0;
        } else {
            ofT = 1;
        }

        const FfxFloat32x3 ofColor = ffxLerp(fPrevColorOF.fRaw, fCurrColorOF.fRaw, ofT);

        FfxFloat32 fOF_Sim = NormalizedDot3(fPrevColorOF.fRaw, fCurrColorOF.fRaw);
        FfxFloat32 fGame_Sim = NormalizedDot3(fPrevColorGame.fRaw, fCurrColorGame.fRaw);

        fGame_Sim = ffxLerp(ffxMax(FFX_FRAMEINTERPOLATION_EPSILON, fGame_Sim), 1.0f, ffxSaturate(fDisoccludedFactor));
        FfxFloat32 fGameMvBias = ffxPow(ffxSaturate(fGame_Sim / ffxMax(FFX_FRAMEINTERPOLATION_EPSILON, fOF_Sim)), 1.0f);

        const FfxFloat32 fFrameIndexFactor = FfxFloat32(FrameIndexSinceLastReset() < 10);
        fGameMvBias = ffxLerp(fGameMvBias, 1.0f, fFrameIndexFactor);

        fInterpolatedColor = ffxLerp(ofColor, fInterpolatedColor, ffxSaturate(fGameMvBias));

        // Lichii: where neither the game's vectors nor optical flow bring the two frames' colours
        // together (motion neither follows, like foliage the vertex shader sways in the wind), a
        // blend shows both frames at once: show the nearer frame's colour instead.
        const FfxFloat32 fGameError = length(fPrevColorGame.fRaw - fCurrColorGame.fRaw) * (1.0f - ffxSaturate(fDisoccludedFactor));
        const FfxFloat32 fOFError = length(fPrevColorOF.fRaw - fCurrColorOF.fRaw);
        const FfxBoolean bAllSampled = fPrevColorGame.fBilinearWeightSum > 0.0f && fCurrColorGame.fBilinearWeightSum > 0.0f &&
                                       fPrevColorOF.fBilinearWeightSum > 0.0f && fCurrColorOF.fBilinearWeightSum > 0.0f;
        FfxFloat32 fMismatch = ffxSaturate((ffxMin(fGameError, fOFError) - 0.08f) / 0.12f) * (1.0f - fFrameIndexFactor) * FfxFloat32(bAllSampled);
#ifdef SCTURBO_SHADOW_BLEND // tried 2026-11-13: a hard shadow then shows twice (ghost); off
        // SC-Turbo: where the two frames differ in brightness only (same colour direction: a shadow
        // or a light passing over a surface the vectors follow correctly), the nearer frame's colour
        // would make the shadow step at the game's frame rate. The blend over time is kept there:
        // the shadow's soft edge fades from its old place to its new one.
        {
            const FfxFloat32x3 fP = fPrevColorGame.fRaw + FfxFloat32x3(0.02f, 0.02f, 0.02f);
            const FfxFloat32x3 fC = fCurrColorGame.fRaw + FfxFloat32x3(0.02f, 0.02f, 0.02f);
            const FfxFloat32 fCosine = dot(fP, fC) / (length(fP) * length(fC));
            fMismatch *= 1.0f - ffxSaturate((fCosine - 0.985f) / 0.01f);
        }
#endif
        const FfxBoolean bPrevious = InterpolationTime() < 0.5f;
        const FfxFloat32x3 fNearestGame = bPrevious ? fPrevColorGame.fRaw : fCurrColorGame.fRaw;
        const FfxFloat32x3 fNearestOF = bPrevious ? fPrevColorOF.fRaw : fCurrColorOF.fRaw;
        const FfxFloat32x3 fNearest = ffxLerp(fNearestOF, fNearestGame, ffxSaturate(fGameMvBias));
        fInterpolatedColor = ffxLerp(fInterpolatedColor, fNearest, fMismatch);

#ifdef SCTURBO_SHADOW_SWEEP
        // SC-Turbo: a shadow moving over a surface. The game's vectors line the surface up, so this
        // pixel is the same surface point in both frames, lit in one and shadowed in the other: it
        // flips at some moment between the two frames. Flipping every such pixel at the same moment
        // (the nearer frame's colour, above) makes the shadow step at the game's frame rate, and a
        // blend shows it twice. The moment is estimated per pixel instead: the band of changed
        // pixels lies between surface that keeps this pixel's new state (crossed first) and surface
        // that keeps its old state (crossed last), so the distances to the two give the crossing
        // time, and the shadow's edge sweeps across the band.
        if (fFrameIndexFactor < 0.5f && bAllSampled)
        {
            const FfxFloat32x3 fLuma = FfxFloat32x3(0.299f, 0.587f, 0.114f);
            const FfxFloat32 fLumP = dot(fPrevColorGame.fRaw, fLuma) + 0.02f;
            const FfxFloat32 fLumC = dot(fCurrColorGame.fRaw, fLuma) + 0.02f;
            const FfxFloat32x3 fP = fPrevColorGame.fRaw + FfxFloat32x3(0.02f, 0.02f, 0.02f);
            const FfxFloat32x3 fC = fCurrColorGame.fRaw + FfxFloat32x3(0.02f, 0.02f, 0.02f);
            const FfxFloat32 fBrightnessOnly = ffxSaturate((dot(fP, fC) / (length(fP) * length(fC)) - 0.985f) / 0.01f);
            const FfxFloat32 fChange = abs(log(fLumC / fLumP));
            // how much of this pixel is a brightness step (a blend of the two frames would show it twice)
            // Only where optical flow does not explain the pixel either and the game's vectors are
            // the trusted ones: some objects (a hose lying on a hangar floor) come with wild vectors,
            // and optical flow is what keeps those in place.
            const FfxFloat32 fSweep = fBrightnessOnly * ffxSaturate((fChange - 0.18f) / 0.12f) * (1.0f - ffxSaturate(fDisoccludedFactor)) *
                                      ffxSaturate((fOFError - 0.04f) / 0.06f) * ffxSaturate(fGameMvBias);
            if (fSweep > 0.0f)
            {
                const FfxFloat32x2 fSize = FfxFloat32x2(DisplaySize());
                const FfxFloat32x2 fPrevMv = gameMv.fMotionVector * PreviousFrameScale() * fUvLetterBoxScale;
                const FfxFloat32x2 fCurrMv = -gameMv.fMotionVector * CurrentFrameScale() * fUvLetterBoxScale;
                // this pixel's two states, as area brightness
                const FfxFloat32x2 fHere = SweepLumaArea(fUvInScreenSpace, fPrevMv, fCurrMv, fSize);
                const FfxFloat32 fNewState = log(fLumC / fLumP) > 0.0f ? ffxMax(fHere.x, fHere.y) : ffxMin(fHere.x, fHere.y);
                const FfxFloat32 fOldState = log(fLumC / fLumP) > 0.0f ? ffxMin(fHere.x, fHere.y) : ffxMax(fHere.x, fHere.y);
                FfxFloat32 fDistNew = 1000.0f;
                FfxFloat32 fDistOld = 1000.0f;
                for (FfxInt32 iDirection = 0; iDirection < 8; iDirection++)
                {
                    const FfxFloat32 fAngle = FfxFloat32(iDirection) * 0.7853982f;
                    const FfxFloat32x2 fDirection = FfxFloat32x2(cos(fAngle), sin(fAngle));
                    FfxFloat32 fDistance = 4.0f;
                    FfxFloat32 fStep = 4.0f;
                    for (FfxInt32 iStep = 0; iStep < 12; iStep++)
                    {
                        const FfxFloat32x2 fUv = fUvInScreenSpace + fDirection * fDistance / fSize;
                        const FfxFloat32x2 fThere = SweepLuma(fUv, fPrevMv, fCurrMv, fSize);
                        if (fThere.x < 0.0f)
                        {
                            break;
                        }
                        if (abs(log(fThere.y / fThere.x)) < 0.3f * fChange)
                        {
                            // unchanged surface, a little further in: in this pixel's new state or its old one?
                            const FfxFloat32x2 fArea = SweepLumaArea(fUv + fDirection * 8.0f / fSize, fPrevMv, fCurrMv, fSize);
                            if (fArea.x > 0.0f && fHere.x > 0.0f)
                            {
                                const FfxFloat32 fState = log(0.5f * (fArea.x + fArea.y));
                                if (abs(fState - log(fNewState)) < abs(fState - log(fOldState)))
                                {
                                    fDistNew = ffxMin(fDistNew, fDistance);
                                }
                                else
                                {
                                    fDistOld = ffxMin(fDistOld, fDistance);
                                }
                            }
                            break;
                        }
                        fDistance += fStep;
                        fStep *= 1.35f;
                    }
                }
                if (fDistNew < 999.0f || fDistOld < 999.0f)
                {
                    // one of the two out of reach: it is at least the search range away
                    fDistNew = ffxMin(fDistNew, 320.0f);
                    fDistOld = ffxMin(fDistOld, 320.0f);
                    const FfxFloat32 fCrossing = fDistNew / (fDistNew + fDistOld);
                    const FfxFloat32 fAfter = ffxSaturate((InterpolationTime() - fCrossing) / 0.08f + 0.5f);
                    const FfxFloat32x3 fSwept = ffxLerp(fPrevColorGame.fRaw, fCurrColorGame.fRaw, fAfter);
                    fInterpolatedColor = ffxLerp(fInterpolatedColor, fSwept, fSweep);
                }
            }
        }
#endif
    }
}

void computeFrameinterpolation(FfxInt32x2 iPxPos)
{
    FfxFloat32x3 fColor            = FfxFloat32x3(0, 0, 0);
    FfxFloat32   fInPaintingWeight = 0.0f;

    if (IsInRect(iPxPos, InterpolationRectBase(), InterpolationRectSize()) == false || FrameIndexSinceLastReset() == 0)
    {
        // if we just reset or we are out of the interpolation rect, copy the current back buffer and don't interpolate
        fColor = LoadCurrentBackbuffer(iPxPos);
    }
    else
    {
        computeInterpolatedColor(iPxPos, fColor, fInPaintingWeight);
    }

    StoreFrameinterpolationOutput(FfxInt32x2(iPxPos), FfxFloat32x4(fColor, fInPaintingWeight));
}

#endif  // FFX_FRAMEINTERPOLATION_H
