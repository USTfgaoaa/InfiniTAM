// Copyright 2014-2015 Isis Innovation Limited and the authors of InfiniTAM

#include "ITMExtendedTracker.h"
#include "../../../ORUtils/Cholesky.h"

#include "../../../ORUtils/FileUtils.h"

#include <math.h>

using namespace ITMLib;

ITMExtendedTracker::ITMExtendedTracker(Vector2i imgSize, TrackerIterationType *trackingRegime, int noHierarchyLevels,
	float terminationThreshold, float failureDetectorThreshold, const ITMLowLevelEngine *lowLevelEngine, MemoryDeviceType memoryType)
{
	viewHierarchy = new ITMImageHierarchy<ITMExtendHierarchyLevel>(imgSize, trackingRegime, noHierarchyLevels, memoryType, true);
	sceneHierarchy = new ITMImageHierarchy<ITMSceneHierarchyLevel>(imgSize, trackingRegime, noHierarchyLevels, memoryType, true);

	this->noIterationsPerLevel = new int[noHierarchyLevels];
	this->spaceThresh = new float[noHierarchyLevels];

	SetupLevels(noHierarchyLevels * 2, 2, 0.01f, 0.002f);

	this->lowLevelEngine = lowLevelEngine;

	this->terminationThreshold = terminationThreshold;

	map = new ORUtils::HomkerMap(2);
	svmClassifier = new ORUtils::SVMClassifier(map->getDescriptorSize(4));

	//all below obtained from dataset in matlab
	float w[20];
	w[0] = -3.15813f; w[1] = -2.38038f; w[2] = 1.93359f; w[3] = 1.56642f; w[4] = 1.76306f;
	w[5] = -0.747641f; w[6] = 4.41852f; w[7] = 1.72048f; w[8] = -0.482545f; w[9] = -5.07793f;
	w[10] = 1.98676f; w[11] = -0.45688f; w[12] = 2.53969f; w[13] = -3.50527f; w[14] = -1.68725f;
	w[15] = 2.31608f; w[16] = 5.14778f; w[17] = 2.31334f; w[18] = -14.128f; w[19] = 6.76423f;

	float b = 9.334260e-01f + failureDetectorThreshold;

	mu = Vector4f(-34.9470512137603f, -33.1379108518478f, 0.195948598235857f, 0.611027292662361f);
	sigma = Vector4f(68.1654461020426f, 60.6607826748643f, 0.00343068557187040f, 0.0402595570918749f);

	svmClassifier->SetVectors(w, b);

	currentFrameNo = 0;
}

ITMExtendedTracker::~ITMExtendedTracker(void)
{
	delete this->viewHierarchy;
	delete this->sceneHierarchy;

	delete[] this->noIterationsPerLevel;
	delete[] this->spaceThresh;

	delete map;
	delete svmClassifier;
}

void ITMExtendedTracker::SetupLevels(int numIterCoarse, int numIterFine, float spaceThreshCoarse, float spaceThreshFine)
{
	int noHierarchyLevels = viewHierarchy->noLevels;

	if ((numIterCoarse != -1) && (numIterFine != -1)) {
		float step = (float)(numIterCoarse - numIterFine) / (float)(noHierarchyLevels - 1);
		float val = (float)numIterCoarse;
		for (int levelId = noHierarchyLevels - 1; levelId >= 0; levelId--) {
			this->noIterationsPerLevel[levelId] = (int)round(val);
			val -= step;
		}
	}

	if ((spaceThreshCoarse >= 0.0f) && (spaceThreshFine >= 0.0f)) {
		float step = (float)(spaceThreshCoarse - spaceThreshFine) / (float)(noHierarchyLevels - 1);
		float val = spaceThreshCoarse;
		for (int levelId = noHierarchyLevels - 1; levelId >= 0; levelId--) {
			this->spaceThresh[levelId] = val;
			val -= step;
		}
	}
}

inline float ComputeCovarianceDet(Vector3f X_sum, float *XXT_triangle, int noNormals)
{
	X_sum *= 1.0f / (float)noNormals;

	Matrix3f S;
	S.m00 = XXT_triangle[0] / (float)noNormals - X_sum.x * X_sum.x;
	S.m01 = XXT_triangle[1] / (float)noNormals - X_sum.x * X_sum.y;
	S.m02 = XXT_triangle[2] / (float)noNormals - X_sum.x * X_sum.z;
	S.m10 = S.m01;
	S.m11 = XXT_triangle[3] / (float)noNormals - X_sum.y * X_sum.y;
	S.m12 = XXT_triangle[4] / (float)noNormals - X_sum.y * X_sum.z;
	S.m20 = S.m02;
	S.m21 = S.m12;
	S.m22 = XXT_triangle[5] / (float)noNormals - X_sum.z * X_sum.z;

	return S.det();
}

