///////////////////////////////////////////////////////////////////////////
//
// Copyright (c) 2016 DreamWorks Animation LLC. 
//
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
// *       Redistributions of source code must retain the above
//         copyright notice, this list of conditions and the following
//         disclaimer.
// *       Redistributions in binary form must reproduce the above
//         copyright notice, this list of conditions and the following
//         disclaimer in the documentation and/or other materials
//         provided with the distribution.
// *       Neither the name of DreamWorks Animation nor the names of its
//         contributors may be used to endorse or promote products
//         derived from this software without specific prior written
//         permission.
// 
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
///////////////////////////////////////////////////////////////////////////
///
/// @file raySampleAccumulatorExample.cpp


//
//  raySampleAccumulatorExample
//
//
//


#include <OpenEXR/ImfHeader.h>
#include <OpenEXR/ImfDeepImageIO.h>
#include <OpenEXR/ImathMatrix.h>

#include <OpenDCX/DcxDeepImageTile.h>
#include <OpenDCX/DcxDeepTransform.h>  // for radians()
#include <OpenDCX/DcxImageFormat.h>


#define DEBUG_TRACER 1
#ifdef DEBUG_TRACER
# define SAMPLER_X 355
# define SAMPLER_Y 84
# define SAMPLINGXY(A, B) (A==SAMPLER_X && B==SAMPLER_Y)
# define SAMPLINGXYZ(A, B, C) (A==SAMPLER_X && B==SAMPLER_Y && C==SAMPLER_Z)
# define SAMPLING_X(A) (A==SAMPLER_X)
# define SAMPLING_Y(A) (A==SAMPLER_Y)
# define SAMPLING_Z(A) (A==SAMPLER_Z)
#endif

//-------------------------------------------------------------------------
//-------------------------------------------------------------------------


typedef uint64_t SurfaceID;


struct MyRay
{
    Imath::V3f  origin;
    Imath::V3f  dir;
};

struct MyCamera
{
    Imath::M44f matrix;
    float       lens;                       // lens magnification
    float       fX, fY, fR, fT, fW, fH;     // float extents of viewport
    float       fWinAspect;                 // aspect ratio of viewport

    MyCamera(const Imath::V3f& translate,
             const Imath::V3f& rotate_degrees,
             float focalLength,
             float hAperture,
             const Imath::Box2i& format,
             float pixel_aspect) :
        fX(format.min.x),
        fY(format.min.y),
        fR(format.max.x),
        fT(format.max.y),
        fW(format.max.x - format.min.x + 1),
        fH(format.max.y - format.min.y + 1)
    {
        matrix.makeIdentity();
        matrix.rotate(Imath::V3f(radians(rotate_degrees.x),
                                 radians(rotate_degrees.y),
                                 radians(rotate_degrees.z)));
        matrix.translate(translate);
        lens = hAperture / focalLength;
        fWinAspect = (fH / fW)/pixel_aspect; // Image aspect with pixel-aspect mixed in
    }

    inline
    void getNdcCoord(float pixelX, float pixelY,
                     float& u, float& v) const
    {
        u = (pixelX - fX)/fW*2.0f - 1.0f;
        v = (pixelY - fY)/fH*2.0f - 1.0f;
    }

    inline
    void buildRay(float pixelX, float pixelY, MyRay& R) const
    {
        float u, v;
        getNdcCoord(pixelX, pixelY, u, v);
        //
        R.origin = matrix.translation();
        matrix.multDirMatrix(Imath::V3f(u*lens*0.5f, v*lens*0.5f*fWinAspect, -1.0f), R.dir);
        R.dir.normalize();
    }
};

//-------------------------------------------------------------------------
//-------------------------------------------------------------------------

struct MySphere
{
    Imath::V3f  center;
    float       radius;
    Imath::V4f  color;
    SurfaceID   surfID;

