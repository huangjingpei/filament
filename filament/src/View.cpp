/*
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "details/View.h"

#include "Culler.h"
#include "DFG.h"
#include "Froxelizer.h"
#include "RenderPrimitive.h"
#include "ResourceAllocator.h"

#include "details/Engine.h"
#include "details/IndirectLight.h"
#include "details/RenderTarget.h"
#include "details/Renderer.h"
#include "details/Scene.h"
#include "details/Skybox.h"

#include <filament/Exposure.h>
#include <filament/TextureSampler.h>
#include <filament/View.h>

#include <private/filament/SibGenerator.h>
#include <private/filament/UibStructs.h>

#include <utils/Profiler.h>
#include <utils/Slice.h>
#include <utils/Systrace.h>
#include <utils/debug.h>
#include <utils/Zip2Iterator.h>

#include <math/scalar.h>
#include <math/fast.h>

#include <memory>

using namespace utils;

namespace filament {

using namespace backend;
using namespace math;

static constexpr float PID_CONTROLLER_Ki = 0.002f;
static constexpr float PID_CONTROLLER_Kd = 0.0f;

FView::FView(FEngine& engine)
    : mFroxelizer(engine),
      mPerViewUniforms(engine),
      mShadowMapManager(engine) {
    DriverApi& driver = engine.getDriverApi();

    FDebugRegistry& debugRegistry = engine.getDebugRegistry();

    debugRegistry.registerProperty("d.view.camera_at_origin",
            &engine.debug.view.camera_at_origin);

    // Integral term is used to fight back the dead-band below, we limit how much it can act.
    mPidController.setIntegralLimits(-100.0f, 100.0f);

    // dead-band, 1% for scaling down, 5% for scaling up. This stabilizes all the jitters.
    mPidController.setOutputDeadBand(-0.01f, 0.05f);

#ifndef NDEBUG
    debugRegistry.registerDataSource("d.view.frame_info",
            mDebugFrameHistory.data(), mDebugFrameHistory.size());
    debugRegistry.registerProperty("d.view.pid.kp", &engine.debug.view.pid.kp);
    debugRegistry.registerProperty("d.view.pid.ki", &engine.debug.view.pid.ki);
    debugRegistry.registerProperty("d.view.pid.kd", &engine.debug.view.pid.kd);
    // default parameters for debugging UI
    engine.debug.view.pid.kp = 1.0f - std::exp(-1.0f / 8.0f);
    engine.debug.view.pid.ki = PID_CONTROLLER_Ki;
    engine.debug.view.pid.kd = PID_CONTROLLER_Kd;
    mPidController.setParallelGains(
            engine.debug.view.pid.kp, engine.debug.view.pid.ki, engine.debug.view.pid.kd);
#endif

    // allocate ubos
    mLightUbh = driver.createBufferObject(CONFIG_MAX_LIGHT_COUNT * sizeof(LightsUib),
            BufferObjectBinding::UNIFORM, BufferUsage::DYNAMIC);

    mShadowUbh = driver.createBufferObject(mShadowUb.getSize(),
            BufferObjectBinding::UNIFORM, BufferUsage::DYNAMIC);

    mIsDynamicResolutionSupported = driver.isFrameTimeSupported();

    mDefaultColorGrading = mColorGrading = engine.getDefaultColorGrading();
}

FView::~FView() noexcept = default;

void FView::terminate(FEngine& engine) {
    // Here we would cleanly free resources we've allocated or we own (currently none).

    while (mActivePickingQueriesList) {
        FPickingQuery* const pQuery = mActivePickingQueriesList;
        mActivePickingQueriesList = pQuery->next;
        pQuery->callback(pQuery->result, pQuery);
        FPickingQuery::put(pQuery);
    }

    DriverApi& driver = engine.getDriverApi();
    driver.destroyBufferObject(mLightUbh);
    driver.destroyBufferObject(mShadowUbh);
    driver.destroyBufferObject(mRenderableUbh);
    drainFrameHistory(engine);
    mPerViewUniforms.terminate(driver);
    mFroxelizer.terminate(driver);
}

void FView::setViewport(filament::Viewport const& viewport) noexcept {
    // catch the cases were user had an underflow and didn't catch it.
    assert((int32_t)viewport.width > 0);
    assert((int32_t)viewport.height > 0);
    mViewport = viewport;
}

void FView::setDynamicResolutionOptions(DynamicResolutionOptions const& options) noexcept {
    DynamicResolutionOptions& dynamicResolution = mDynamicResolution;
    dynamicResolution = options;

    // only enable if dynamic resolution is supported
    dynamicResolution.enabled = dynamicResolution.enabled && mIsDynamicResolutionSupported;
    if (dynamicResolution.enabled) {
        // if enabled, sanitize the parameters

        // minScale cannot be 0 or negative
        dynamicResolution.minScale = max(dynamicResolution.minScale, float2(1.0f / 1024.0f));

        // maxScale cannot be < minScale
        dynamicResolution.maxScale = max(dynamicResolution.maxScale, dynamicResolution.minScale);

        // clamp maxScale to 2x because we're doing bilinear filtering, so super-sampling
        // is not useful above that.
        dynamicResolution.maxScale = min(dynamicResolution.maxScale, float2(2.0f));

        dynamicResolution.sharpness = clamp(dynamicResolution.sharpness, 0.0f, 2.0f);
    }
}

void FView::setDynamicLightingOptions(float zLightNear, float zLightFar) noexcept {
    mFroxelizer.setOptions(zLightNear, zLightFar);
}

float2 FView::updateScale(FEngine& engine,
        FrameInfo const& info,
        Renderer::FrameRateOptions const& frameRateOptions,
        Renderer::DisplayInfo const& displayInfo) noexcept {

    // scale factor returned to the caller is modified so the scaled viewport is rounded to
    // 8 pixels. The internal scale factor, mScale, doesn't have this rounding.
    float2 roundedScale = mScale;

    DynamicResolutionOptions const& options = mDynamicResolution;
    if (options.enabled) {
        if (!UTILS_UNLIKELY(info.valid)) {
            // always clamp to the min/max scale range
            mScale = clamp(1.0f, options.minScale, options.maxScale);
            return mScale;
        }

#ifndef NDEBUG
        const float Kp = engine.debug.view.pid.kp;
        const float Ki = engine.debug.view.pid.ki;
        const float Kd = engine.debug.view.pid.kd;
#else
        const float Kp = (1.0f - std::exp(-frameRateOptions.scaleRate));
        const float Ki = PID_CONTROLLER_Ki;
        const float Kd = PID_CONTROLLER_Kd;
#endif
        mPidController.setParallelGains(Kp, Ki, Kd);

        // all values in ms below
        using std::chrono::duration;
        const float dt = 1.0f; // we don't really need dt here, setting it to 1, means our parameters are in "frames"
        const float target = (1000.0f * float(frameRateOptions.interval)) / displayInfo.refreshRate;
        const float targetWithHeadroom = target * (1.0f - frameRateOptions.headRoomRatio);
        float measured = duration<float, std::milli>{ info.denoisedFrameTime }.count();
        float out = mPidController.update(measured / targetWithHeadroom, 1.0f, dt);

        // maps pid command to a scale (absolute or relative, see below)
         const float command = out < 0.0f ? (1.0f / (1.0f - out)) : (1.0f + out);

        /*
         * There is two ways we can control the scale factor, either by having the PID controller
         * output a new scale factor directly (like a "position" control), or having it evaluate
         * a relative scale factor (like a "velocity" control).
         * More experimentation is needed to figure out which works better in more cases.
         */

        // direct scaling ("position" control)
        //const float scale = command;
        // relative scaling ("velocity" control)
        const float scale = mScale.x * mScale.y * command;

        const float w = float(mViewport.width);
        const float h = float(mViewport.height);
        if (scale < 1.0f && !options.homogeneousScaling) {
            // figure out the major and minor axis
            const float major = std::max(w, h);
            const float minor = std::min(w, h);

            // the major axis is scaled down first, down to the minor axis
            const float maxMajorScale = minor / major;
            const float majorScale = std::max(scale, maxMajorScale);

            // then the minor axis is scaled down to the original aspect-ratio
            const float minorScale = std::max(scale / majorScale, majorScale * maxMajorScale);

            // if we have some scaling capacity left, scale homogeneously
            const float homogeneousScale = scale / (majorScale * minorScale);

            // finally, write the scale factors
            float& majorRef = w > h ? mScale.x : mScale.y;
            float& minorRef = w > h ? mScale.y : mScale.x;
            majorRef = std::sqrt(homogeneousScale) * majorScale;
            minorRef = std::sqrt(homogeneousScale) * minorScale;
        } else {
            // when scaling up, we're always using homogeneous scaling.
            mScale = std::sqrt(scale);
        }

        // always clamp to the min/max scale range
        const auto s = mScale;
        mScale = clamp(s, options.minScale, options.maxScale);

        // disable the integration term when we're outside the controllable range
        // (i.e. we clamped). This help not to have to wait too long for the Integral term
        // to kick in after a clamping event.
        mPidController.setIntegralInhibitionEnabled(mScale != s);

        // now tweak the scaling factor to get multiples of 8 (to help quad-shading)
        // i.e. 8x8=64 fragments, to try to help with warp sizes.
        roundedScale.x = mScale.x == 1.0f ? 1.0f : (std::floor(mScale.x * float{ w } / 8) * 8) / float{ w };
        roundedScale.y = mScale.y == 1.0f ? 1.0f : (std::floor(mScale.y * float{ h } / 8) * 8) / float{ h };
    } else {
        mScale = 1.0f;
        roundedScale = 1.0f;
    }

