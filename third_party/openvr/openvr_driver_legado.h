#pragma once

// Versões antigas de interfaces do openvr_driver.h, copiadas sem mudança do SDK OpenVR
// oficial (github.com/ValveSoftware/openvr, mesma licença BSD de LICENSE). Drivers ainda
// compilados com esses SDKs (ex.: o Pico Connect) pedem estas versões ao SteamVR.
//   IVRDriverDirectModeComponent_008: headers/openvr_driver.h da tag v2.2.3
//   IVRDisplayComponent_002:          headers/openvr_driver.h da tag v1.26.7
// Só ficam num namespace próprio para conviver com as versões atuais.

#include "openvr_driver.h"

namespace vr
{
namespace legado
{

	// ---- tag v1.26.7 ----
	class IVRDisplayComponent
	{
	public:

		// ------------------------------------
		// Display Methods
		// ------------------------------------

		/** Size and position that the window needs to be on the VR display. */
		virtual void GetWindowBounds( int32_t *pnX, int32_t *pnY, uint32_t *pnWidth, uint32_t *pnHeight ) = 0;

		/** Returns true if the display is extending the desktop. */
		virtual bool IsDisplayOnDesktop( ) = 0;

		/** Returns true if the display is real and not a fictional display. */
		virtual bool IsDisplayRealDisplay( ) = 0;

		/** Suggested size for the intermediate render target that the distortion pulls from. */
		virtual void GetRecommendedRenderTargetSize( uint32_t *pnWidth, uint32_t *pnHeight ) = 0;

		/** Gets the viewport in the frame buffer to draw the output of the distortion into */
		virtual void GetEyeOutputViewport( EVREye eEye, uint32_t *pnX, uint32_t *pnY, uint32_t *pnWidth, uint32_t *pnHeight ) = 0;

		/** The components necessary to build your own projection matrix in case your
		* application is doing something fancy like infinite Z */
		virtual void GetProjectionRaw( EVREye eEye, float *pfLeft, float *pfRight, float *pfTop, float *pfBottom ) = 0;

		/** Returns the result of the distortion function for the specified eye and input UVs. UVs go from 0,0 in
		* the upper left of that eye's viewport and 1,1 in the lower right of that eye's viewport. */
		virtual DistortionCoordinates_t ComputeDistortion( EVREye eEye, float fU, float fV ) = 0;

	};

	static const char *IVRDisplayComponent_Version = "IVRDisplayComponent_002";

	// ---- tag v2.2.3 ----
	class IVRDriverDirectModeComponent
	{
	public:

		// -----------------------------------
		// Direct mode methods
		// -----------------------------------

		struct SwapTextureSetDesc_t
		{
			uint32_t nWidth;
			uint32_t nHeight;
			uint32_t nFormat;
			uint32_t nSampleCount;
		};

		struct SwapTextureSet_t
		{
			vr::SharedTextureHandle_t rSharedTextureHandles[ 3 ];
			uint32_t unTextureFlags;
		};

		/** Called to allocate textures for applications to render into.  One of these per eye will be passed back to SubmitLayer each frame. */
		virtual void CreateSwapTextureSet( uint32_t unPid, const SwapTextureSetDesc_t *pSwapTextureSetDesc, SwapTextureSet_t *pOutSwapTextureSet ) {}

		/** Used to textures created using CreateSwapTextureSet.  Only one of the set's handles needs to be used to destroy the entire set. */
		virtual void DestroySwapTextureSet( vr::SharedTextureHandle_t sharedTextureHandle ) {}

		/** Used to purge all texture sets for a given process. */
		virtual void DestroyAllSwapTextureSets( uint32_t unPid ) {}

		/** After Present returns, calls this to get the next index to use for rendering. */
		virtual void GetNextSwapTextureSetIndex( vr::SharedTextureHandle_t sharedTextureHandles[ 2 ], uint32_t( *pIndices )[ 2 ] ) {}

		/** Call once per layer to draw for this frame.  One shared texture handle per eye.  Textures must be created
		* using CreateSwapTextureSet and should be alternated per frame.  Call Present once all layers have been submitted. */
		struct SubmitLayerPerEye_t
		{
			// Shared texture handles (depth not always provided).
			vr::SharedTextureHandle_t hTexture, hDepthTexture;

			// Valid region of provided texture (and depth).
			vr::VRTextureBounds_t bounds;

			// Projection matrix used to render the depth buffer.
			vr::HmdMatrix44_t mProjection;

			// Hmd pose used to render this layer.
			vr::HmdMatrix34_t mHmdPose;
		};
		virtual void SubmitLayer( const SubmitLayerPerEye_t( &perEye )[ 2 ] ) {}

		/** Submits queued layers for display. */
		virtual void Present( vr::SharedTextureHandle_t syncTexture ) {}

		/** Called after Present to allow driver to take more time until vsync after they've successfully acquired the sync texture in Present.
		* Set Prop_Hmd_SupportsAppThrottling_Bool to enable throttling / prediction UI in per-app video settings. */
		struct Throttling_t
		{
			uint32_t nFramesToThrottle;
			uint32_t nAdditionalFramesToPredict;
		};
		virtual void PostPresent( const Throttling_t *pThrottling ) {}

		/** Called to get additional frame timing stats from driver.  Check m_nSize for versioning (new members will be added to end only). */
		virtual void GetFrameTiming( DriverDirectMode_FrameTiming *pFrameTiming ) {}
	};

	static const char *IVRDriverDirectModeComponent_Version = "IVRDriverDirectModeComponent_008";

} // namespace legado
} // namespace vr