    inline
    bool intersect(const MyRay& R,
                   double& tmin,
                   double& tmax,
                   Imath::V3f& P,
                   Imath::V3f& N) const
    {
        const Imath::V3f s_minus_r = R.origin - center;
        const double a = R.dir.length2();
        const double b = 2.0 * R.dir.dot(s_minus_r);
        const double c = s_minus_r.length2() - radius*radius;
        const double discrm = b*b - 4.0*a*c;
        if (discrm >= EPSILONd) {
           const double l = sqrt(discrm);
           tmin = (-b - l) / (2.0 * a);
           tmax = (-b + l) / (2.0 * a);
           if (tmin < EPSILONd && tmax < EPSILONd)
              return false; // behind sphere
           P = R.origin + R.dir*tmin;
           N = P - center;
           N.normalize();
           return true;
        }
        if (fabs(discrm) < EPSILONd) {
           // Ray is tangent to sphere:
           tmin = tmax = -b / (2.0 * a);
           if (tmin < EPSILONd)
              return false; // behind sphere
           P = R.origin + R.dir*tmin;
           N = P - center;
           N.normalize();
           return true;
        }
        return false;
    }

};

//-------------------------------------------------------------------------
//-------------------------------------------------------------------------


enum PrimType
{
    SPHERE      = 0,    // Only support spheres in this example
    //DISC        = 1,
    //TRIANGLE    = 2
};


struct DeepIntersection
{
    double          tmin;           // Distance from ray origin to nearest intersection point
    double          tmax;           // Distance from ray origin to farthest intersection point
    void*           primPtr;        // Pointer to hit primitive
    PrimType        primType;       // Object type to cast primPtr
    Imath::V4f      color;          // Shaded surface color at intersection
    Imath::V3f      N;              // Shaded surface normal
    Dcx::SpMask8    spmask;         // Subpixel mask
    int             count;          // Number of intersections combined with this one
};

//! List of DeepIntersections
typedef std::vector<DeepIntersection> DeepIntersectionList;

//
// A surface can overlap itself causing the same surface ID to show up multiple times
// in the same deep intersection list, but we don't want to always combine them if the
// surface intersections are facing away from each other or are not close in Z.
//

// List of same-surface DeepIntersection indices
typedef std::vector<size_t> DeepSurfaceIntersectionList;
typedef std::map<SurfaceID, DeepSurfaceIntersectionList> DeepIntersectionMap;

//-------------------------------------------------------------------------
//-------------------------------------------------------------------------

void
usageMessage (const char argv0[], bool verbose=false)
{
    std::cerr << "usage: " << argv0 << " [options] infile" << std::endl;

    if (verbose)
    {
        std::cerr << "\n"
                "Print info about a deep pixel or line of deep pixels\n"
                "\n"
                "Options:\n"
                "  -skip <n>       read every nth input pixel when creating spheres\n"
                "  -scale <v>      scale the sphere radius by this\n"
                "  -sp <v>         subpixel sampling rate\n"
                "  -spX <x>        X subpixel sampling rate\n"
                "  -spY <y>        Y subpixel sampling rate\n"
                "  -zthresh <v>    Z distance threshold for combining samples\n"
                "\n"
                "  -h              prints this message\n";

         std::cerr << std::endl;
    }
    exit (1);
}