void ITMExtendedTracker::SetEvaluationData(ITMTrackingState *trackingState, const ITMView *view)
{
	//// complexity
	//trackingState->pointCloud->colours->UpdateHostFromDevice();
	//trackingState->pointCloud->locations->UpdateHostFromDevice();

	//Vector4f *points = trackingState->pointCloud->locations->GetData(MEMORYDEVICE_CPU);
	//Vector4f *normals = trackingState->pointCloud->colours->GetData(MEMORYDEVICE_CPU);
	//Vector2i imgSize = view->depth->noDims;

	//view->depthUncertainty->Clear();

	//view->depthConfidence->UpdateHostFromDevice();

	////float *dc = view->depthConfidence->GetData(MEMORYDEVICE_CPU);

	//float *du = view->depthUncertainty->GetData(MEMORYDEVICE_CPU);
	//for (int y = 2; y < imgSize.y - 3; y++) for (int x = 2; x < imgSize.x - 3; x++)
	//{
	//	float XXT_triangle[5]; Vector3f sum(0.0f, 0.0f, 0.0f);
	//	int noNormals = 0;

	//	for (int i = 0; i < 5; i++) XXT_triangle[i] = 0;

	//	for (int offY = -2; offY <= 2; offY++) for (int offX = -2; offX <= 2; offX++)
	//	{
	//		Vector4f X = normals[(x + offX) + (y + offY) * imgSize.x];

	//		if (X.w > -1.0f)
	//		{
	//			XXT_triangle[0] += X.x * X.x; XXT_triangle[1] += X.x * X.y; XXT_triangle[2] += X.x * X.z;
	//			XXT_triangle[3] += X.y * X.y; XXT_triangle[4] += X.y * X.z;
	//			XXT_triangle[5] += X.z * X.z;

	//			sum += X.toVector3();

	//			noNormals++;
	//		}
	//	}

	//	float det = 0;
	//	if (noNormals > 0)
	//		det = ComputeCovarianceDet(sum, XXT_triangle, noNormals);

	//	//if (det > 0) printf("%f\n", det);

	//	//du[x + y * imgSize.x] = points[x + y * imgSize.x].w > 10 ? det : 0.2f;
	//	du[x + y * imgSize.x] = det;
	//}

	//WriteToBIN(view->depthUncertainty->GetData(MEMORYDEVICE_CPU), 640 * 480, "c:/temp/file.bin");

	//view->depthUncertainty->UpdateDeviceFromHost();

	this->trackingState = trackingState;
	this->view = view;

	sceneHierarchy->levels[0]->intrinsics = view->calib->intrinsics_d.projectionParamsSimple.all;
	viewHierarchy->levels[0]->intrinsics = view->calib->intrinsics_d.projectionParamsSimple.all;

	// the image hierarchy allows pointers to external data at level 0
	viewHierarchy->levels[0]->depth = view->depth;
	viewHierarchy->levels[0]->depthUncertainty = view->depthUncertainty;
	viewHierarchy->levels[0]->depthNormals = view->depthNormals;

	sceneHierarchy->levels[0]->pointsMap = trackingState->pointCloud->locations;
	sceneHierarchy->levels[0]->normalsMap = trackingState->pointCloud->colours;

	scenePose = trackingState->pose_pointCloud->GetM();
}

void ITMExtendedTracker::PrepareForEvaluation()
{
	for (int i = 1; i < viewHierarchy->noLevels; i++)
	{
		ITMExtendHierarchyLevel *currentLevelView = viewHierarchy->levels[i], *previousLevelView = viewHierarchy->levels[i - 1];
		
		lowLevelEngine->FilterSubsampleWithHoles(currentLevelView->depth, previousLevelView->depth);
		lowLevelEngine->FilterSubsampleWithHoles(currentLevelView->depthUncertainty, previousLevelView->depthUncertainty);
		lowLevelEngine->FilterSubsampleWithHoles(currentLevelView->depthNormals, previousLevelView->depthNormals);

		currentLevelView->intrinsics = previousLevelView->intrinsics * 0.5f;

		ITMSceneHierarchyLevel *currentLevelScene = sceneHierarchy->levels[i], *previousLevelScene = sceneHierarchy->levels[i - 1];
		//lowLevelEngine->FilterSubsampleWithHoles(currentLevelScene->pointsMap, previousLevelScene->pointsMap);
		//lowLevelEngine->FilterSubsampleWithHoles(currentLevelScene->normalsMap, previousLevelScene->normalsMap);
		currentLevelScene->intrinsics = previousLevelScene->intrinsics * 0.5f;
	}
}