#ifndef NDEBUG
    // only for debugging...
    using duration_ms = std::chrono::duration<float, std::milli>;
    const float target = (1000.0f * float(frameRateOptions.interval)) / displayInfo.refreshRate;
    const float targetWithHeadroom = target * (1.0f - frameRateOptions.headRoomRatio);
    std::move(mDebugFrameHistory.begin() + 1,
            mDebugFrameHistory.end(), mDebugFrameHistory.begin());
    mDebugFrameHistory.back() = {
            .target             = target,
            .targetWithHeadroom = targetWithHeadroom,
            .frameTime          = std::chrono::duration_cast<duration_ms>(info.frameTime).count(),
            .frameTimeDenoised  = std::chrono::duration_cast<duration_ms>(info.denoisedFrameTime).count(),
            .scale              = mScale.x * mScale.y,
            .pid_e              = mPidController.getError(),
            .pid_i              = mPidController.getIntegral(),
            .pid_d              = mPidController.getDerivative()
    };
#endif

    return roundedScale;
}

void FView::setVisibleLayers(uint8_t select, uint8_t values) noexcept {
    mVisibleLayers = (mVisibleLayers & ~select) | (values & select);
}

bool FView::isSkyboxVisible() const noexcept {
    FSkybox const* skybox = mScene ? mScene->getSkybox() : nullptr;
    return skybox != nullptr && (skybox->getLayerMask() & mVisibleLayers);
}