int
main (int argc, char *argv[])
{

    const char* inFile = 0;
    const char* outFile = 0;

    int   skipPixel = 8;
    float scaleSpheres = 40.0f;
    int   subpixelXRate = 16;
    int   subpixelYRate = 16;
    float deepCombineZThreshold = 1.0f;

    //
    // Parse the command line.
    //

    if (argc < 2)
        usageMessage(argv[0], true);

    {
        int i = 1;
        while (i < argc)
        {
            if (!strcmp(argv[i], "-skip"))
            {
                // Input pixel-skip rate:
                if (i > argc - 2)
                    usageMessage(argv[0]);
                skipPixel = (int)floor(strtol(argv[i + 1], 0, 0));
                i += 2;
            }
            else if (!strcmp(argv[i], "-scale"))
            {
                // Sphere scale factor:
                if (i > argc - 2)
                    usageMessage(argv[0]);
                scaleSpheres = fabs(strtol(argv[i + 1], 0, 0));
                i += 2;
            }
            else if (!strcmp(argv[i], "-sp"))
            {
                // Subpixel rate:
                if (i > argc - 2)
                    usageMessage(argv[0]);
                subpixelXRate = std::max(1, (int)floor(strtol(argv[i + 1], 0, 0)));
                subpixelYRate = subpixelXRate;
                i += 2;
            }
            else if (!strcmp(argv[i], "-spX"))
            {
                // Subpixel X rate:
                if (i > argc - 2)
                    usageMessage(argv[0]);
                subpixelXRate = std::max(1, (int)floor(strtol(argv[i + 1], 0, 0)));
                i += 2;
            }
            else if (!strcmp(argv[i], "-spY"))
            {
                // Subpixel Y rate:
                if (i > argc - 2)
                    usageMessage(argv[0]);
                subpixelYRate = std::max(1, (int)floor(strtol(argv[i + 1], 0, 0)));
                i += 2;
            }
            else if (!strcmp(argv[i], "-zthresh"))
            {
                // Subpixel rate:
                if (i > argc - 2)
                    usageMessage(argv[0]);
                deepCombineZThreshold = fabs(strtol(argv[i + 1], 0, 0));
                i += 2;
            }
            else if (!strcmp(argv[i], "-h"))
            {
                // Print help message:
                usageMessage(argv[0], true);
            }
            else
            {
                // Image file names:
                if (inFile == 0)
                    inFile = argv[i];
                else
                    outFile = argv[i];
                i += 1;
            }
        }
    }

    if (inFile == 0 || outFile == 0)
        usageMessage(argv[0]);

    //
    // Load inFile, spawn spheres for each input deep sample, raytrace each
    // output pixel and save the result in outFile.
    //

    int exitStatus = 0;

    Dcx::ChannelContext channelCtx; // stores shared channel aliases

    try
    {
        Imf::Header inHeader; // for access to displayWindow...
        Imf::DeepImage inDeepImage;
        Imf::loadDeepScanLineImage(std::string(inFile), inHeader, inDeepImage);

        // Dcx::DeepTile stores the ChannelSet along with the channel ptrs:
        Dcx::DeepImageInputTile inDeepTile(inHeader, inDeepImage, channelCtx, true/*Yup*/);

        //std::cout << "reading file '" << inFile << "'" << std::endl;
        //std::cout << " in_bbox" << inDeepTile.dataWindow() << std::endl;
        //inDeepTile.channels().print("channels=", std::cout, channelCtx); std::cout << std::endl;

        // Output tile is copy of in tile.
        // This is for convenience, the output image can be completely
        // different.
        Dcx::DeepImageOutputTile outDeepTile(inDeepTile);
        outDeepTile.setOutputFile(outFile, Imf::INCREASING_Y/*lineOrder*/);

        //--------------------------------------------------------------------------

        MyCamera cam(Imath::V3f(outDeepTile.w()/2.0f + 0.5f,
                                outDeepTile.h()/2.0f + 0.5f,
                                float(outDeepTile.w()))/*translate*/,
                     Imath::V3f(0.0f, 0.0f, 0.0f)/*rotate*/,
                     50.0f/*focalLength*/, 24.0f/*hAperture*/,
                     inDeepTile.displayWindow(), 1.0f/*pixel_aspect*/);

        // Make a bunch of spheres to intersect:
        std::vector<MySphere> pixelSpheres;

        Dcx::ChannelSet shaderChannels(inDeepTile.channels());
        shaderChannels &= Dcx::Mask_RGBA; // only use rgba

        uint64_t ID = 0;
        int maxSamples = 0;
        Dcx::DeepPixel inDeepPixel(shaderChannels);
        for (int inY=inDeepTile.y(); inY <= inDeepTile.t(); inY += skipPixel)
        {
            for (int inX=inDeepTile.x(); inX <= inDeepTile.r(); inX += skipPixel)
            {
                inDeepTile.getDeepPixel(inX, inY, inDeepPixel);
                const size_t nSamples = inDeepPixel.size();
                for (size_t s=0; s < nSamples; ++s)
                {
                    const Dcx::DeepSegment ds = inDeepPixel.getSegment(s);
                    const Dcx::Pixelf dp = inDeepPixel.getSegmentPixel(s);
                    //
                    MySphere sphere;
                    sphere.center.setValue(float(inX), float(inY), -ds.Zf);
                    sphere.radius = std::min(0.1f, ds.Zb - ds.Zf) * scaleSpheres;
                    int c = 0;
                    foreach_channel(z, shaderChannels)
                        sphere.color[c++] = dp[z];
                    sphere.surfID = ID++;

                    pixelSpheres.push_back(sphere);
                }
                maxSamples = std::max(maxSamples, (int)nSamples);
            }
        }
        //std::cout << "spheres=" << pixelSpheres.size() << ", maxSamples=" << maxSamples << std::endl;

        //--------------------------------------------------------------------------
        // Temp list of intersections, reused at each subpixel:
        DeepIntersectionList deepIntersectionList;
        deepIntersectionList.reserve(20);
        // The accumulated list of intersections for the whole pixel:
        DeepIntersectionList deepAccumIntersectionList;
        deepAccumIntersectionList.reserve(maxSamples);
        // Map of unique prim intersections for this pixel:
        DeepIntersectionMap deepIntersectionMap;
        //--------------------------------------------------------------------------

        // Resued at each pixel:
        Dcx::DeepPixel outDeepPixel(shaderChannels);
        outDeepPixel.reserve(10);
        Dcx::DeepSegment outDs;
        Dcx::Pixelf outDp(outDeepPixel.channels());

#if 0//def DEBUG_TRACER
#else
        std::cout << "raytracing " << pixelSpheres.size() << " spheres for " << outDeepTile.h() << " lines:" << std::endl;
#endif

        for (int outY=outDeepTile.y(); outY <= outDeepTile.t(); ++outY)
        {
#if 0//def DEBUG_TRACER
#else
            std::cout << "  line " << outY << std::endl;
#endif
            for (int outX=outDeepTile.x(); outX <= outDeepTile.r(); ++outX)
            {
#if 0//def DEBUG_TRACER
                const bool debug = (SAMPLINGXY(outX, outY));
                if (!debug)
                {
                    outDeepTile.clearDeepPixel(outX, outY);
                    continue;
                }
                std::cout << outX << "," << outY << ":" << std::endl;
#endif

                deepAccumIntersectionList.clear();
                deepIntersectionMap.clear();

                for (int sy=0; sy < subpixelYRate; ++sy)
                {
                    const float sdy = (float(sy)/float(subpixelYRate - 1));
                    for (int sx=0; sx < subpixelXRate; ++sx)
                    {
                        const float sdx = (float(sx)/float(subpixelXRate - 1));

                        // Build output spmask for this subpixel:
                        Dcx::SpMask8 outSpMask = Dcx::SpMask8::allBitsOff;
                        int outSpX, outSpY, outSpR, outSpT;
                        Dcx::SpMask8::mapXCoord(sx, subpixelXRate, outSpX, outSpR);
                        Dcx::SpMask8::mapYCoord(sy, subpixelYRate, outSpY, outSpT);
                        outSpMask.setSubpixels(outSpX, outSpY, outSpR, outSpT);
#if 0//def DEBUG_TRACER
                        outSpMask.printPattern(std::cout, "  ");
#endif

                        // Build a ray with this subpixel offset:
                        MyRay R;
                        cam.buildRay(float(outX)+sdx,
                                     float(outY)+sdy, R);

                        deepIntersectionList.clear();

                        // Naively intersect the ray with all the spheres - obviously
                        // in practice this would use an acceleration structure:
                        const size_t nSpheres = pixelSpheres.size();
#if 0//def DEBUG_TRACER
                        std::cout << "  " << sx << "," << sy << ": ray-tracing against " << nSpheres << " spheres" << std::endl;
#endif
                        for (size_t i=0; i < nSpheres; ++i)
                        {
                            const MySphere& sphere = pixelSpheres[i];
                            double tmin, tmax;
                            Imath::V3f P, N;
                            if (sphere.intersect(R, tmin, tmax, P, N))
                            {
#if 0//def DEBUG_TRACER
                                std::cout << "  " << sx << "," << sy << ": hit! sphere ID#" << sphere.surfID << std::endl;
#endif
                                bool addIt = true;
                                {
                                    //
                                    // Shade step would go here - make spheres shiny, or check
                                    // if alpha < epsilon to produce holes (addIt = false)
                                    //
                                }
                                if (addIt)
                                {
                                    // Add a intersection reference to the sphere:
                                    deepIntersectionList.push_back(DeepIntersection());
                                    DeepIntersection& I = deepIntersectionList[deepIntersectionList.size()-1];
                                    I.tmin     = tmin;
                                    I.tmax     = tmin;
                                    I.primPtr  = (void*)&sphere;
                                    I.primType = PrimType::SPHERE;
                                    I.color    = sphere.color;
                                    I.N        = N;
                                    I.spmask   = outSpMask;
                                    I.count    = 1;
                                }
                            }
                        }

                        const size_t nIntersections = deepIntersectionList.size();
                        if (nIntersections == 0)
                            continue;

                        // Find all the intersections that should be combined:
                        for (size_t i=0; i < nIntersections; ++i)
                        {
                            DeepIntersection& I = deepIntersectionList[i];
                            assert(I.primPtr);

                            // We only understand spheres in this example...:
                            if (I.primType != PrimType::SPHERE)
                                continue;
                            const MySphere* sphere = static_cast<MySphere*>(I.primPtr);

                            // Has the sphere's surface ID been intersected before for
                            // this subpixel?
                            DeepIntersectionMap::iterator it = deepIntersectionMap.find(sphere->surfID);
                            if (it == deepIntersectionMap.end())
                            {
                               // Not in map yet, add intersection to the accumulate list:
                               deepAccumIntersectionList.push_back(I);
                               const size_t accumIndex = deepAccumIntersectionList.size()-1;
                               DeepSurfaceIntersectionList dsl;
                               dsl.reserve(10);
                               dsl.push_back(accumIndex);
                               deepIntersectionMap[sphere->surfID] = dsl;
#if 0//def DEBUG_TRACER
                               if (debug)
                                   std::cout << "      new surface, adding to map" << std::endl;
#endif
                                continue;
                            }

                            //------------------------------------------------------------------------
                            // Intersection already in map, so let's see if it's close enough in Z
                            // and N to combine with one of the previous intersections.
                            //
                            // (TODO: we only check the first match, it's better to find all potential
                            //  matches and select the closest match)
                            //
                            //------------------------------------------------------------------------
                            DeepSurfaceIntersectionList& dsl = it->second;
                            const size_t nCurrentSurfaces = dsl.size();
                            bool match = false;
                            for (size_t j=0; j < nCurrentSurfaces; ++j)
                            {
                                DeepIntersection& matchedI = deepAccumIntersectionList[dsl[j]];
                                //---------------------------------------------------------------------
                                // Compare criteria:
                                //      * minZ within threshold range
                                //      * maxZ within threshold range
                                //      * normal < 180deg diff
                                // (TODO: compare other params like color contrast to retain high-freq
                                //  detail!)
                                //---------------------------------------------------------------------
                                const float eMinZ = matchedI.tmin - deepCombineZThreshold;
                                const float eMaxZ = matchedI.tmax + deepCombineZThreshold;
                                const float eN = I.N.dot(matchedI.N);
#if 0//def DEBUG_TRACER
                                if (debug) {
                                    std::cout << "    minZ=" << matchedI.tmin << ", maxZ=" << matchedI.tmax << std::endl;
                                    std::cout << "    eMinZ=" << eMinZ << ", eMaxZ=" << eMaxZ << ", eN=" << eN << std::endl;
                                }
#endif
                                if (I.tmin < eMinZ || I.tmin > eMaxZ || eN < 0.5f)
                                    continue; // no match, skip to next

                                // Matched! Combine intersections:
                                matchedI.tmin = std::min(matchedI.tmin, I.tmin);
                                matchedI.tmax = std::max(matchedI.tmax, I.tmin);

                                matchedI.color  += I.color; // Add colors together
                                matchedI.spmask |= I.spmask; // Or the subpixel masks together
                                matchedI.count  += 1; // Increase combined count
#if 0//def DEBUG_TRACER
                                if (debug) {
                                    std::cout << "      combine with prev, minZ=" << matchedI.tmin << " maxZ=" << matchedI.tmax << std::endl;
                                    matchedI.spmask.printPattern(std::cout, "      ");
                                }
#endif
                                match = true;
                                break;
                            }
                            if (!match)
                            {
                                //--------------------------------------------------------------
                                // No match in current surface list, add intersection as unique.
                                // Copy unique intersection to accumlate list:
                                //--------------------------------------------------------------
                                deepAccumIntersectionList.push_back(I);
                                const size_t accumIndex = deepAccumIntersectionList.size()-1;
                                dsl.push_back(accumIndex);
#if 0//def DEBUG_TRACER
                                if (debug)
                                    std::cout << "      surfaceID match, but intersections too different, add to surface's list" << std::endl;
#endif
                            }
#if 0//def DEBUG_TRACER
                            if (debug) {
                               std::cout << "      nDeepIntersections=" << nIntersections;
                               std::cout << ", nCurrentSurfaces=" << nCurrentSurfaces;
                               std::cout << ", dsl.size()=" << dsl.size();
                               std::cout << std::endl;
                            }
#endif

                        } // nIntersections loop

                    } // subpixel-x loop

                } // subpixel-y loop

                const size_t nIntersections = deepAccumIntersectionList.size();
                if (nIntersections == 0)
                {
                    outDeepTile.clearDeepPixel(outX, outY);
                    continue;
                }

                outDeepPixel.clear();
                outDeepPixel.reserve(nIntersections);

                for (size_t i=0; i < nIntersections; ++i)
                {
                    // Build an output DeepSegment for each combined intersection:
                    const DeepIntersection& I = deepAccumIntersectionList[i];
                    outDs.Zf = float(I.tmin);
                    outDs.Zb = float(I.tmax);
                    outDs.index = -1; // gets assigned when appended to DeepPixel
                    outDs.metadata.spmask = I.spmask;
                    outDs.metadata.flags = Dcx::DEEP_LINEAR_INTERP_SAMPLE; // always hard surfaces for this example
                    const size_t dsIndex = outDeepPixel.append(outDs); // add DeepSegment and get index to it
                    // Copy color to DeepSegment's pixel:
                    Dcx::Pixelf& dp = outDeepPixel.getSegmentPixel(dsIndex);
                    int c = 0;
                    foreach_channel(z, Dcx::Mask_RGBA)
                    {
                        dp[z] = I.color[c++] / float(I.count);
                    }
                }

#if 0//def DEBUG_TRACER
                if (debug)
                    outDeepPixel.printInfo(std::cout, "outDeepPixel=");
#endif
                outDeepTile.setDeepPixel(outX, outY, outDeepPixel);


            } // outX loop

            // Write deep scanline so we can free tile line memory:
            outDeepTile.writeScanline(outY, true/*flush-line*/);

        } // outY loop

    }
    catch (const std::exception &e)
    {
        std::cerr << e.what() << std::endl;
        exitStatus = 1;
    }

    return exitStatus;
}