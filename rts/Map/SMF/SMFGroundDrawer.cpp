/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "SMFReadMap.h"
#include "SMFGroundDrawer.h"
#include "SMFGroundTextures.h"
#include "SMFRenderState.h"
#include "Game/Camera.h"
#include "Map/MapInfo.h"
#include "Map/ReadMap.h"
#include "Map/SMF/Legacy/LegacyMeshDrawer.h"
#include "Map/SMF/ROAM/RoamMeshDrawer.h"
#include "Rendering/GlobalRendering.h"
#include "Rendering/ShadowHandler.h"
#include "Rendering/Env/ISky.h"
#include "Rendering/GL/myGL.h"
#include "Rendering/GL/VertexArray.h"
#include "Rendering/Shaders/Shader.h"
#include "System/Config/ConfigHandler.h"
#include "System/FastMath.h"
#include "System/Log/ILog.h"
#include "System/myMath.h"
#include "System/TimeProfiler.h"


CONFIG(int, GroundDetail).defaultValue(60).minimumValue(0).maximumValue(200).description("Controls how detailed the map geometry will be. On lowered settings, cliffs may appear to be jagged or \"melting\".");
CONFIG(bool, MapBorder).defaultValue(true).description("Draws a solid border at the edges of the map.");


CONFIG(int, MaxDynamicMapLights)
	.defaultValue(1)
	.minimumValue(0);

CONFIG(bool, AdvMapShading).defaultValue(true).safemodeValue(false).description("Enable shaders for terrain rendering and enable so more effects.");
CONFIG(bool, AllowDeferredMapRendering).defaultValue(true).safemodeValue(false);

CONFIG(int, ROAM)
	.defaultValue(Patch::VBO)
	.safemodeValue(Patch::DL)
	.minimumValue(0)
	.maximumValue(Patch::VA)
	.description("Use ROAM for terrain mesh rendering. 0:=disable ROAM, 1=VBO mode, 2=DL mode, 3=VA mode");


CSMFGroundDrawer::CSMFGroundDrawer(CSMFReadMap* rm)
	: smfMap(rm)
	, meshDrawer(NULL)
{
	groundTextures = new CSMFGroundTextures(smfMap);
	meshDrawer = SwitchMeshDrawer((!!configHandler->GetInt("ROAM")) ? SMF_MESHDRAWER_ROAM : SMF_MESHDRAWER_LEGACY);

	smfRenderStateSSP = ISMFRenderState::GetInstance(globalRendering->haveARB, globalRendering->haveGLSL);
	smfRenderStateFFP = ISMFRenderState::GetInstance(                   false,                     false);

	// also set in ::Draw, but UpdateSunDir can be called
	// first if DynamicSun is enabled --> must be non-NULL
	SelectRenderState(false);

	// LH must be initialized before render-state is initialized
	lightHandler.Init(2U, configHandler->GetInt("MaxDynamicMapLights"));

	drawMapEdges = configHandler->GetBool("MapBorder");
	drawDeferred = configHandler->GetBool("AllowDeferredMapRendering");

	// NOTE:
	//     advShading can NOT change at runtime if initially false
	//     (see AdvMapShadingActionExecutor), so we will always use
	//     smfRenderStateFFP (in ::Draw) in that special case and it
	//     does not matter if smfRenderStateSSP is initialized
	groundDetail = configHandler->GetInt("GroundDetail");
	advShading = smfRenderStateSSP->Init(this);

	waterPlaneCamInDispList  = 0;
	waterPlaneCamOutDispList = 0;

	memset(&geomBufferTextureIDs[0], 0, sizeof(geomBufferTextureIDs));
	memset(&geomBufferAttachments[0], 0, sizeof(geomBufferAttachments));

	if (mapInfo->water.hasWaterPlane) {
		waterPlaneCamInDispList = glGenLists(1);
		glNewList(waterPlaneCamInDispList, GL_COMPILE);
		CreateWaterPlanes(false);
		glEndList();

		waterPlaneCamOutDispList = glGenLists(1);
		glNewList(waterPlaneCamOutDispList, GL_COMPILE);
		CreateWaterPlanes(true);
		glEndList();
	}

	if (drawDeferred) {
		drawDeferred &= UpdateGeometryBuffer(true);
	}
}