void FView::prepareShadowing(FEngine& engine, DriverApi& driver,
        FScene::RenderableSoa& renderableData, FScene::LightSoa& lightData) noexcept {
    SYSTRACE_CALL();

    mHasShadowing = false;
    mNeedsShadowMap = false;
    if (!mShadowingEnabled) {
        return;
    }

    mShadowMapManager.reset();

    auto& lcm = engine.getLightManager();

    // dominant directional light is always as index 0
    FLightManager::Instance directionalLight = lightData.elementAt<FScene::LIGHT_INSTANCE>(0);
    const bool hasDirectionalShadows = directionalLight && lcm.isShadowCaster(directionalLight);
    if (UTILS_UNLIKELY(hasDirectionalShadows)) {
        const auto& shadowOptions = lcm.getShadowOptions(directionalLight);
        assert_invariant(shadowOptions.shadowCascades >= 1 &&
                shadowOptions.shadowCascades <= CONFIG_MAX_SHADOW_CASCADES);
        mShadowMapManager.setShadowCascades(0, &shadowOptions);
    }

    // Find all shadow-casting spotlights.
    size_t shadowCastingSpotCount = 0;

    // We allow a max of CONFIG_MAX_SHADOW_CASTING_SPOTS spot light shadows. Any additional
    // shadow-casting spotlights are ignored.
    for (size_t l = FScene::DIRECTIONAL_LIGHTS_COUNT; l < lightData.size(); l++) {

        // when we get here all the lights should be visible
        assert_invariant(lightData.elementAt<FScene::VISIBILITY>(l));

        FLightManager::Instance li = lightData.elementAt<FScene::LIGHT_INSTANCE>(l);

        if (UTILS_LIKELY(!li)) {
            continue; // invalid instance
        }

        if (UTILS_LIKELY(!lcm.isShadowCaster(li))) {
            continue; // doesn't cast shadows
        }

        if (UTILS_LIKELY(!lcm.isSpotLight(li))) {
            continue; // is not a spot-li (we're not supporting point-lights yet)
        }

        const auto& shadowOptions = lcm.getShadowOptions(li);
        mShadowMapManager.addSpotShadowMap(l, &shadowOptions);
        ++shadowCastingSpotCount;
        if (shadowCastingSpotCount > CONFIG_MAX_SHADOW_CASTING_SPOTS - 1) {
            break; // we ran out of spotlight shadow casting
        }
    }

    auto shadowTechnique = mShadowMapManager.update(engine, *this,
            mShadowUb, renderableData, lightData);

    mHasShadowing = any(shadowTechnique);
    mNeedsShadowMap = any(shadowTechnique & ShadowMapManager::ShadowTechnique::SHADOW_MAP);
}

void FView::prepareLighting(FEngine& engine, FEngine::DriverApi& driver, ArenaScope& arena,
        filament::Viewport const& viewport) noexcept {
    SYSTRACE_CALL();

    const CameraInfo& camera = mViewingCameraInfo;
    FScene* const scene = mScene;
    auto const& lightData = scene->getLightData();

    /*
     * Dynamic lights
     */

    mHasDynamicLighting = scene->getLightData().size() > FScene::DIRECTIONAL_LIGHTS_COUNT;
    if (mHasDynamicLighting) {
        scene->prepareDynamicLights(camera, arena, mLightUbh);
        Froxelizer& froxelizer = mFroxelizer;
        if (froxelizer.prepare(driver, arena, viewport, camera.projection, camera.zn, camera.zf)) {
            // update our uniform buffer if needed
            mPerViewUniforms.prepareDynamicLights(mFroxelizer);
        }
    }

    // here the array of visible lights has been shrunk to CONFIG_MAX_LIGHT_COUNT
    SYSTRACE_VALUE32("visibleLights", lightData.size() - FScene::DIRECTIONAL_LIGHTS_COUNT);

    /*
     * Exposure
     */

    const float exposure = Exposure::exposure(camera.ev100);

    mPerViewUniforms.prepareExposure(camera.ev100);

    /*
     * Indirect light (IBL)
     */

    // If the scene does not have an IBL, use the black 1x1 IBL and honor the fallback intensity
    // associated with the skybox.
    float intensity;
    FIndirectLight const* ibl = scene->getIndirectLight();
    if (UTILS_LIKELY(ibl)) {
        intensity = ibl->getIntensity();
    } else {
        ibl = engine.getDefaultIndirectLight();
        FSkybox const* const skybox = scene->getSkybox();
        intensity = skybox ? skybox->getIntensity() : FIndirectLight::DEFAULT_INTENSITY;
    }

    mPerViewUniforms.prepareAmbientLight(*ibl, intensity, exposure);

    /*
     * Directional light (always at index 0)
     */

    FLightManager::Instance directionalLight = lightData.elementAt<FScene::LIGHT_INSTANCE>(0);
    const float3 sceneSpaceDirection = lightData.elementAt<FScene::DIRECTION>(0); // guaranteed normalized
    mPerViewUniforms.prepareDirectionalLight(exposure, sceneSpaceDirection, directionalLight);
    mHasDirectionalLight = directionalLight.isValid();
}