void ITMExtendedTracker::SetEvaluationParams(int levelId)
{
	this->levelId = levelId;
	this->iterationType = viewHierarchy->levels[levelId]->iterationType;
	this->sceneHierarchyLevel = sceneHierarchy->levels[0];
	this->viewHierarchyLevel = viewHierarchy->levels[levelId];
}

void ITMExtendedTracker::ComputeDelta(float *step, float *nabla, float *hessian, bool shortIteration) const
{
	for (int i = 0; i < 6; i++) step[i] = 0;

	if (shortIteration)
	{
		float smallHessian[3 * 3];
		for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) smallHessian[r + c * 3] = hessian[r + c * 6];

		ORUtils::Cholesky cholA(smallHessian, 3);
		cholA.Backsub(step, nabla);
	}
	else
	{
		ORUtils::Cholesky cholA(hessian, 6);
		cholA.Backsub(step, nabla);
	}
}

bool ITMExtendedTracker::HasConverged(float *step) const
{
	float stepLength = 0.0f;
	for (int i = 0; i < 6; i++) stepLength += step[i] * step[i];

	if (sqrt(stepLength) / 6 < terminationThreshold) return true; //converged

	return false;
}

void ITMExtendedTracker::ApplyDelta(const Matrix4f & para_old, const float *delta, Matrix4f & para_new) const
{
	float step[6];

	switch (iterationType)
	{
	case TRACKER_ITERATION_ROTATION:
		step[0] = (float)(delta[0]); step[1] = (float)(delta[1]); step[2] = (float)(delta[2]);
		step[3] = 0.0f; step[4] = 0.0f; step[5] = 0.0f;
		break;
	case TRACKER_ITERATION_TRANSLATION:
		step[0] = 0.0f; step[1] = 0.0f; step[2] = 0.0f;
		step[3] = (float)(delta[0]); step[4] = (float)(delta[1]); step[5] = (float)(delta[2]);
		break;
	default:
	case TRACKER_ITERATION_BOTH:
		step[0] = (float)(delta[0]); step[1] = (float)(delta[1]); step[2] = (float)(delta[2]);
		step[3] = (float)(delta[3]); step[4] = (float)(delta[4]); step[5] = (float)(delta[5]);
		break;
	}

	Matrix4f Tinc;

	Tinc.m00 = 1.0f;		Tinc.m10 = step[2];		Tinc.m20 = -step[1];	Tinc.m30 = step[3];
	Tinc.m01 = -step[2];	Tinc.m11 = 1.0f;		Tinc.m21 = step[0];		Tinc.m31 = step[4];
	Tinc.m02 = step[1];		Tinc.m12 = -step[0];	Tinc.m22 = 1.0f;		Tinc.m32 = step[5];
	Tinc.m03 = 0.0f;		Tinc.m13 = 0.0f;		Tinc.m23 = 0.0f;		Tinc.m33 = 1.0f;

	para_new = Tinc * para_old;
}