CSMFGroundDrawer::~CSMFGroundDrawer()
{
	if (geomBuffer.IsValid()) {
		geomBuffer.DetachAll();
		glDeleteTextures(GBUFFER_ATTACHMENT_COUNT, &geomBufferTextureIDs[0]);
	}

	// if ROAM _was_ enabled, the configvar is written in CRoamMeshDrawer's dtor
	if (dynamic_cast<CRoamMeshDrawer*>(meshDrawer) == NULL)
		configHandler->Set("ROAM", 0);
	configHandler->Set("GroundDetail", groundDetail);

	smfRenderStateSSP->Kill(); ISMFRenderState::FreeInstance(smfRenderStateSSP);
	smfRenderStateFFP->Kill(); ISMFRenderState::FreeInstance(smfRenderStateFFP);

	delete groundTextures;
	delete meshDrawer;

	if (waterPlaneCamInDispList) {
		glDeleteLists(waterPlaneCamInDispList, 1);
		glDeleteLists(waterPlaneCamOutDispList, 1);
	}

	lightHandler.Kill();
}


IMeshDrawer* CSMFGroundDrawer::SwitchMeshDrawer(int mode)
{
	const int curMode = (dynamic_cast<CRoamMeshDrawer*>(meshDrawer) ? SMF_MESHDRAWER_ROAM : SMF_MESHDRAWER_LEGACY);

	// mode == -1: toggle modes
	if (mode < 0) {
		mode = curMode + 1;
		mode %= SMF_MESHDRAWER_LAST;
	}

	if ((curMode == mode) && (meshDrawer != NULL))
		return meshDrawer;

	delete meshDrawer;

	switch (mode) {
		case SMF_MESHDRAWER_LEGACY:
			LOG("Switching to Legacy Mesh Rendering");
			meshDrawer = new CLegacyMeshDrawer(smfMap, this);
			break;
		default:
			LOG("Switching to ROAM Mesh Rendering");
			meshDrawer = new CRoamMeshDrawer(smfMap, this);
			break;
	}

	return meshDrawer;
}



void CSMFGroundDrawer::CreateWaterPlanes(bool camOufOfMap) {
	glDisable(GL_TEXTURE_2D);
	glDepthMask(GL_FALSE);

	const float xsize = (smfMap->mapSizeX) >> 2;
	const float ysize = (smfMap->mapSizeZ) >> 2;
	const float size = std::min(xsize, ysize);

	CVertexArray* va = GetVertexArray();
	va->Initialize();

	const unsigned char fogColor[4] = {
		(unsigned char)(255 * mapInfo->atmosphere.fogColor[0]),
		(unsigned char)(255 * mapInfo->atmosphere.fogColor[1]),
		(unsigned char)(255 * mapInfo->atmosphere.fogColor[2]),
		 255
	};

	const unsigned char planeColor[4] = {
		(unsigned char)(255 * mapInfo->water.planeColor[0]),
		(unsigned char)(255 * mapInfo->water.planeColor[1]),
		(unsigned char)(255 * mapInfo->water.planeColor[2]),
		 255
	};

	const float alphainc = fastmath::PI2 / 32;
	float alpha,r1,r2;

	float3 p(0.0f, std::min(-200.0f, smfMap->GetInitMinHeight() - 400.0f), 0.0f);

	for (int n = (camOufOfMap) ? 0 : 1; n < 4 ; ++n) {
		if ((n == 1) && !camOufOfMap) {
			// don't render vertices under the map
			r1 = 2 * size;
		} else {
			r1 = n*n * size;
		}

		if (n == 3) {
			// last stripe: make it thinner (looks better with fog)
			r2 = (n+0.5)*(n+0.5) * size;
		} else {
			r2 = (n+1)*(n+1) * size;
		}
		for (alpha = 0.0f; (alpha - fastmath::PI2) < alphainc ; alpha += alphainc) {
			p.x = r1 * fastmath::sin(alpha) + 2 * xsize;
			p.z = r1 * fastmath::cos(alpha) + 2 * ysize;
			va->AddVertexC(p, planeColor );
			p.x = r2 * fastmath::sin(alpha) + 2 * xsize;
			p.z = r2 * fastmath::cos(alpha) + 2 * ysize;
			va->AddVertexC(p, (n==3) ? fogColor : planeColor);
		}
	}
	va->DrawArrayC(GL_TRIANGLE_STRIP);

	glDepthMask(GL_TRUE);
}