void FView::prepare(FEngine& engine, DriverApi& driver, ArenaScope& arena,
        filament::Viewport const& viewport, float4 const& userTime) noexcept {
    JobSystem& js = engine.getJobSystem();

    /*
     * Prepare the scene -- this is where we gather all the objects added to the scene,
     * and in particular their world-space AABB.
     */

    FScene* const scene = getScene();

    /*
     * We apply a "world origin" to "everything" in order to implement the IBL rotation.
     * The "world origin" could also be useful for other things, like keeping the origin
     * close to the camera position to improve fp precision in the shader for large scenes.
     */
    mat4 worldOriginScene;
    FIndirectLight const* const ibl = scene->getIndirectLight();
    if (ibl) {
        // the IBL transformation must be a rigid transform
        mat3f rotation{ scene->getIndirectLight()->getRotation() };
        // for a rigid-body transform, the inverse is the transpose
        worldOriginScene = mat4{ transpose(rotation) };
    }

    /*
     * Calculate all camera parameters needed to render this View for this frame.
     */
    FCamera const* const camera = mViewingCamera ? mViewingCamera : mCullingCamera;

    if (engine.debug.view.camera_at_origin) {
        // this moves the camera to the origin, effectively doing all shader computations in
        // view-space, which improves floating point precision in the shader by staying around
        // zero, where fp precision is highest. This also ensures that when the camera is placed
        // very far from the origin, objects are still rendered and lit properly.
        worldOriginScene[3].xyz -= camera->getPosition();
    }

    // Note: for debugging (i.e. visualize what the camera / objects are doing, using
    // the viewing camera), we can set worldOriginScene to identity when mViewingCamera
    // is set
    mViewingCameraInfo = CameraInfo(*camera, worldOriginScene);

    mCullingFrustum = Frustum(mat4f{
                    mCullingCamera->getCullingProjectionMatrix() *
                    inverse(worldOriginScene * mCullingCamera->getModelMatrix()) });

    /*
     * Gather all information needed to render this scene. Apply the world origin to all
     * objects in the scene.
     */
    scene->prepare(worldOriginScene, hasVSM());

    /*
     * Light culling: runs in parallel with Renderable culling (below)
     */

    JobSystem::Job* prepareVisibleLightsJob = nullptr;
    if (scene->getLightData().size() > FScene::DIRECTIONAL_LIGHTS_COUNT) {
        prepareVisibleLightsJob = js.runAndRetain(js.createJob(nullptr,
                [this, &engine, &arena, scene](JobSystem&, JobSystem::Job*) {
                    FView::prepareVisibleLights(engine.getLightManager(), arena,
                            mViewingCameraInfo, mCullingFrustum, scene->getLightData());
                }));
    }

    Range merged;
    FScene::RenderableSoa& renderableData = scene->getRenderableData();

    { // all the operations in this scope must happen sequentially

        Slice<Culler::result_type> cullingMask = renderableData.slice<FScene::VISIBLE_MASK>();
        std::uninitialized_fill(cullingMask.begin(), cullingMask.end(), 0);

        /*
         * Culling: as soon as possible we perform our camera-culling
         * (this will set the VISIBLE_RENDERABLE bit)
         */

        prepareVisibleRenderables(js, mCullingFrustum, renderableData);


        /*
         * Shadowing: compute the shadow camera and cull shadow casters
         * (this will set the VISIBLE_DIR_SHADOW_CASTER bit and VISIBLE_SPOT_SHADOW_CASTER bits)
         */

        // prepareShadowing relies on prepareVisibleLights().
        if (prepareVisibleLightsJob) {
            js.waitAndRelease(prepareVisibleLightsJob);
        }
        prepareShadowing(engine, driver, renderableData, scene->getLightData());

        /*
         * Partition the SoA so that renderables are partitioned w.r.t their visibility into the
         * following groups:
         *
         * 1. renderables
         * 2. renderables and directional shadow casters
         * 3. directional shadow casters only
         * 4. punctual light shadow casters only
         * 5. invisible renderables
         *
         * Note that the first three groups are partitioned based only on the lowest two bits of the
         * VISIBLE_MASK (VISIBLE_RENDERABLE and VISIBLE_DIR_SHADOW_CASTER), and thus can also
         * contain punctual light shadow casters as well. The fourth group contains *only* punctual
         * shadow casters.
         *
         * This operation is somewhat heavy as it sorts the whole SoA. We use std::partition instead
         * of sort(), which gives us O(4.N) instead of O(N.log(N)) application of swap().
         */

        // calculate the sorting key for all elements, based on their visibility
        uint8_t const* layers = renderableData.data<FScene::LAYERS>();
        auto const* visibility = renderableData.data<FScene::VISIBILITY_STATE>();
        computeVisibilityMasks(getVisibleLayers(), layers, visibility, cullingMask.begin(),
                renderableData.size());

        auto const beginRenderables = renderableData.begin();
        auto beginCasters = partition(beginRenderables, renderableData.end(), VISIBLE_RENDERABLE);
        auto beginCastersOnly = partition(beginCasters, renderableData.end(),
                VISIBLE_RENDERABLE | VISIBLE_DIR_SHADOW_RENDERABLE);
        auto beginSpotLightCastersOnly = partition(beginCastersOnly, renderableData.end(),
                VISIBLE_DIR_SHADOW_RENDERABLE);
        auto endSpotLightCastersOnly = std::partition(beginSpotLightCastersOnly,
                renderableData.end(), [](auto it) {
                    return (it.template get<FScene::VISIBLE_MASK>() & VISIBLE_SPOT_SHADOW_RENDERABLE);
                });

        // convert to indices
        uint32_t iEnd = uint32_t(beginSpotLightCastersOnly - beginRenderables);
        uint32_t iSpotLightCastersEnd = uint32_t(endSpotLightCastersOnly - beginRenderables);
        mVisibleRenderables = Range{ 0, uint32_t(beginCastersOnly - beginRenderables) };
        mVisibleDirectionalShadowCasters = Range{ uint32_t(beginCasters - beginRenderables), iEnd };
        mSpotLightShadowCasters = Range{ 0, iSpotLightCastersEnd };
        merged = Range{ 0, iSpotLightCastersEnd };

        // update those UBOs
        const size_t size = merged.size() * sizeof(PerRenderableUib);
        if (size) {
            if (mRenderableUBOSize < size) {
                // allocate 1/3 extra, with a minimum of 16 objects
                const size_t count = std::max(size_t(16u), (4u * merged.size() + 2u) / 3u);
                mRenderableUBOSize = uint32_t(count * sizeof(PerRenderableUib));
                driver.destroyBufferObject(mRenderableUbh);
                mRenderableUbh = driver.createBufferObject(mRenderableUBOSize,
                        BufferObjectBinding::UNIFORM, BufferUsage::STREAM);
            } else {
                // TODO: should we shrink the underlying UBO at some point?
            }
            assert_invariant(mRenderableUbh);
            scene->updateUBOs(merged, mRenderableUbh);
        }
    }

    /*
     * Prepare lighting -- this is where we update the lights UBOs, set-up the IBL,
     * set-up the froxelization parameters.
     * Relies on FScene::prepare() and prepareVisibleLights()
     */

    prepareLighting(engine, driver, arena, viewport);

    /*
     * Update driver state
     */

    mPerViewUniforms.prepareTime(userTime);
    mPerViewUniforms.prepareFog(mViewingCameraInfo, mFogOptions);
    mPerViewUniforms.prepareTemporalNoise(mTemporalAntiAliasingOptions);

    // set uniforms and samplers
    bindPerViewUniformsAndSamplers(driver);
}