void ITMExtendedTracker::UpdatePoseQuality(int noValidPoints_old, float *hessian_good, float f_old)
{
	int noTotalPoints = viewHierarchy->levels[0]->depth->noDims.x * viewHierarchy->levels[0]->depth->noDims.y;

	int noValidPointsMax = lowLevelEngine->CountValidDepths(view->depth);

	float normFactor_v1 = (float)noValidPoints_old / (float)noTotalPoints;
	float normFactor_v2 = (float)noValidPoints_old / (float)noValidPointsMax;

	float det = 0.0f;
	if (iterationType == TRACKER_ITERATION_BOTH) {
		ORUtils::Cholesky cholA(hessian_good, 6);
		det = cholA.Determinant();
		if (isnan(det)) det = 0.0f;
	}

	float det_norm_v1 = 0.0f;
	if (iterationType == TRACKER_ITERATION_BOTH) {
		float h[6 * 6];
		for (int i = 0; i < 6 * 6; ++i) h[i] = hessian_good[i] * normFactor_v1;
		ORUtils::Cholesky cholA(h, 6);
		det_norm_v1 = cholA.Determinant();
		if (isnan(det_norm_v1)) det_norm_v1 = 0.0f;
	}

	float det_norm_v2 = 0.0f;
	if (iterationType == TRACKER_ITERATION_BOTH) {
		float h[6 * 6];
		for (int i = 0; i < 6 * 6; ++i) h[i] = hessian_good[i] * normFactor_v2;
		ORUtils::Cholesky cholA(h, 6);
		det_norm_v2 = cholA.Determinant();
		if (isnan(det_norm_v2)) det_norm_v2 = 0.0f;
	}

	float finalResidual_v2 = sqrt(((float)noValidPoints_old * f_old + (float)(noValidPointsMax - noValidPoints_old) * spaceThresh[0]) / (float)noValidPointsMax);
	float percentageInliers_v2 = (float)noValidPoints_old / (float)noValidPointsMax;

	if (noValidPointsMax != 0 && noTotalPoints != 0 && det_norm_v1 > 0 && det_norm_v2 > 0) {
		Vector4f inputVector(log(det_norm_v1), log(det_norm_v2), finalResidual_v2, percentageInliers_v2);

		Vector4f normalisedVector = (inputVector - mu) / sigma;

		float mapped[20];
		map->evaluate(mapped, normalisedVector.v, 4);

		float score = svmClassifier->Classify(mapped);

		if (score > 0) trackingState->poseQuality = 1.0f;
		else if (score > -10.0f) trackingState->poseQuality = 0.5f;
		else trackingState->poseQuality = 0.2f;

		//printf("score: %f\n", score);
	}
}

void ITMExtendedTracker::TrackCamera(ITMTrackingState *trackingState, const ITMView *view)
{
	this->currentFrameNo++;

	this->SetEvaluationData(trackingState, view);
	this->PrepareForEvaluation();

	float f_old = 1e10, f_new;
	int noValidPoints_new;
	int noValidPoints_old = 0;
	int noTotalPoints = 0;

	float hessian_good[6 * 6], hessian_new[6 * 6], A[6 * 6];
	float nabla_good[6], nabla_new[6];
	float step[6];

	for (int i = 0; i < 6 * 6; ++i) hessian_good[i] = 0.0f;
	for (int i = 0; i < 6; ++i) nabla_good[i] = 0.0f;

	for (int levelId = viewHierarchy->noLevels - 1; levelId >= 0; levelId--)
	{
		this->SetEvaluationParams(levelId);
		if (iterationType == TRACKER_ITERATION_NONE) continue;

		noTotalPoints = viewHierarchy->levels[levelId]->depth->noDims.x * viewHierarchy->levels[levelId]->depth->noDims.y;

		Matrix4f approxInvPose = trackingState->pose_d->GetInvM();
		ORUtils::SE3Pose lastKnownGoodPose(*(trackingState->pose_d));
		f_old = 1e20f;
		noValidPoints_old = 0;
		float lambda = 1.0;

		for (int iterNo = 0; iterNo < noIterationsPerLevel[levelId]; iterNo++)
		{
			// evaluate error function and gradients
			noValidPoints_new = this->ComputeGandH(f_new, nabla_new, hessian_new, approxInvPose);

			// check if error increased. If so, revert
			if ((noValidPoints_new <= 0) || (f_new > f_old)) {
				trackingState->pose_d->SetFrom(&lastKnownGoodPose);
				approxInvPose = trackingState->pose_d->GetInvM();
				lambda *= 10.0f;
			}
			else {
				lastKnownGoodPose.SetFrom(trackingState->pose_d);
				f_old = f_new;
				noValidPoints_old = noValidPoints_new;

				//printf("%d -> %f\n", levelId, f_old);

				for (int i = 0; i < 6 * 6; ++i) hessian_good[i] = hessian_new[i] / noValidPoints_new;
				for (int i = 0; i < 6; ++i) nabla_good[i] = nabla_new[i] / noValidPoints_new;
				lambda /= 10.0f;
			}
			for (int i = 0; i < 6 * 6; ++i) A[i] = hessian_good[i];
			for (int i = 0; i < 6; ++i) A[i + i * 6] *= 1.0f + lambda;

			// compute a new step and make sure we've got an SE3
			ComputeDelta(step, nabla_good, A, iterationType != TRACKER_ITERATION_BOTH);
			ApplyDelta(approxInvPose, step, approxInvPose);
			trackingState->pose_d->SetInvM(approxInvPose);
			trackingState->pose_d->Coerce();
			approxInvPose = trackingState->pose_d->GetInvM();

			// if step is small, assume it's going to decrease the error and finish
			if (HasConverged(step)) break;
		}
	}

	//printf("----\n");

	this->UpdatePoseQuality(noValidPoints_old, hessian_good, f_old);
}