inline void CSMFGroundDrawer::DrawWaterPlane(bool drawWaterReflection) {
	if (!drawWaterReflection) {
		const bool skipUnderground = (camera->GetPos().IsInBounds() && !mapInfo->map.voidWater);
		const unsigned int dispList = skipUnderground ? waterPlaneCamInDispList: waterPlaneCamOutDispList;

		glCallList(dispList);
	}
}


void DrawDeferredDebug(unsigned int texID)
{
	glPushMatrix();
	glLoadIdentity();
	glMatrixMode(GL_PROJECTION);
	glPushMatrix();
	glLoadIdentity();

	glActiveTexture(GL_TEXTURE0);
	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, texID);
	glBegin(GL_QUADS);
	glTexCoord2f(0.0f, 0.0f); glNormal3f(0.0f, 1.0f, 0.0f); glVertex2f(0.0f, 0.0f);
	glTexCoord2f(1.0f, 0.0f); glNormal3f(0.0f, 1.0f, 0.0f); glVertex2f(1.0f, 0.0f);
	glTexCoord2f(1.0f, 1.0f); glNormal3f(0.0f, 1.0f, 0.0f); glVertex2f(1.0f, 1.0f);
	glTexCoord2f(0.0f, 1.0f); glNormal3f(0.0f, 1.0f, 0.0f); glVertex2f(0.0f, 1.0f);

	glEnd();
	glBindTexture(GL_TEXTURE_2D, 0);
	glDisable(GL_TEXTURE_2D);

	glPopMatrix();
	glMatrixMode(GL_MODELVIEW);
	glPopMatrix();
}

void CSMFGroundDrawer::DrawDeferredPass(const DrawPass::e& drawPass)
{
	if (!geomBuffer.IsValid())
		return;

	// water renderers use FBO's for the reflection pass
	if (drawPass == DrawPass::WaterReflection)
		return;
	// deferred pass must be executed with GLSL shaders
	if (!smfRenderState->CanDrawDeferred())
		return;

	geomBuffer.Bind();

	// clear color buffer so it contains only null-normals
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	{
		// switch current SSP shader to deferred version
		smfRenderState->SetCurrentShader(DrawPass::TerrainDeferred);
		smfRenderState->Enable(this, drawPass);

		if (mapInfo->map.voidGround || (mapInfo->map.voidWater && drawPass != DrawPass::WaterReflection)) {
			glEnable(GL_ALPHA_TEST);
			glAlphaFunc(GL_GREATER, mapInfo->map.voidAlphaMin);
		}

		meshDrawer->DrawMesh(drawPass);

		if (mapInfo->map.voidGround || (mapInfo->map.voidWater && drawPass != DrawPass::WaterReflection)) {
			glDisable(GL_ALPHA_TEST);
		}

		smfRenderState->Disable(this, drawPass);
		smfRenderState->SetCurrentShader(drawPass);
	}

	geomBuffer.Unbind();

	#if 0
	DrawDeferredDebug(geomBufferTextureIDs[GBUFFER_ATTACHMENT_NORMTEX]);
	#endif
}

void CSMFGroundDrawer::Draw(const DrawPass::e& drawPass)
{
	// must be here because water renderers also call us
	if (!globalRendering->drawGround)
		return;
	// if entire map is under voidwater, no need to draw *ground*
	if (readMap->HasOnlyVoidWater())
		return;

	SelectRenderState(smfRenderStateSSP->CanEnable(this));
	UpdateCamRestraints(cam2);

	glDisable(GL_BLEND);
	glEnable(GL_CULL_FACE);
	glCullFace(GL_BACK);

	if (drawDeferred) {
		// do the deferred pass first, will allow us to re-use
		// its output at some future point and eventually draw
		// the entire map deferred
		DrawDeferredPass(drawPass);
	}

	{
		smfRenderState->Enable(this, drawPass);

		if (wireframe) {
			glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
		}
		if (mapInfo->map.voidGround || (mapInfo->map.voidWater && drawPass != DrawPass::WaterReflection)) {
			glEnable(GL_ALPHA_TEST);
			glAlphaFunc(GL_GREATER, mapInfo->map.voidAlphaMin);
		}

		meshDrawer->DrawMesh(drawPass);

		if (mapInfo->map.voidGround || (mapInfo->map.voidWater && drawPass != DrawPass::WaterReflection)) {
			glDisable(GL_ALPHA_TEST);
		}
		if (wireframe) {
			glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
		}

		smfRenderState->Disable(this, drawPass);
	}

	glDisable(GL_CULL_FACE);

	if (drawPass == DrawPass::Normal) {
		if (mapInfo->water.hasWaterPlane) {
			DrawWaterPlane(false);
		}

		if (drawMapEdges) {
			SCOPED_TIMER("CSMFGroundDrawer::DrawBorder");
			DrawBorder(drawPass);
		}
	}
}