void FView::computeVisibilityMasks(
        uint8_t visibleLayers,
        uint8_t const* UTILS_RESTRICT layers,
        FRenderableManager::Visibility const* UTILS_RESTRICT visibility,
        Culler::result_type* UTILS_RESTRICT visibleMask, size_t count) {
    // __restrict__ seems to only be taken into account as function parameters. This is very
    // important here, otherwise, this loop doesn't get vectorized.
    // This is vectorized 16x.
    count = (count + 0xFu) & ~0xFu; // capacity guaranteed to be multiple of 16
    for (size_t i = 0; i < count; ++i) {
        Culler::result_type mask = visibleMask[i];
        FRenderableManager::Visibility v = visibility[i];
        bool inVisibleLayer = layers[i] & visibleLayers;

        // The logic below essentially does the following:
        //
        // if inVisibleLayer:
        //     if !v.culling:
        //         set all bits in visibleMask to 1
        // else:
        //     set all bits in visibleMask to 0
        // if !v.castShadows:
        //     if !vsm or !v.receivesShadows:       // with vsm, we also render shadow receivers
        //         set shadow visibility bits in visibleMask to 0
        //
        // It is written without if statements to avoid branches, which allows it to be vectorized 16x.

        const bool visRenderables = (!v.culling || (mask & VISIBLE_RENDERABLE)) && inVisibleLayer;
        const bool visShadowParticipant = v.castShadows;
        const bool visShadowRenderable = (!v.culling || (mask & VISIBLE_DIR_SHADOW_RENDERABLE))
                && inVisibleLayer && visShadowParticipant;
        visibleMask[i] = Culler::result_type(visRenderables) |
                Culler::result_type(visShadowRenderable << 1u);
        // this loop gets fully unrolled
        for (size_t j = 0; j < CONFIG_MAX_SHADOW_CASTING_SPOTS; ++j) {
            const bool visSpotShadowRenderable =
                    (!v.culling || (mask & VISIBLE_SPOT_SHADOW_RENDERABLE_N(j))) &&
                        inVisibleLayer && visShadowParticipant;
            visibleMask[i] |=
                Culler::result_type(visSpotShadowRenderable << VISIBLE_SPOT_SHADOW_RENDERABLE_N_BIT(j));
        }
    }
}

UTILS_NOINLINE
/* static */ FScene::RenderableSoa::iterator FView::partition(
        FScene::RenderableSoa::iterator begin,
        FScene::RenderableSoa::iterator end,
        uint8_t mask) noexcept {
    return std::partition(begin, end, [mask](auto it) {
        // Mask VISIBLE_MASK to ignore higher bits related to spot shadows. We only partition based
        // on renderable and directional shadow visibility.
        return (it.template get<FScene::VISIBLE_MASK>() &
                (VISIBLE_RENDERABLE | VISIBLE_DIR_SHADOW_RENDERABLE)) == mask;
    });
}

void FView::prepareUpscaler(float2 scale) const noexcept {
    SYSTRACE_CALL();
    mPerViewUniforms.prepareUpscaler(scale, mDynamicResolution);
}

void FView::prepareCamera(const CameraInfo& camera) const noexcept {
    SYSTRACE_CALL();
    mPerViewUniforms.prepareCamera(camera);
}

void FView::prepareViewport(const filament::Viewport& viewport) const noexcept {
    SYSTRACE_CALL();
    mPerViewUniforms.prepareViewport(viewport);
}

void FView::prepareSSAO(Handle<HwTexture> ssao) const noexcept {
    mPerViewUniforms.prepareSSAO(ssao, mAmbientOcclusionOptions);
}

void FView::prepareSSR(Handle <HwTexture> ssr, float refractionLodOffset,
        mat4f const& historyProjection, mat4f const& uvFromViewMatrix,
        ScreenSpaceReflectionsOptions const& ssrOptions) const noexcept {
    mPerViewUniforms.prepareSSR(ssr, refractionLodOffset,
            historyProjection, uvFromViewMatrix, ssrOptions);
}

void FView::prepareStructure(Handle<HwTexture> structure) const noexcept {
    // sampler must be NEAREST
    mPerViewUniforms.prepareStructure(structure);
}

void FView::prepareShadow(Handle<HwTexture> texture) const noexcept {
    switch (mShadowType) {
        case filament::ShadowType::PCF:
            mPerViewUniforms.prepareShadowPCF(texture);
            break;
        case filament::ShadowType::VSM:
            mPerViewUniforms.prepareShadowVSM(texture, mVsmShadowOptions);
            break;
        case filament::ShadowType::DPCF:
            mPerViewUniforms.prepareShadowDPCF(texture, mSoftShadowOptions);
            break;
        case filament::ShadowType::PCSS:
            mPerViewUniforms.prepareShadowPCSS(texture, mSoftShadowOptions);
            break;
    }
}

void FView::prepareShadowMap() const noexcept {
    mPerViewUniforms.prepareShadowMapping(
            mShadowMapManager.getShadowMappingUniforms(), mVsmShadowOptions);
}

void FView::cleanupRenderPasses() const noexcept {
    mPerViewUniforms.unbindSamplers();
}

void FView::froxelize(FEngine& engine) const noexcept {
    SYSTRACE_CALL();
    assert_invariant(mHasDynamicLighting);
    mFroxelizer.froxelizeLights(engine, mViewingCameraInfo, mScene->getLightData());
}

void FView::commitUniforms(DriverApi& driver) const noexcept {
    mPerViewUniforms.commit(driver);
    if (mShadowUb.isDirty()) {
        driver.updateBufferObject(mShadowUbh, mShadowUb.toBufferDescriptor(driver), 0);
    }
}

void FView::commitFroxels(DriverApi& driverApi) const noexcept {
    if (mHasDynamicLighting) {
        mFroxelizer.commit(driverApi);
    }
}