void CSMFGroundDrawer::DrawBorder(const DrawPass::e drawPass)
{
	glEnable(GL_CULL_FACE);
	glCullFace(GL_BACK);

	SelectRenderState(false);
	// smfRenderState->Enable(this, drawPass);

	glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

	glActiveTexture(GL_TEXTURE2);
	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, smfMap->GetDetailTexture());

	//glMultiTexCoord4f(GL_TEXTURE2_ARB, 1.0f, 1.0f, 1.0f, 1.0f);
	//SetTexGen(1.0f / (gs->pwr2mapx * SQUARE_SIZE), 1.0f / (gs->pwr2mapy * SQUARE_SIZE), -0.5f / gs->pwr2mapx, -0.5f / gs->pwr2mapy);

	static const GLfloat planeX[] = {0.005f, 0.0f, 0.005f, 0.5f};
	static const GLfloat planeZ[] = {0.0f, 0.005f, 0.0f, 0.5f};

	glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_EYE_LINEAR);
	glTexGenfv(GL_S, GL_EYE_PLANE, planeX);
	glEnable(GL_TEXTURE_GEN_S);

	glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, GL_EYE_LINEAR);
	glTexGenfv(GL_T, GL_EYE_PLANE, planeZ);
	glEnable(GL_TEXTURE_GEN_T);

	glActiveTexture(GL_TEXTURE3);
	glDisable(GL_TEXTURE_2D);
	glActiveTexture(GL_TEXTURE1);
	glDisable(GL_TEXTURE_2D);
	glActiveTexture(GL_TEXTURE0);
	glEnable(GL_TEXTURE_2D); // needed for the non-shader case

	glEnable(GL_BLEND);

		if (wireframe) {
			glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
		}
			/*if (mapInfo->map.voidWater && (drawPass != DrawPass::WaterReflection)) {
				glEnable(GL_ALPHA_TEST);
				glAlphaFunc(GL_GREATER, 0.9f);
			}*/

				meshDrawer->DrawBorderMesh(drawPass);

			/*if (mapInfo->map.voidWater && (drawPass != DrawPass::WaterReflection)) {
				glDisable(GL_ALPHA_TEST);
			}*/
		if (wireframe) {
			glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
		}

	glDisable(GL_BLEND);

	glActiveTexture(GL_TEXTURE2);
	glDisable(GL_TEXTURE_2D);
	glDisable(GL_TEXTURE_GEN_S);
	glDisable(GL_TEXTURE_GEN_T);

	glActiveTexture(GL_TEXTURE0);
	glDisable(GL_TEXTURE_2D);

	smfRenderState->Disable(this, drawPass);
	glDisable(GL_CULL_FACE);
}


void CSMFGroundDrawer::DrawShadowPass()
{
	if (!globalRendering->drawGround)
		return;
	if (readMap->HasOnlyVoidWater())
		return;

	Shader::IProgramObject* po = shadowHandler->GetShadowGenProg(CShadowHandler::SHADOWGEN_PROGRAM_MAP);

	glEnable(GL_POLYGON_OFFSET_FILL);

	glPolygonOffset(-1.f, -1.f);
		po->Enable();
			meshDrawer->DrawMesh(DrawPass::Shadow);
		po->Disable();

	glDisable(GL_POLYGON_OFFSET_FILL);
}



void CSMFGroundDrawer::SetupBaseDrawPass() { smfRenderStateSSP->SetCurrentShader(DrawPass::Normal); }
void CSMFGroundDrawer::SetupReflDrawPass() { smfRenderStateSSP->SetCurrentShader(DrawPass::WaterReflection); }
void CSMFGroundDrawer::SetupRefrDrawPass() { smfRenderStateSSP->SetCurrentShader(DrawPass::WaterRefraction); }

void CSMFGroundDrawer::SetupBigSquare(const int bigSquareX, const int bigSquareY)
{
	groundTextures->BindSquareTexture(bigSquareX, bigSquareY);
	smfRenderState->SetSquareTexGen(bigSquareX, bigSquareY);
}



void CSMFGroundDrawer::Update()
{
	if (readMap->HasOnlyVoidWater())
		return;

	groundTextures->DrawUpdate();
	meshDrawer->Update();

	if (drawDeferred) {
		drawDeferred &= UpdateGeometryBuffer(false);
	}
}