UTILS_NOINLINE
void FView::prepareVisibleRenderables(JobSystem& js,
        Frustum const& frustum, FScene::RenderableSoa& renderableData) const noexcept {
    SYSTRACE_CALL();
    if (UTILS_LIKELY(isFrustumCullingEnabled())) {
        FView::cullRenderables(js, renderableData, frustum, VISIBLE_RENDERABLE_BIT);
    } else {
        std::uninitialized_fill(renderableData.begin<FScene::VISIBLE_MASK>(),
                  renderableData.end<FScene::VISIBLE_MASK>(), VISIBLE_RENDERABLE);
    }
}

void FView::cullRenderables(JobSystem& js,
        FScene::RenderableSoa& renderableData, Frustum const& frustum, size_t bit) noexcept {
    SYSTRACE_CALL();

    float3 const* worldAABBCenter = renderableData.data<FScene::WORLD_AABB_CENTER>();
    float3 const* worldAABBExtent = renderableData.data<FScene::WORLD_AABB_EXTENT>();
    FScene::VisibleMaskType* visibleArray = renderableData.data<FScene::VISIBLE_MASK>();

    // culling job (this runs on multiple threads)
    auto functor = [&frustum, worldAABBCenter, worldAABBExtent, visibleArray, bit]
            (uint32_t index, uint32_t c) {
        Culler::intersects(
                visibleArray + index,
                frustum,
                worldAABBCenter + index,
                worldAABBExtent + index, c, bit);
    };

    // Note: we can't use jobs::parallel_for() here because Culler::intersects() must process
    //       multiples of eight primitives.
    // Moreover, even with a large number of primitives, the overhead of the JobSystem is too
    // large compared to the run time of Culler::intersects, e.g.: ~100us for 4000 primitives
    // on Pixel4.
    functor(0, renderableData.size());
}

void FView::prepareVisibleLights(FLightManager const& lcm, ArenaScope& rootArena,
        const CameraInfo& camera, Frustum const& frustum, FScene::LightSoa& lightData) noexcept {
    SYSTRACE_CALL();
    assert_invariant(lightData.size() > FScene::DIRECTIONAL_LIGHTS_COUNT);

    auto const* UTILS_RESTRICT sphereArray     = lightData.data<FScene::POSITION_RADIUS>();
    auto const* UTILS_RESTRICT directions      = lightData.data<FScene::DIRECTION>();
    auto const* UTILS_RESTRICT instanceArray   = lightData.data<FScene::LIGHT_INSTANCE>();
    auto      * UTILS_RESTRICT visibleArray    = lightData.data<FScene::VISIBILITY>();

    Culler::intersects(visibleArray, frustum, sphereArray, lightData.size());

    const float4* const UTILS_RESTRICT planes = frustum.getNormalizedPlanes();
    // the directional light is considered visible
    size_t visibleLightCount = FScene::DIRECTIONAL_LIGHTS_COUNT;
    // skip directional light
    for (size_t i = FScene::DIRECTIONAL_LIGHTS_COUNT; i < lightData.size(); i++) {
        FLightManager::Instance li = instanceArray[i];
        if (visibleArray[i]) {
            if (!lcm.isLightCaster(li)) {
                visibleArray[i] = 0;
                continue;
            }
            if (lcm.getIntensity(li) <= 0.0f) {
                visibleArray[i] = 0;
                continue;
            }
            // cull spotlights that cannot possibly intersect the view frustum
            if (lcm.isSpotLight(li)) {
                const float3 position = sphereArray[i].xyz;
                const float3 axis = directions[i];
                const float cosSqr = lcm.getCosOuterSquared(li);
                bool invisible = false;
                for (size_t j = 0; j < 6; ++j) {
                    const float p = dot(position + planes[j].xyz * planes[j].w, planes[j].xyz);
                    const float c = dot(planes[j].xyz, axis);
                    invisible |= ((1.0f - c * c) < cosSqr && c > 0 && p > 0);
                }
                if (invisible) {
                    visibleArray[i] = 0;
                    continue;
                }
            }
            visibleLightCount++;
        }
    }

    // Partition array such that all visible lights appear first
    UTILS_UNUSED_IN_RELEASE auto last =
            std::partition(lightData.begin() + FScene::DIRECTIONAL_LIGHTS_COUNT, lightData.end(),
                    [](auto const& it) {
                        return it.template get<FScene::VISIBILITY>() != 0;
                    });
    assert_invariant(visibleLightCount == size_t(last - lightData.begin()));


    /*
     * Some lights might be left out if there are more than the GPU buffer allows (i.e. 256).
     *
     * We always sort lights by distance to the camera so that:
     * - we can build light trees later
     * - lights farther from the camera are dropped when in excess
     *   Note this doesn't always work well, e.g. for search-lights, we might need to also
     *   take the radius into account.
     * - This helps our limited numbers of spot-shadow as well.
     */

    ArenaScope arena(rootArena.getAllocator());
    size_t const size = visibleLightCount;
    // number of point/spot lights
    size_t const positionalLightCount = size - FScene::DIRECTIONAL_LIGHTS_COUNT;
    if (positionalLightCount) {
        // always allocate at least 4 entries, because the vectorized loops below rely on that
        float* const UTILS_RESTRICT distances =
                arena.allocate<float>((size + 3u) & ~3u, CACHELINE_SIZE);

        // pre-compute the lights' distance to the camera, for sorting below
        // - we don't skip the directional light, because we don't care, it's ignored during sorting
        float4 const* const UTILS_RESTRICT spheres = lightData.data<FScene::POSITION_RADIUS>();
        computeLightCameraDistances(distances, camera, spheres, size);

        // skip directional light
        Zip2Iterator<FScene::LightSoa::iterator, float*> b = { lightData.begin(), distances };
        std::sort(b + FScene::DIRECTIONAL_LIGHTS_COUNT, b + size,
                [](auto const& lhs, auto const& rhs) { return lhs.second < rhs.second; });
    }

    // drop excess lights
    lightData.resize(std::min(size, CONFIG_MAX_LIGHT_COUNT + FScene::DIRECTIONAL_LIGHTS_COUNT));
}

// These methods need to exist so clang honors the __restrict__ keyword, which in turn
// produces much better vectorization. The ALWAYS_INLINE keyword makes sure we actually don't
// pay the price of the call!
UTILS_ALWAYS_INLINE
inline void FView::computeLightCameraDistances(
        float* UTILS_RESTRICT const distances,
        CameraInfo const& UTILS_RESTRICT camera,
        float4 const* UTILS_RESTRICT const spheres, size_t count) noexcept {

    // without this, the vectorization is less efficient
    // we're guaranteed to have a multiple of 4 lights (at least)
    count = uint32_t(count + 3u) & ~3u;
    for (size_t i = 0 ; i < count; i++) {
        const float4 sphere = spheres[i];
        const float4 center = camera.view * sphere.xyz; // camera points towards the -z axis
        distances[i] = length(center);
    }
}

void FView::updatePrimitivesLod(FEngine& engine, const CameraInfo&,
        FScene::RenderableSoa& renderableData, Range visible) noexcept {
    FRenderableManager const& rcm = engine.getRenderableManager();
    for (uint32_t index : visible) {
        uint8_t level = 0; // TODO: pick the proper level of detail
        auto ri = renderableData.elementAt<FScene::RENDERABLE_INSTANCE>(index);
        renderableData.elementAt<FScene::PRIMITIVES>(index) = rcm.getRenderPrimitives(ri, level);
    }
}

void FView::renderShadowMaps(FrameGraph& fg, FEngine& engine, FEngine::DriverApi& driver,
        RenderPass const& pass) noexcept {
    mShadowMapManager.render(fg, engine, driver, pass, *this);
}

void FView::commitFrameHistory(FEngine& engine) noexcept {
    // Here we need to destroy resources in mFrameHistory.back()
    auto& frameHistory = mFrameHistory;
    FrameHistoryEntry& last = frameHistory.back();
    last.color.destroy(engine.getResourceAllocator());

    // and then push the new history entry to the history stack
    frameHistory.commit();
}

void FView::drainFrameHistory(FEngine& engine) noexcept {
    // make sure we free all resources in the history
    for (size_t i = 0; i < mFrameHistory.size(); ++i) {
        commitFrameHistory(engine);
    }
}

void FView::executePickingQueries(backend::DriverApi& driver,
        backend::RenderTargetHandle handle, float scale) noexcept {

    while (mActivePickingQueriesList) {
        FPickingQuery* const pQuery = mActivePickingQueriesList;
        mActivePickingQueriesList = pQuery->next;

        // adjust for dynamic resolution and structure buffer scale
        const uint32_t x = uint32_t(float(pQuery->x) * (scale * mScale.x));
        const uint32_t y = uint32_t(float(pQuery->y) * (scale * mScale.y));
        driver.readPixels(handle, x, y, 1, 1, {
                &pQuery->result.renderable, 4*4, // 4*uint
                // FIXME: RGBA_INTEGER is guaranteed to work. R_INTEGER must be queried.
                backend::PixelDataFormat::RG_INTEGER, backend::PixelDataType::UINT,
                pQuery->handler, [](void* buffer, size_t size, void* user) {
                    FPickingQuery* pQuery = static_cast<FPickingQuery*>(user);
                    pQuery->result.fragCoords = {
                            pQuery->x, pQuery->y,float(1.0 - pQuery->result.depth) };
                    pQuery->callback(pQuery->result, pQuery);
                    FPickingQuery::put(pQuery);
                }, pQuery
        });
    }
}

// ------------------------------------------------------------------------------------------------
// Trampoline calling into private implementation
// ------------------------------------------------------------------------------------------------

void View::setScene(Scene* scene) {
    return upcast(this)->setScene(upcast(scene));
}

Scene* View::getScene() noexcept {
    return upcast(this)->getScene();
}


void View::setCamera(Camera* camera) noexcept {
    upcast(this)->setCameraUser(upcast(camera));
}

Camera& View::getCamera() noexcept {
    return upcast(this)->getCameraUser();
}

void View::setViewport(filament::Viewport const& viewport) noexcept {
    upcast(this)->setViewport(viewport);
}

filament::Viewport const& View::getViewport() const noexcept {
    return upcast(this)->getViewport();
}

void View::setFrustumCullingEnabled(bool culling) noexcept {
    upcast(this)->setFrustumCullingEnabled(culling);
}

bool View::isFrustumCullingEnabled() const noexcept {
    return upcast(this)->isFrustumCullingEnabled();
}

void View::setDebugCamera(Camera* camera) noexcept {
    upcast(this)->setViewingCamera(upcast(camera));
}

void View::setVisibleLayers(uint8_t select, uint8_t values) noexcept {
    upcast(this)->setVisibleLayers(select, values);
}

void View::setName(const char* name) noexcept {
    upcast(this)->setName(name);
}

const char* View::getName() const noexcept {
    return upcast(this)->getName();
}

Camera const* View::getDirectionalLightCamera() const noexcept {
    return upcast(this)->getDirectionalLightCamera();
}

void View::setShadowingEnabled(bool enabled) noexcept {
    upcast(this)->setShadowingEnabled(enabled);
}

void View::setRenderTarget(RenderTarget* renderTarget) noexcept {
    upcast(this)->setRenderTarget(upcast(renderTarget));
}

RenderTarget* View::getRenderTarget() const noexcept {
    return upcast(this)->getRenderTarget();
}

void View::setSampleCount(uint8_t count) noexcept {
    upcast(this)->setSampleCount(count);
}

uint8_t View::getSampleCount() const noexcept {
    return upcast(this)->getSampleCount();
}

void View::setAntiAliasing(AntiAliasing type) noexcept {
    upcast(this)->setAntiAliasing(type);
}

View::AntiAliasing View::getAntiAliasing() const noexcept {
    return upcast(this)->getAntiAliasing();
}

void View::setTemporalAntiAliasingOptions(TemporalAntiAliasingOptions options) noexcept {
    upcast(this)->setTemporalAntiAliasingOptions(options);
}