void CSMFGroundDrawer::UpdateSunDir() {
	/* the GLSL shader may run even w/o shadows and depends on a correct sunDir
	if (!shadowHandler->shadowsLoaded) {
		return;
	}
	*/

	smfRenderState->UpdateCurrentShader(sky->GetLight());
}



bool CSMFGroundDrawer::CreateGeometryBuffer(const int2 size)
{
	geomBuffer.Bind();

	for (unsigned int n = 0; n < GBUFFER_ATTACHMENT_COUNT; n++) {
		glGenTextures(1, &geomBufferTextureIDs[n]);
		glBindTexture(GL_TEXTURE_2D, geomBufferTextureIDs[n]);

		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

		if (n == GBUFFER_ATTACHMENT_ZVALTEX) {
			glTexParameteri(GL_TEXTURE_2D, GL_DEPTH_TEXTURE_MODE, GL_LUMINANCE);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32, size.x, size.y, 0, GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
			geomBufferAttachments[n] = GL_DEPTH_ATTACHMENT_EXT;
		} else {
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, size.x, size.y, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
			geomBufferAttachments[n] = GL_COLOR_ATTACHMENT0_EXT + n;
		}

		geomBuffer.AttachTexture(geomBufferTextureIDs[n], GL_TEXTURE_2D, geomBufferAttachments[n]);
	}

	glBindTexture(GL_TEXTURE_2D, 0);
	// define the attachments we are going to draw into
	// note: the depth-texture attachment does not count
	// here and will be GL_NONE implicitly!
	glDrawBuffers(GBUFFER_ATTACHMENT_COUNT - 1, &geomBufferAttachments[0]);

	// FBO must have been valid from point of construction
	// if we reached CreateGeometryBuffer, but CheckStatus
	// can still invalidate it
	assert(geomBuffer.IsValid());

	const bool ret = geomBuffer.CheckStatus("SMF-GBUFFER");

	geomBuffer.Unbind();
	return ret;
}

bool CSMFGroundDrawer::UpdateGeometryBuffer(bool init)
{
	assert(drawDeferred);

	// NOTE:
	//   Lua can toggle drawDeferred and might be the
	//   first to call us --> initial buffer size must
	//   be (0, 0) so prevSize != currSize (when !init)
	static int2 prevBufferSize = GetGeomBufferSize(false);
	 const int2 currBufferSize = GetGeomBufferSize(true);

	// FBO must be valid from point of construction
	if (!geomBuffer.IsValid())
		return false;

	if (geomBuffer.GetStatus() == GL_FRAMEBUFFER_COMPLETE_EXT) {
		// FBO cannot be complete yet during init!
		assert(!init);

		// FBO was already initialized (during init
		// or from Lua) so it will have attachments
		// --> check if they need to be regenerated
		// (eg. if a window resize event happened)
		if (prevBufferSize == currBufferSize)
			return true;

		geomBuffer.DetachAll();
		glDeleteTextures(GBUFFER_ATTACHMENT_COUNT, &geomBufferTextureIDs[0]);
	}

	return (CreateGeometryBuffer(prevBufferSize = currBufferSize));
}



void CSMFGroundDrawer::IncreaseDetail()
{
	groundDetail += 2;
	LOG("GroundDetail is now %i", groundDetail);
}

void CSMFGroundDrawer::DecreaseDetail()
{
	if (groundDetail > 4) {
		groundDetail -= 2;
		LOG("GroundDetail is now %i", groundDetail);
	}
}



int CSMFGroundDrawer::GetGroundDetail(const DrawPass::e& drawPass) const
{
	int detail = groundDetail;

	switch (drawPass) {
		case DrawPass::UnitReflection:
			detail *= LODScaleUnitReflection;
			break;
		case DrawPass::WaterReflection:
			detail *= LODScaleReflection;
			break;
		case DrawPass::WaterRefraction:
			detail *= LODScaleRefraction;
			break;
		//TODO: currently the shadow mesh needs to be idential with the normal pass one
		//  else we get z-fighting issues in the shadows. Ideal would be a special
		//  shadow pass mesh renderer that reduce the mesh to `walls`/contours that cause the
		//  same shadows as the original terrain
		//case DrawPass::Shadow:
		//	detail *= LODScaleShadow;
		//	break;
		default:
			break;
	}

	return std::max(4, detail);
}