const View::TemporalAntiAliasingOptions& View::getTemporalAntiAliasingOptions() const noexcept {
    return upcast(this)->getTemporalAntiAliasingOptions();
}

void View::setMultiSampleAntiAliasingOptions(MultiSampleAntiAliasingOptions options) noexcept {
    upcast(this)->setMultiSampleAntiAliasingOptions(options);
}

const View::MultiSampleAntiAliasingOptions& View::getMultiSampleAntiAliasingOptions() const noexcept {
    return upcast(this)->getMultiSampleAntiAliasingOptions();
}

void View::setScreenSpaceReflectionsOptions(ScreenSpaceReflectionsOptions options) noexcept {
    upcast(this)->setScreenSpaceReflectionsOptions(options);
}

const View::ScreenSpaceReflectionsOptions& View::getScreenSpaceReflectionsOptions() const noexcept {
    return upcast(this)->getScreenSpaceReflectionsOptions();
}

void View::setColorGrading(ColorGrading* colorGrading) noexcept {
    return upcast(this)->setColorGrading(upcast(colorGrading));
}

const ColorGrading* View::getColorGrading() const noexcept {
    return upcast(this)->getColorGrading();
}

void View::setDithering(Dithering dithering) noexcept {
    upcast(this)->setDithering(dithering);
}

View::Dithering View::getDithering() const noexcept {
    return upcast(this)->getDithering();
}

void View::setDynamicResolutionOptions(const DynamicResolutionOptions& options) noexcept {
    upcast(this)->setDynamicResolutionOptions(options);
}

View::DynamicResolutionOptions View::getDynamicResolutionOptions() const noexcept {
    return upcast(this)->getDynamicResolutionOptions();
}

void View::setRenderQuality(const RenderQuality& renderQuality) noexcept {
    upcast(this)->setRenderQuality(renderQuality);
}

View::RenderQuality View::getRenderQuality() const noexcept {
    return upcast(this)->getRenderQuality();
}

void View::setPostProcessingEnabled(bool enabled) noexcept {
    upcast(this)->setPostProcessingEnabled(enabled);
}

bool View::isPostProcessingEnabled() const noexcept {
    return upcast(this)->hasPostProcessPass();
}

void View::setFrontFaceWindingInverted(bool inverted) noexcept {
    upcast(this)->setFrontFaceWindingInverted(inverted);
}

bool View::isFrontFaceWindingInverted() const noexcept {
    return upcast(this)->isFrontFaceWindingInverted();
}

void View::setDynamicLightingOptions(float zLightNear, float zLightFar) noexcept {
    upcast(this)->setDynamicLightingOptions(zLightNear, zLightFar);
}

void View::setShadowType(View::ShadowType shadow) noexcept {
    upcast(this)->setShadowType(shadow);
}

void View::setVsmShadowOptions(VsmShadowOptions const& options) noexcept {
    upcast(this)->setVsmShadowOptions(options);
}

View::VsmShadowOptions View::getVsmShadowOptions() const noexcept {
    return upcast(this)->getVsmShadowOptions();
}

void View::setSoftShadowOptions(SoftShadowOptions const& options) noexcept {
    upcast(this)->setSoftShadowOptions(options);
}

SoftShadowOptions View::getSoftShadowOptions() const noexcept {
    return upcast(this)->getSoftShadowOptions();
}

void View::setAmbientOcclusion(View::AmbientOcclusion ambientOcclusion) noexcept {
    upcast(this)->setAmbientOcclusion(ambientOcclusion);
}

View::AmbientOcclusion View::getAmbientOcclusion() const noexcept {
    return upcast(this)->getAmbientOcclusion();
}

void View::setAmbientOcclusionOptions(View::AmbientOcclusionOptions const& options) noexcept {
    upcast(this)->setAmbientOcclusionOptions(options);
}

View::AmbientOcclusionOptions const& View::getAmbientOcclusionOptions() const noexcept {
    return upcast(this)->getAmbientOcclusionOptions();
}

void View::setBloomOptions(View::BloomOptions options) noexcept {
    upcast(this)->setBloomOptions(options);
}

View::BloomOptions View::getBloomOptions() const noexcept {
    return upcast(this)->getBloomOptions();
}

void View::setFogOptions(View::FogOptions options) noexcept {
    upcast(this)->setFogOptions(options);
}

View::FogOptions View::getFogOptions() const noexcept {
    return upcast(this)->getFogOptions();
}

void View::setDepthOfFieldOptions(DepthOfFieldOptions options) noexcept {
    upcast(this)->setDepthOfFieldOptions(options);
}

View::DepthOfFieldOptions View::getDepthOfFieldOptions() const noexcept {
    return upcast(this)->getDepthOfFieldOptions();
}

void View::setVignetteOptions(View::VignetteOptions options) noexcept {
    upcast(this)->setVignetteOptions(options);
}

View::VignetteOptions View::getVignetteOptions() const noexcept {
    return upcast(this)->getVignetteOptions();
}

void View::setBlendMode(BlendMode blendMode) noexcept {
    upcast(this)->setBlendMode(blendMode);
}

View::BlendMode View::getBlendMode() const noexcept {
    return upcast(this)->getBlendMode();
}

uint8_t View::getVisibleLayers() const noexcept {
  return upcast(this)->getVisibleLayers();
}

bool View::isShadowingEnabled() const noexcept {
    return upcast(this)->isShadowingEnabled();
}

void View::setScreenSpaceRefractionEnabled(bool enabled) noexcept {
    upcast(this)->setScreenSpaceRefractionEnabled(enabled);
}

bool View::isScreenSpaceRefractionEnabled() const noexcept {
    return upcast(this)->isScreenSpaceRefractionEnabled();
}

View::PickingQuery& View::pick(uint32_t x, uint32_t y, backend::CallbackHandler* handler,
        View::PickingQueryResultCallback callback) noexcept {
    return upcast(this)->pick(x, y, handler, callback);
}

} // namespace filament
