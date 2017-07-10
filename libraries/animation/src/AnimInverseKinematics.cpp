//
//  AnimInverseKinematics.cpp
//
//  Copyright 2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "AnimInverseKinematics.h"

#include <GeometryUtil.h>
#include <GLMHelpers.h>
#include <NumericalConstants.h>
#include <SharedUtil.h>
#include <shared/NsightHelpers.h>
#include <DebugDraw.h>
#include "Rig.h"

#include "ElbowConstraint.h"
#include "SwingTwistConstraint.h"
#include "AnimationLogging.h"
#include "CubicHermiteSpline.h"
#include "AnimUtil.h"

static void lookupJointChainInfo(AnimInverseKinematics::JointChainInfo* jointChainInfos, size_t numJointChainInfos,
                                 int indexA, int indexB,
                                 AnimInverseKinematics::JointChainInfo** jointChainInfoA,
                                 AnimInverseKinematics::JointChainInfo** jointChainInfoB) {
    *jointChainInfoA = nullptr;
    *jointChainInfoB = nullptr;
    for (size_t i = 0; i < numJointChainInfos; i++) {
        if (jointChainInfos[i].jointIndex == indexA) {
            *jointChainInfoA = jointChainInfos + i;
        }
        if (jointChainInfos[i].jointIndex == indexB) {
            *jointChainInfoB = jointChainInfos + i;
        }
        if (*jointChainInfoA && *jointChainInfoB) {
            break;
        }
    }
}

static float easeOutExpo(float t) {
    return 1.0f - powf(2, -10.0f * t);
}

AnimInverseKinematics::IKTargetVar::IKTargetVar(const QString& jointNameIn, const QString& positionVarIn, const QString& rotationVarIn,
                                                const QString& typeVarIn, const QString& weightVarIn, float weightIn, const std::vector<float>& flexCoefficientsIn,
                                                const QString& poleVectorEnabledVarIn, const QString& poleReferenceVectorVarIn, const QString& poleVectorVarIn) :
    jointName(jointNameIn),
    positionVar(positionVarIn),
    rotationVar(rotationVarIn),
    typeVar(typeVarIn),
    weightVar(weightVarIn),
    poleVectorEnabledVar(poleVectorEnabledVarIn),
    poleReferenceVectorVar(poleReferenceVectorVarIn),
    poleVectorVar(poleVectorVarIn),
    weight(weightIn),
    numFlexCoefficients(flexCoefficientsIn.size()),
    jointIndex(-1)
{
    numFlexCoefficients = std::min(numFlexCoefficients, (size_t)MAX_FLEX_COEFFICIENTS);
    for (size_t i = 0; i < numFlexCoefficients; i++) {
        flexCoefficients[i] = flexCoefficientsIn[i];
    }
}

AnimInverseKinematics::IKTargetVar::IKTargetVar(const IKTargetVar& orig) :
    jointName(orig.jointName),
    positionVar(orig.positionVar),
    rotationVar(orig.rotationVar),
    typeVar(orig.typeVar),
    weightVar(orig.weightVar),
    poleVectorEnabledVar(orig.poleVectorEnabledVar),
    poleReferenceVectorVar(orig.poleReferenceVectorVar),
    poleVectorVar(orig.poleVectorVar),
    weight(orig.weight),
    numFlexCoefficients(orig.numFlexCoefficients),
    jointIndex(orig.jointIndex)
{
    numFlexCoefficients = std::min(numFlexCoefficients, (size_t)MAX_FLEX_COEFFICIENTS);
    for (size_t i = 0; i < numFlexCoefficients; i++) {
        flexCoefficients[i] = orig.flexCoefficients[i];
    }
}

AnimInverseKinematics::AnimInverseKinematics(const QString& id) : AnimNode(AnimNode::Type::InverseKinematics, id) {
}

AnimInverseKinematics::~AnimInverseKinematics() {
    clearConstraints();
    _rotationAccumulators.clear();
    _translationAccumulators.clear();
    _targetVarVec.clear();
}

void AnimInverseKinematics::loadDefaultPoses(const AnimPoseVec& poses) {
    _defaultRelativePoses = poses;
    assert(_skeleton && _skeleton->getNumJoints() == (int)poses.size());
}

void AnimInverseKinematics::loadPoses(const AnimPoseVec& poses) {
    assert(_skeleton && ((poses.size() == 0) || (_skeleton->getNumJoints() == (int)poses.size())));
    if (_skeleton->getNumJoints() == (int)poses.size()) {
        _relativePoses = poses;
        _rotationAccumulators.resize(_relativePoses.size());
        _translationAccumulators.resize(_relativePoses.size());
    } else {
        _relativePoses.clear();
        _rotationAccumulators.clear();
        _translationAccumulators.clear();
    }
}

void AnimInverseKinematics::computeAbsolutePoses(AnimPoseVec& absolutePoses) const {
    int numJoints = (int)_relativePoses.size();
    assert(numJoints <= _skeleton->getNumJoints());
    assert(numJoints == (int)absolutePoses.size());
    for (int i = 0; i < numJoints; ++i) {
        int parentIndex = _skeleton->getParentIndex(i);
        if (parentIndex < 0) {
            absolutePoses[i] = _relativePoses[i];
        } else {
            absolutePoses[i] = absolutePoses[parentIndex] * _relativePoses[i];
        }
    }
}

void AnimInverseKinematics::setTargetVars(const QString& jointName, const QString& positionVar, const QString& rotationVar,
                                          const QString& typeVar, const QString& weightVar, float weight, const std::vector<float>& flexCoefficients,
                                          const QString& poleVectorEnabledVar, const QString& poleReferenceVectorVar, const QString& poleVectorVar) {
    IKTargetVar targetVar(jointName, positionVar, rotationVar, typeVar, weightVar, weight, flexCoefficients, poleVectorEnabledVar, poleReferenceVectorVar, poleVectorVar);

    // if there are dups, last one wins.
    bool found = false;
    for (auto& targetVarIter: _targetVarVec) {
        if (targetVarIter.jointName == jointName) {
            targetVarIter = targetVar;
            found = true;
            break;
        }
    }
    if (!found) {
        // create a new entry
        _targetVarVec.push_back(targetVar);
    }
}

void AnimInverseKinematics::computeTargets(const AnimVariantMap& animVars, std::vector<IKTarget>& targets, const AnimPoseVec& underPoses) {
    // build a list of valid targets from _targetVarVec and animVars
    _maxTargetIndex = -1;
    _hipsTargetIndex = -1;
    bool removeUnfoundJoints = false;

    for (auto& targetVar : _targetVarVec) {
        if (targetVar.jointIndex == -1) {
            // this targetVar hasn't been validated yet...
            int jointIndex = _skeleton->nameToJointIndex(targetVar.jointName);
            if (jointIndex >= 0) {
                // this targetVar has a valid joint --> cache the indices
                targetVar.jointIndex = jointIndex;
            } else {
                qCWarning(animation) << "AnimInverseKinematics could not find jointName" << targetVar.jointName << "in skeleton";
                removeUnfoundJoints = true;
            }
        } else {
            IKTarget target;
            target.setType(animVars.lookup(targetVar.typeVar, (int)IKTarget::Type::RotationAndPosition));
            if (target.getType() != IKTarget::Type::Unknown) {
                AnimPose absPose = _skeleton->getAbsolutePose(targetVar.jointIndex, underPoses);
                glm::quat rotation = animVars.lookupRigToGeometry(targetVar.rotationVar, absPose.rot());
                glm::vec3 translation = animVars.lookupRigToGeometry(targetVar.positionVar, absPose.trans());
                float weight = animVars.lookup(targetVar.weightVar, targetVar.weight);

                target.setPose(rotation, translation);
                target.setIndex(targetVar.jointIndex);
                target.setWeight(weight);
                target.setFlexCoefficients(targetVar.numFlexCoefficients, targetVar.flexCoefficients);

                bool poleVectorEnabled = animVars.lookup(targetVar.poleVectorEnabledVar, false);
                target.setPoleVectorEnabled(poleVectorEnabled);

                glm::vec3 poleVector = animVars.lookupRigToGeometryVector(targetVar.poleVectorVar, Vectors::UNIT_Z);
                target.setPoleVector(glm::normalize(poleVector));

                glm::vec3 poleReferenceVector = animVars.lookupRigToGeometryVector(targetVar.poleReferenceVectorVar, Vectors::UNIT_Z);
                target.setPoleReferenceVector(glm::normalize(poleReferenceVector));

                targets.push_back(target);

                if (targetVar.jointIndex > _maxTargetIndex) {
                    _maxTargetIndex = targetVar.jointIndex;
                }

                // record the index of the hips ik target.
                if (target.getIndex() == _hipsIndex) {
                    _hipsTargetIndex = (int)targets.size() - 1;
                }
            }
        }
    }

    if (removeUnfoundJoints) {
        int numVars = (int)_targetVarVec.size();
        int i = 0;
        while (i < numVars) {
            if (_targetVarVec[i].jointIndex == -1) {
                if (numVars > 1) {
                    // swap i for last element
                    _targetVarVec[i] = _targetVarVec[numVars - 1];
                }
                _targetVarVec.pop_back();
                --numVars;
            } else {
                ++i;
            }
        }
    }
}

void AnimInverseKinematics::solve(const AnimContext& context, const std::vector<IKTarget>& targets) {
    // compute absolute poses that correspond to relative target poses
    AnimPoseVec absolutePoses;
    absolutePoses.resize(_relativePoses.size());
    computeAbsolutePoses(absolutePoses);

    // clear the accumulators before we start the IK solver
    for (auto& accumulator : _rotationAccumulators) {
        accumulator.clearAndClean();
    }
    for (auto& accumulator : _translationAccumulators) {
        accumulator.clearAndClean();
    }

    float maxError = FLT_MAX;
    int numLoops = 0;
    const int MAX_IK_LOOPS = 16;
    const float MAX_ERROR_TOLERANCE = 0.1f; // cm
    while (maxError > MAX_ERROR_TOLERANCE && numLoops < MAX_IK_LOOPS) {
        ++numLoops;

        bool debug = context.getEnableDebugDrawIKChains() && numLoops == MAX_IK_LOOPS;

        // solve all targets
        for (auto& target: targets) {
            if (target.getType() == IKTarget::Type::Spline) {
                solveTargetWithSpline(context, target, absolutePoses, debug);
            } else {
                solveTargetWithCCD(context, target, absolutePoses, debug);
            }
        }

        // harvest accumulated rotations and apply the average
        for (int i = 0; i < (int)_relativePoses.size(); ++i) {
            if (_rotationAccumulators[i].size() > 0) {
                _relativePoses[i].rot() = _rotationAccumulators[i].getAverage();
                _rotationAccumulators[i].clear();
            }
            if (_translationAccumulators[i].size() > 0) {
                _relativePoses[i].trans() = _translationAccumulators[i].getAverage();
                _translationAccumulators[i].clear();
            }
        }

        // update the absolutePoses
        for (int i = 0; i < (int)_relativePoses.size(); ++i) {
            auto parentIndex = _skeleton->getParentIndex((int)i);
            if (parentIndex != -1) {
                absolutePoses[i] = absolutePoses[parentIndex] * _relativePoses[i];
            }
        }

        // compute maxError
        maxError = 0.0f;
        for (size_t i = 0; i < targets.size(); i++) {
            if (targets[i].getType() == IKTarget::Type::RotationAndPosition || targets[i].getType() == IKTarget::Type::HmdHead ||
                targets[i].getType() == IKTarget::Type::HipsRelativeRotationAndPosition) {
                float error = glm::length(absolutePoses[targets[i].getIndex()].trans() - targets[i].getTranslation());
                if (error > maxError) {
                    maxError = error;
                }
            }
        }
    }
    _maxErrorOnLastSolve = maxError;

    // finally set the relative rotation of each tip to agree with absolute target rotation
    for (auto& target: targets) {
        int tipIndex = target.getIndex();
        int parentIndex = _skeleton->getParentIndex(tipIndex);

        // update rotationOnly targets that don't lie on the ik chain of other ik targets.
        if (parentIndex != -1 && !_rotationAccumulators[tipIndex].isDirty() && target.getType() == IKTarget::Type::RotationOnly) {
            const glm::quat& targetRotation = target.getRotation();
            // compute tip's new parent-relative rotation
            // Q = Qp * q   -->   q' = Qp^ * Q
            glm::quat newRelativeRotation = glm::inverse(absolutePoses[parentIndex].rot()) * targetRotation;
            RotationConstraint* constraint = getConstraint(tipIndex);
            if (constraint) {
                constraint->apply(newRelativeRotation);
                // TODO: ATM the final rotation target just fails but we need to provide
                // feedback to the IK system so that it can adjust the bones up the skeleton
                // to help this rotation target get met.
            }
            _relativePoses[tipIndex].rot() = newRelativeRotation;
            absolutePoses[tipIndex].rot() = targetRotation;
        }
    }
}

void AnimInverseKinematics::solveTargetWithCCD(const AnimContext& context, const IKTarget& target, const AnimPoseVec& absolutePoses, bool debug) {
    size_t chainDepth = 0;

    IKTarget::Type targetType = target.getType();
    if (targetType == IKTarget::Type::RotationOnly) {
        // the final rotation will be enforced after the iterations
        // TODO: solve this correctly
        return;
    }

    int tipIndex = target.getIndex();
    int pivotIndex = _skeleton->getParentIndex(tipIndex);
    if (pivotIndex == -1 || pivotIndex == _hipsIndex) {
        return;
    }
    int pivotsParentIndex = _skeleton->getParentIndex(pivotIndex);
    if (pivotsParentIndex == -1) {
        // TODO?: handle case where tip's parent is root?
        return;
    }

    // cache tip's absolute orientation
    glm::quat tipOrientation = absolutePoses[tipIndex].rot();

    // also cache tip's parent's absolute orientation so we can recompute
    // the tip's parent-relative as we proceed up the chain
    glm::quat tipParentOrientation = absolutePoses[pivotIndex].rot();

    const size_t MAX_CHAIN_DEPTH = 30;
    JointChainInfo jointChainInfos[MAX_CHAIN_DEPTH];

    // NOTE: if this code is removed, the head will remain rigid, causing the spine/hips to thrust forward backward
    // as the head is nodded.
    if (targetType == IKTarget::Type::HmdHead ||
        targetType == IKTarget::Type::RotationAndPosition ||
        targetType == IKTarget::Type::HipsRelativeRotationAndPosition) {

        // rotate tip toward target orientation
        glm::quat deltaRot = target.getRotation() * glm::inverse(tipOrientation);

        deltaRot *= target.getFlexCoefficient(chainDepth);
        glm::normalize(deltaRot);

        // compute parent relative rotation
        glm::quat tipRelativeRotation = glm::inverse(tipParentOrientation) * deltaRot * tipOrientation;

        // then enforce tip's constraint
        RotationConstraint* constraint = getConstraint(tipIndex);
        bool constrained = false;
        if (constraint) {
            constrained = constraint->apply(tipRelativeRotation);
            if (constrained) {
                tipOrientation = tipParentOrientation * tipRelativeRotation;
                tipRelativeRotation = tipRelativeRotation;
            }
        }

        glm::vec3 tipRelativeTranslation = _relativePoses[target.getIndex()].trans();
        jointChainInfos[chainDepth] = { tipRelativeRotation, tipRelativeTranslation, target.getWeight(), tipIndex, constrained };
    }

    // cache tip absolute position
    glm::vec3 tipPosition = absolutePoses[tipIndex].trans();

    chainDepth++;

    // descend toward root, pivoting each joint to get tip closer to target position
    while (pivotIndex != _hipsIndex && pivotsParentIndex != -1) {

        assert(chainDepth < MAX_CHAIN_DEPTH);

        // compute the two lines that should be aligned
        glm::vec3 jointPosition = absolutePoses[pivotIndex].trans();
        glm::vec3 leverArm = tipPosition - jointPosition;

        glm::quat deltaRotation;
        if (targetType == IKTarget::Type::RotationAndPosition ||
            targetType == IKTarget::Type::HipsRelativeRotationAndPosition) {
            // compute the swing that would get get tip closer
            glm::vec3 targetLine = target.getTranslation() - jointPosition;

            const float MIN_AXIS_LENGTH = 1.0e-4f;
            RotationConstraint* constraint = getConstraint(pivotIndex);


            // only allow swing on lowerSpine if there is a hips IK target.
            if (_hipsTargetIndex < 0 && constraint && constraint->isLowerSpine() && tipIndex != _headIndex) {
                // for these types of targets we only allow twist at the lower-spine
                // (this prevents the hand targets from bending the spine too much and thereby driving the hips too far)
                glm::vec3 twistAxis = absolutePoses[pivotIndex].trans() - absolutePoses[pivotsParentIndex].trans();
                float twistAxisLength = glm::length(twistAxis);
                if (twistAxisLength > MIN_AXIS_LENGTH) {
                    // project leverArm and targetLine to the plane
                    twistAxis /= twistAxisLength;
                    leverArm -= glm::dot(leverArm, twistAxis) * twistAxis;
                    targetLine -= glm::dot(targetLine, twistAxis) * twistAxis;
                } else {
                    leverArm = Vectors::ZERO;
                    targetLine = Vectors::ZERO;
                }
            }

            glm::vec3 axis = glm::cross(leverArm, targetLine);
            float axisLength = glm::length(axis);
            if (axisLength > MIN_AXIS_LENGTH) {
                // compute angle of rotation that brings tip closer to target
                axis /= axisLength;
                float cosAngle = glm::clamp(glm::dot(leverArm, targetLine) / (glm::length(leverArm) * glm::length(targetLine)), -1.0f, 1.0f);
                float angle = acosf(cosAngle);
                const float MIN_ADJUSTMENT_ANGLE = 1.0e-4f;

                if (angle > MIN_ADJUSTMENT_ANGLE) {
                    // reduce angle by a flexCoefficient
                    angle *= target.getFlexCoefficient(chainDepth);
                    deltaRotation = glm::angleAxis(angle, axis);

                    // The swing will re-orient the tip but there will tend to be be a non-zero delta between the tip's
                    // new orientation and its target.  This is the final parent-relative orientation that the tip joint have
                    // make to achieve its target orientation.
                    glm::quat tipRelativeRotation = glm::inverse(deltaRotation * tipParentOrientation) * target.getRotation();

                    // enforce tip's constraint
                    RotationConstraint* constraint = getConstraint(tipIndex);
                    if (constraint) {
                        bool constrained = constraint->apply(tipRelativeRotation);
                        if (constrained) {
                            // The tip's final parent-relative rotation would violate its constraint
                            // so we try to pre-twist this pivot to compensate.
                            glm::quat constrainedTipRotation = deltaRotation * tipParentOrientation * tipRelativeRotation;
                            glm::quat missingRotation = target.getRotation() * glm::inverse(constrainedTipRotation);
                            glm::quat swingPart;
                            glm::quat twistPart;
                            glm::vec3 axis = glm::normalize(deltaRotation * leverArm);
                            swingTwistDecomposition(missingRotation, axis, swingPart, twistPart);
                            float dotSign = copysignf(1.0f, twistPart.w);
                            const float LIMIT_LEAK_FRACTION = 0.1f;
                            deltaRotation = glm::normalize(glm::lerp(glm::quat(), dotSign * twistPart, LIMIT_LEAK_FRACTION)) * deltaRotation;
                        }
                    }
                }
            }
        } else if (targetType == IKTarget::Type::HmdHead) {
            // An HmdHead target slaves the orientation of the end-effector by distributing rotation
            // deltas up the hierarchy.  Its target position is enforced later (by shifting the hips).
            deltaRotation = target.getRotation() * glm::inverse(tipOrientation);
            float dotSign = copysignf(1.0f, deltaRotation.w);
            const float ANGLE_DISTRIBUTION_FACTOR = 0.45f;
            deltaRotation = glm::normalize(glm::lerp(glm::quat(), dotSign * deltaRotation, ANGLE_DISTRIBUTION_FACTOR));
        }

        // compute joint's new parent-relative rotation after swing
        // Q' = dQ * Q   and   Q = Qp * q   -->   q' = Qp^ * dQ * Q
        glm::quat newRot = glm::normalize(glm::inverse(absolutePoses[pivotsParentIndex].rot()) *
                                          deltaRotation *
                                          absolutePoses[pivotIndex].rot());

        // enforce pivot's constraint
        RotationConstraint* constraint = getConstraint(pivotIndex);
        bool constrained = false;
        if (constraint) {
            constrained = constraint->apply(newRot);
            if (constrained) {
                // the constraint will modify the local rotation of the tip so we must
                // compute the corresponding model-frame deltaRotation
                // Q' = Qp^ * dQ * Q  -->  dQ =   Qp * Q' * Q^
                deltaRotation = absolutePoses[pivotsParentIndex].rot() * newRot * glm::inverse(absolutePoses[pivotIndex].rot());
            }
        }

        glm::vec3 newTrans = _relativePoses[pivotIndex].trans();
        jointChainInfos[chainDepth] = { newRot, newTrans, target.getWeight(), pivotIndex, constrained };

        // keep track of tip's new transform as we descend towards root
        tipPosition = jointPosition + deltaRotation * (tipPosition - jointPosition);
        tipOrientation = glm::normalize(deltaRotation * tipOrientation);
        tipParentOrientation = glm::normalize(deltaRotation * tipParentOrientation);

        pivotIndex = pivotsParentIndex;
        pivotsParentIndex = _skeleton->getParentIndex(pivotIndex);

        chainDepth++;
    }

    if (target.getPoleVectorEnabled()) {
        int topJointIndex = target.getIndex();
        int midJointIndex = _skeleton->getParentIndex(topJointIndex);
        if (midJointIndex != -1) {
            int baseJointIndex = _skeleton->getParentIndex(midJointIndex);
            if (baseJointIndex != -1) {
                int baseParentJointIndex = _skeleton->getParentIndex(baseJointIndex);
                AnimPose topPose, midPose, basePose;
                int topChainIndex = -1, baseChainIndex = -1;
                AnimPose postAbsPoses[MAX_CHAIN_DEPTH];
                AnimPose accum = absolutePoses[_hipsIndex];
                AnimPose baseParentPose = absolutePoses[_hipsIndex];
                for (int i = (int)chainDepth - 1; i >= 0; i--) {
                    accum = accum * AnimPose(glm::vec3(1.0f), jointChainInfos[i].relRot, jointChainInfos[i].relTrans);
                    postAbsPoses[i] = accum;
                    if (jointChainInfos[i].jointIndex == topJointIndex) {
                        topChainIndex = i;
                        topPose = accum;
                    }
                    if (jointChainInfos[i].jointIndex == midJointIndex) {
                        midPose = accum;
                    }
                    if (jointChainInfos[i].jointIndex == baseJointIndex) {
                        baseChainIndex = i;
                        basePose = accum;
                    }
                    if (jointChainInfos[i].jointIndex == baseParentJointIndex) {
                        baseParentPose = accum;
                    }
                }

                glm::quat poleRot = Quaternions::IDENTITY;
                glm::vec3 d = basePose.trans() - topPose.trans();
                float dLen = glm::length(d);
                if (dLen > EPSILON) {
                    glm::vec3 dUnit = d / dLen;
                    glm::vec3 e = midPose.xformVector(target.getPoleReferenceVector());
                    glm::vec3 eProj = e - glm::dot(e, dUnit) * dUnit;
                    float eProjLen = glm::length(eProj);

                    const float MIN_EPROJ_LEN = 0.5f;
                    if (eProjLen < MIN_EPROJ_LEN) {
                        glm::vec3 midPoint = topPose.trans() + d * 0.5f;
                        e = midPose.trans() - midPoint;
                        eProj = e - glm::dot(e, dUnit) * dUnit;
                        eProjLen = glm::length(eProj);
                    }

                    glm::vec3 p = target.getPoleVector();
                    glm::vec3 pProj = p - glm::dot(p, dUnit) * dUnit;
                    float pProjLen = glm::length(pProj);

                    if (eProjLen > EPSILON && pProjLen > EPSILON) {
                        // as pProjLen become orthognal to d, reduce the amount of rotation.
                        float magnitude = easeOutExpo(pProjLen);
                        float dot = glm::clamp(glm::dot(eProj / eProjLen, pProj / pProjLen), 0.0f, 1.0f);
                        float theta = acosf(dot);
                        glm::vec3 cross = glm::cross(eProj, pProj);
                        const float MIN_ADJUSTMENT_ANGLE = 0.001745f;  // 0.1 degree
                        if (theta > MIN_ADJUSTMENT_ANGLE) {
                            glm::vec3 axis = dUnit;
                            if (glm::dot(cross, dUnit) < 0) {
                                axis = -dUnit;
                            }
                            poleRot = glm::angleAxis(magnitude * theta, axis);
                        }
                    }
                }

                if (debug) {
                    const vec4 RED(1.0f, 0.0f, 0.0f, 1.0f);
                    const vec4 GREEN(0.0f, 1.0f, 0.0f, 1.0f);
                    const vec4 BLUE(0.0f, 0.0f, 1.0f, 1.0f);
                    const vec4 YELLOW(1.0f, 1.0f, 0.0f, 1.0f);
                    const vec4 WHITE(1.0f, 1.0f, 1.0f, 1.0f);

                    AnimPose geomToWorldPose = AnimPose(context.getRigToWorldMatrix() * context.getGeometryToRigMatrix());

                    glm::vec3 dUnit = d / dLen;
                    glm::vec3 e = midPose.xformVector(target.getPoleReferenceVector());
                    glm::vec3 eProj = e - glm::dot(e, dUnit) * dUnit;
                    float eProjLen = glm::length(eProj);
                    const float MIN_EPROJ_LEN = 0.5f;
                    if (eProjLen < MIN_EPROJ_LEN) {
                        glm::vec3 midPoint = topPose.trans() + d * 0.5f;
                        e = midPose.trans() - midPoint;
                        eProj = e - glm::dot(e, dUnit) * dUnit;
                        eProjLen = glm::length(eProj);
                    }

                    glm::vec3 p = target.getPoleVector();
                    const float PROJ_VECTOR_LEN = 10.0f;
                    const float POLE_VECTOR_LEN = 100.0f;
                    glm::vec3 midPoint = (basePose.trans() + topPose.trans()) * 0.5f;
                    DebugDraw::getInstance().drawRay(geomToWorldPose.xformPoint(basePose.trans()),
                                                     geomToWorldPose.xformPoint(topPose.trans()),
                                                     YELLOW);
                    DebugDraw::getInstance().drawRay(geomToWorldPose.xformPoint(midPoint),
                                                     geomToWorldPose.xformPoint(midPoint + PROJ_VECTOR_LEN * glm::normalize(e)),
                                                     RED);
                    DebugDraw::getInstance().drawRay(geomToWorldPose.xformPoint(midPoint),
                                                     geomToWorldPose.xformPoint(midPoint + POLE_VECTOR_LEN * glm::normalize(p)),
                                                     BLUE);
                }

                glm::quat newBaseRelRot = glm::inverse(baseParentPose.rot()) * poleRot * basePose.rot();
                jointChainInfos[baseChainIndex].relRot = newBaseRelRot;

                glm::quat newTopRelRot = glm::inverse(midPose.rot()) * glm::inverse(poleRot) * topPose.rot();
                jointChainInfos[topChainIndex].relRot = newTopRelRot;
            }
        }
    }

    for (size_t i = 0; i < chainDepth; i++) {
        _rotationAccumulators[jointChainInfos[i].jointIndex].add(jointChainInfos[i].relRot, jointChainInfos[i].weight);
        _translationAccumulators[jointChainInfos[i].jointIndex].add(jointChainInfos[i].relTrans, jointChainInfos[i].weight);
    }

    if (debug) {
        debugDrawIKChain(jointChainInfos, chainDepth, context);
    }
}

static CubicHermiteSplineFunctorWithArcLength computeSplineFromTipAndBase(const AnimPose& tipPose, const AnimPose& basePose, float baseGain = 1.0f, float tipGain = 1.0f) {
    float linearDistance = glm::length(basePose.trans() - tipPose.trans());
    glm::vec3 p0 = basePose.trans();
    glm::vec3 m0 = baseGain * linearDistance * (basePose.rot() * Vectors::UNIT_Y);
    glm::vec3 p1 = tipPose.trans();
    glm::vec3 m1 = tipGain * linearDistance * (tipPose.rot() * Vectors::UNIT_Y);

    return CubicHermiteSplineFunctorWithArcLength(p0, m0, p1, m1);
}

// pre-compute information about each joint influeced by this spline IK target.
void AnimInverseKinematics::computeSplineJointInfosForIKTarget(const AnimContext& context, const IKTarget& target) {
    std::vector<SplineJointInfo> splineJointInfoVec;

    // build spline between the default poses.
    AnimPose tipPose = _skeleton->getAbsoluteDefaultPose(target.getIndex());
    AnimPose basePose = _skeleton->getAbsoluteDefaultPose(_hipsIndex);

    CubicHermiteSplineFunctorWithArcLength spline;
    if (target.getIndex() == _headIndex) {
        // set gain factors so that more curvature occurs near the tip of the spline.
        const float HIPS_GAIN = 0.5f;
        const float HEAD_GAIN = 1.0f;
        spline = computeSplineFromTipAndBase(tipPose, basePose, HIPS_GAIN, HEAD_GAIN);
    } else {
        spline = computeSplineFromTipAndBase(tipPose, basePose);
    }

    // measure the total arc length along the spline
    float totalArcLength = spline.arcLength(1.0f);

    glm::vec3 baseToTip = tipPose.trans() - basePose.trans();
    float baseToTipLength = glm::length(baseToTip);
    glm::vec3 baseToTipNormal = baseToTip / baseToTipLength;

    int index = target.getIndex();
    int endIndex = _skeleton->getParentIndex(_hipsIndex);
    while (index != endIndex) {
        AnimPose defaultPose = _skeleton->getAbsoluteDefaultPose(index);

        float ratio = glm::dot(defaultPose.trans() - basePose.trans(), baseToTipNormal) / baseToTipLength;

        // compute offset from spline to the default pose.
        float t = spline.arcLengthInverse(ratio * totalArcLength);

        // compute the rotation by using the derivative of the spline as the y-axis, and the defaultPose x-axis
        glm::vec3 y = glm::normalize(spline.d(t));
        glm::vec3 x = defaultPose.rot() * Vectors::UNIT_X;
        glm::vec3 u, v, w;
        generateBasisVectors(y, x, v, u, w);
        glm::mat3 m(u, v, glm::cross(u, v));
        glm::quat rot = glm::normalize(glm::quat_cast(m));

        AnimPose pose(glm::vec3(1.0f), rot, spline(t));
        AnimPose offsetPose = pose.inverse() * defaultPose;

        SplineJointInfo splineJointInfo = { index, ratio, offsetPose };
        splineJointInfoVec.push_back(splineJointInfo);
        index = _skeleton->getParentIndex(index);
    }

    _splineJointInfoMap[target.getIndex()] = splineJointInfoVec;
}

const std::vector<AnimInverseKinematics::SplineJointInfo>* AnimInverseKinematics::findOrCreateSplineJointInfo(const AnimContext& context, const IKTarget& target) {
    // find or create splineJointInfo for this target
    auto iter = _splineJointInfoMap.find(target.getIndex());
    if (iter != _splineJointInfoMap.end()) {
        return &(iter->second);
    } else {
        computeSplineJointInfosForIKTarget(context, target);
        auto iter = _splineJointInfoMap.find(target.getIndex());
        if (iter != _splineJointInfoMap.end()) {
            return &(iter->second);
        }
    }

    return nullptr;
}

void AnimInverseKinematics::solveTargetWithSpline(const AnimContext& context, const IKTarget& target, const AnimPoseVec& absolutePoses, bool debug) {

    const size_t MAX_CHAIN_DEPTH = 30;
    JointChainInfo jointChainInfos[MAX_CHAIN_DEPTH];

    const int baseIndex = _hipsIndex;

    // build spline from tip to base
    AnimPose tipPose = AnimPose(glm::vec3(1.0f), target.getRotation(), target.getTranslation());
    AnimPose basePose = absolutePoses[baseIndex];
    CubicHermiteSplineFunctorWithArcLength spline;
    if (target.getIndex() == _headIndex) {
        // set gain factors so that more curvature occurs near the tip of the spline.
        const float HIPS_GAIN = 0.5f;
        const float HEAD_GAIN = 1.0f;
        spline = computeSplineFromTipAndBase(tipPose, basePose, HIPS_GAIN, HEAD_GAIN);
    } else {
        spline = computeSplineFromTipAndBase(tipPose, basePose);
    }
    float totalArcLength = spline.arcLength(1.0f);

    // This prevents the rotation interpolation from rotating the wrong physical way (but correct mathematical way)
    // when the head is arched backwards very far.
    glm::quat halfRot = glm::normalize(glm::lerp(basePose.rot(), tipPose.rot(), 0.5f));
    if (glm::dot(halfRot * Vectors::UNIT_Z, basePose.rot() * Vectors::UNIT_Z) < 0.0f) {
        tipPose.rot() = -tipPose.rot();
    }

    // find or create splineJointInfo for this target
    const std::vector<SplineJointInfo>* splineJointInfoVec = findOrCreateSplineJointInfo(context, target);

    if (splineJointInfoVec && splineJointInfoVec->size() > 0) {
        const int baseParentIndex = _skeleton->getParentIndex(baseIndex);
        AnimPose parentAbsPose = (baseParentIndex >= 0) ? absolutePoses[baseParentIndex] : AnimPose();

        // go thru splineJointInfoVec backwards (base to tip)
        for (int i = (int)splineJointInfoVec->size() - 1; i >= 0; i--) {
            const SplineJointInfo& splineJointInfo = (*splineJointInfoVec)[i];
            float t = spline.arcLengthInverse(splineJointInfo.ratio * totalArcLength);
            glm::vec3 trans = spline(t);

            // for head splines, preform most twist toward the tip by using ease in function. t^2
            float rotT = t;
            if (target.getIndex() == _headIndex) {
                rotT = t * t;
            }
            glm::quat twistRot = glm::normalize(glm::lerp(basePose.rot(), tipPose.rot(), rotT));

            // compute the rotation by using the derivative of the spline as the y-axis, and the twistRot x-axis
            glm::vec3 y = glm::normalize(spline.d(t));
            glm::vec3 x = twistRot * Vectors::UNIT_X;
            glm::vec3 u, v, w;
            generateBasisVectors(y, x, v, u, w);
            glm::mat3 m(u, v, glm::cross(u, v));
            glm::quat rot = glm::normalize(glm::quat_cast(m));

            AnimPose desiredAbsPose = AnimPose(glm::vec3(1.0f), rot, trans) * splineJointInfo.offsetPose;

            // apply flex coefficent
            AnimPose flexedAbsPose;
            ::blend(1, &absolutePoses[splineJointInfo.jointIndex], &desiredAbsPose, target.getFlexCoefficient(i), &flexedAbsPose);

            AnimPose relPose = parentAbsPose.inverse() * flexedAbsPose;

            bool constrained = false;
            if (splineJointInfo.jointIndex != _hipsIndex) {
                // constrain the amount the spine can stretch or compress
                float length = glm::length(relPose.trans());
                const float EPSILON = 0.0001f;
                if (length > EPSILON) {
                    float defaultLength = glm::length(_skeleton->getRelativeDefaultPose(splineJointInfo.jointIndex).trans());
                    const float STRETCH_COMPRESS_PERCENTAGE = 0.15f;
                    const float MAX_LENGTH = defaultLength * (1.0f + STRETCH_COMPRESS_PERCENTAGE);
                    const float MIN_LENGTH = defaultLength * (1.0f - STRETCH_COMPRESS_PERCENTAGE);
                    if (length > MAX_LENGTH) {
                        relPose.trans() = (relPose.trans() / length) * MAX_LENGTH;
                        constrained = true;
                    } else if (length < MIN_LENGTH) {
                        relPose.trans() = (relPose.trans() / length) * MIN_LENGTH;
                        constrained = true;
                    }
                } else {
                    relPose.trans() = glm::vec3(0.0f);
                }
            }

            jointChainInfos[i] = { relPose.rot(), relPose.trans(), target.getWeight(), splineJointInfo.jointIndex, constrained };

            parentAbsPose = flexedAbsPose;
        }
    }

    for (size_t i = 0; i < splineJointInfoVec->size(); i++) {
        _rotationAccumulators[jointChainInfos[i].jointIndex].add(jointChainInfos[i].relRot, jointChainInfos[i].weight);
        _translationAccumulators[jointChainInfos[i].jointIndex].add(jointChainInfos[i].relTrans, jointChainInfos[i].weight);
    }

    if (debug) {
        debugDrawIKChain(jointChainInfos, splineJointInfoVec->size(), context);
    }
}

//virtual
const AnimPoseVec& AnimInverseKinematics::evaluate(const AnimVariantMap& animVars, const AnimContext& context, float dt, AnimNode::Triggers& triggersOut) {
    // don't call this function, call overlay() instead
    assert(false);
    return _relativePoses;
}


//virtual
const AnimPoseVec& AnimInverseKinematics::overlay(const AnimVariantMap& animVars, const AnimContext& context, float dt, Triggers& triggersOut, const AnimPoseVec& underPoses) {
    // allows solutionSource to be overridden by an animVar
    auto solutionSource = animVars.lookup(_solutionSourceVar, (int)_solutionSource);

    const float MAX_OVERLAY_DT = 1.0f / 30.0f; // what to clamp delta-time to in AnimInverseKinematics::overlay
    if (dt > MAX_OVERLAY_DT) {
        dt = MAX_OVERLAY_DT;
    }

    if (_relativePoses.size() != underPoses.size()) {
        loadPoses(underPoses);
    } else {

        PROFILE_RANGE_EX(simulation_animation, "ik/relax", 0xffff00ff, 0);

        initRelativePosesFromSolutionSource((SolutionSource)solutionSource, underPoses);

        if (!underPoses.empty()) {
            // Sometimes the underpose itself can violate the constraints.  Rather than
            // clamp the animation we dynamically expand each constraint to accomodate it.
            std::map<int, RotationConstraint*>::iterator constraintItr = _constraints.begin();
            while (constraintItr != _constraints.end()) {
                int index = constraintItr->first;
                constraintItr->second->dynamicallyAdjustLimits(underPoses[index].rot());
                ++constraintItr;
            }
        }
    }

    if (!_relativePoses.empty()) {

        // build a list of targets from _targetVarVec
        std::vector<IKTarget> targets;
        {
            PROFILE_RANGE_EX(simulation_animation, "ik/computeTargets", 0xffff00ff, 0);
            computeTargets(animVars, targets, underPoses);
        }

        if (targets.empty()) {
            _relativePoses = underPoses;
        } else {

            {
                PROFILE_RANGE_EX(simulation_animation, "ik/shiftHips", 0xffff00ff, 0);

                if (_hipsTargetIndex >= 0 && _hipsTargetIndex < (int)targets.size()) {
                    // slam the hips to match the _hipsTarget
                    AnimPose absPose = targets[_hipsTargetIndex].getPose();
                    int parentIndex = _skeleton->getParentIndex(targets[_hipsTargetIndex].getIndex());
                    if (parentIndex != -1) {
                        _relativePoses[_hipsIndex] = _skeleton->getAbsolutePose(parentIndex, _relativePoses).inverse() * absPose;
                    } else {
                        _relativePoses[_hipsIndex] = absPose;
                    }
                } else {
                    // if there is no hips target, shift hips according to the _hipsOffset from the previous frame
                    float offsetLength = glm::length(_hipsOffset);
                    const float MIN_HIPS_OFFSET_LENGTH = 0.03f;
                    if (offsetLength > MIN_HIPS_OFFSET_LENGTH && _hipsIndex >= 0) {
                        float scaleFactor = ((offsetLength - MIN_HIPS_OFFSET_LENGTH) / offsetLength);
                        glm::vec3 hipsOffset = scaleFactor * _hipsOffset;
                        if (_hipsParentIndex == -1) {
                            _relativePoses[_hipsIndex].trans() = _relativePoses[_hipsIndex].trans() + hipsOffset;
                        } else {
                            auto absHipsPose = _skeleton->getAbsolutePose(_hipsIndex, _relativePoses);
                            absHipsPose.trans() += hipsOffset;
                            _relativePoses[_hipsIndex] = _skeleton->getAbsolutePose(_hipsParentIndex, _relativePoses).inverse() * absHipsPose;
                        }
                    }
                }

                // update all HipsRelative targets to account for the hips shift/ik target.
                auto shiftedHipsAbsPose = _skeleton->getAbsolutePose(_hipsIndex, _relativePoses);
                auto underHipsAbsPose = _skeleton->getAbsolutePose(_hipsIndex, underPoses);
                auto absHipsOffset = shiftedHipsAbsPose.trans() - underHipsAbsPose.trans();
                for (auto& target: targets) {
                    if (target.getType() == IKTarget::Type::HipsRelativeRotationAndPosition) {
                        auto pose = target.getPose();
                        pose.trans() = pose.trans() + absHipsOffset;
                        target.setPose(pose.rot(), pose.trans());
                    }
                }
            }

            {
                PROFILE_RANGE_EX(simulation_animation, "ik/debugDraw", 0xffff00ff, 0);

                // debug render ik targets
                if (context.getEnableDebugDrawIKTargets()) {
                    const vec4 WHITE(1.0f);
                    glm::mat4 rigToAvatarMat = createMatFromQuatAndPos(Quaternions::Y_180, glm::vec3());

                    for (auto& target : targets) {
                        glm::mat4 geomTargetMat = createMatFromQuatAndPos(target.getRotation(), target.getTranslation());
                        glm::mat4 avatarTargetMat = rigToAvatarMat * context.getGeometryToRigMatrix() * geomTargetMat;

                        QString name = QString("ikTarget%1").arg(target.getIndex());
                        DebugDraw::getInstance().addMyAvatarMarker(name, glmExtractRotation(avatarTargetMat), extractTranslation(avatarTargetMat), WHITE);
                    }
                } else if (context.getEnableDebugDrawIKTargets() != _previousEnableDebugIKTargets) {
                    // remove markers if they were added last frame.
                    for (auto& target : targets) {
                        QString name = QString("ikTarget%1").arg(target.getIndex());
                        DebugDraw::getInstance().removeMyAvatarMarker(name);
                    }
                }

                _previousEnableDebugIKTargets = context.getEnableDebugDrawIKTargets();
            }

            {
                PROFILE_RANGE_EX(simulation_animation, "ik/ccd", 0xffff00ff, 0);
                preconditionRelativePosesToAvoidLimbLock(context, targets);
                solve(context, targets);
            }

            if (_hipsTargetIndex < 0) {
                PROFILE_RANGE_EX(simulation_animation, "ik/measureHipsOffset", 0xffff00ff, 0);
                computeHipsOffset(targets, underPoses, dt);
            } else {
                _hipsOffset = Vectors::ZERO;
            }
        }

        if (context.getEnableDebugDrawIKConstraints()) {
            debugDrawConstraints(context);
        }
    }

    if (_leftHandIndex > -1) {
        _uncontrolledLeftHandPose = _skeleton->getAbsolutePose(_leftHandIndex, underPoses);
    }
    if (_rightHandIndex > -1) {
        _uncontrolledRightHandPose = _skeleton->getAbsolutePose(_rightHandIndex, underPoses);
    }
    if (_hipsIndex > -1) {
        _uncontrolledHipsPose = _skeleton->getAbsolutePose(_hipsIndex, underPoses);
    }

    return _relativePoses;
}

void AnimInverseKinematics::computeHipsOffset(const std::vector<IKTarget>& targets, const AnimPoseVec& underPoses, float dt) {
    // measure new _hipsOffset for next frame
    // by looking for discrepancies between where a targeted endEffector is
    // and where it wants to be (after IK solutions are done)
    glm::vec3 newHipsOffset = Vectors::ZERO;
    for (auto& target: targets) {
        int targetIndex = target.getIndex();
        if (targetIndex == _headIndex && _headIndex != -1) {
            // special handling for headTarget
            if (target.getType() == IKTarget::Type::RotationOnly) {
                // we want to shift the hips to bring the underPose closer
                // to where the head happens to be (overpose)
                glm::vec3 under = _skeleton->getAbsolutePose(_headIndex, underPoses).trans();
                glm::vec3 actual = _skeleton->getAbsolutePose(_headIndex, _relativePoses).trans();
                const float HEAD_OFFSET_SLAVE_FACTOR = 0.65f;
                newHipsOffset += HEAD_OFFSET_SLAVE_FACTOR * (actual - under);
            } else if (target.getType() == IKTarget::Type::HmdHead) {
                // we want to shift the hips to bring the head to its designated position
                glm::vec3 actual = _skeleton->getAbsolutePose(_headIndex, _relativePoses).trans();
                _hipsOffset += target.getTranslation() - actual;
                // and ignore all other targets
                newHipsOffset = _hipsOffset;
                break;
            } else if (target.getType() == IKTarget::Type::RotationAndPosition) {
                glm::vec3 actualPosition = _skeleton->getAbsolutePose(targetIndex, _relativePoses).trans();
                glm::vec3 targetPosition = target.getTranslation();
                newHipsOffset += targetPosition - actualPosition;

                // Add downward pressure on the hips
                const float PRESSURE_SCALE_FACTOR = 0.95f;
                const float PRESSURE_TRANSLATION_OFFSET = 1.0f;
                newHipsOffset *= PRESSURE_SCALE_FACTOR;
                newHipsOffset -= PRESSURE_TRANSLATION_OFFSET;
            }
        } else if (target.getType() == IKTarget::Type::RotationAndPosition) {
            glm::vec3 actualPosition = _skeleton->getAbsolutePose(targetIndex, _relativePoses).trans();
            glm::vec3 targetPosition = target.getTranslation();
            newHipsOffset += targetPosition - actualPosition;
        }
    }

    // smooth transitions by relaxing _hipsOffset toward the new value
    const float HIPS_OFFSET_SLAVE_TIMESCALE = 0.10f;
    float tau = dt < HIPS_OFFSET_SLAVE_TIMESCALE ?  dt / HIPS_OFFSET_SLAVE_TIMESCALE : 1.0f;
    _hipsOffset += (newHipsOffset - _hipsOffset) * tau;

    // clamp the hips offset
    float hipsOffsetLength = glm::length(_hipsOffset);
    if (hipsOffsetLength > _maxHipsOffsetLength) {
        _hipsOffset *= _maxHipsOffsetLength / hipsOffsetLength;
    }
}

void AnimInverseKinematics::setMaxHipsOffsetLength(float maxLength) {
    // manually adjust scale here
    const float METERS_TO_CENTIMETERS = 100.0f;
    _maxHipsOffsetLength = METERS_TO_CENTIMETERS * maxLength;
}

void AnimInverseKinematics::clearIKJointLimitHistory() {
    for (auto& pair : _constraints) {
        pair.second->clearHistory();
    }
}

RotationConstraint* AnimInverseKinematics::getConstraint(int index) const {
    RotationConstraint* constraint = nullptr;
    std::map<int, RotationConstraint*>::const_iterator constraintItr = _constraints.find(index);
    if (constraintItr != _constraints.end()) {
        constraint = constraintItr->second;
    }
    return constraint;
}

void AnimInverseKinematics::clearConstraints() {
    std::map<int, RotationConstraint*>::iterator constraintItr = _constraints.begin();
    while (constraintItr != _constraints.end()) {
        delete constraintItr->second;
        ++constraintItr;
    }
    _constraints.clear();
}

// set up swing limits around a swingTwistConstraint in an ellipse, where lateralSwingPhi is the swing limit for lateral swings (side to side)
// anteriorSwingPhi is swing limit for forward and backward swings.  (where x-axis of reference rotation is sideways and -z-axis is forward)
static void setEllipticalSwingLimits(SwingTwistConstraint* stConstraint, float lateralSwingPhi, float anteriorSwingPhi) {
    assert(stConstraint);
    const int NUM_SUBDIVISIONS = 16;
    std::vector<float> minDots;
    minDots.reserve(NUM_SUBDIVISIONS);
    float dTheta = TWO_PI / NUM_SUBDIVISIONS;
    float theta = 0.0f;
    for (int i = 0; i < NUM_SUBDIVISIONS; i++) {
        float theta_prime = atanf((anteriorSwingPhi / lateralSwingPhi) * tanf(theta));
        float phi = (cosf(2.0f * theta_prime) * ((anteriorSwingPhi - lateralSwingPhi) / 2.0f)) + ((anteriorSwingPhi + lateralSwingPhi) / 2.0f);
        minDots.push_back(cosf(phi));
        theta += dTheta;
    }
    stConstraint->setSwingLimits(minDots);
}

void AnimInverseKinematics::initConstraints() {
    if (!_skeleton) {
    }
    // We create constraints for the joints shown here
    // (and their Left counterparts if applicable).
    //
    //
    //                                    O RightHand
    //                      Head         /
    //                          O       /
    //                      Neck|      O RightForeArm
    //                          O     /
    //                        O | O  / RightShoulder
    //      O-------O-------O' \|/ 'O
    //                   Spine2 O  RightArm
    //                          |
    //                          |
    //                   Spine1 O
    //                          |
    //                          |
    //                    Spine O
    //         y                |
    //         |                |
    //         |            O---O---O RightUpLeg
    //      z  |            | Hips  |
    //       \ |            |       |
    //        \|            |       |
    //  x -----+            O       O RightLeg
    //                      |       |
    //                      |       |
    //                      |       |
    //                      O       O RightFoot
    //                     /       /
    //                 O--O    O--O

    loadDefaultPoses(_skeleton->getRelativeBindPoses());

    int numJoints = (int)_defaultRelativePoses.size();

    /* KEEP THIS CODE for future experimentation
    // compute corresponding absolute poses
    AnimPoseVec absolutePoses;
    absolutePoses.resize(numJoints);
    for (int i = 0; i < numJoints; ++i) {
        int parentIndex = _skeleton->getParentIndex(i);
        if (parentIndex < 0) {
            absolutePoses[i] = _defaultRelativePoses[i];
        } else {
            absolutePoses[i] = absolutePoses[parentIndex] * _defaultRelativePoses[i];
        }
    }
    */

    clearConstraints();
    for (int i = 0; i < numJoints; ++i) {
        // compute the joint's baseName and remember whether its prefix was "Left" or not
        QString baseName = _skeleton->getJointName(i);
        bool isLeft = baseName.startsWith("Left", Qt::CaseSensitive);
        float mirror = isLeft ? -1.0f : 1.0f;
        if (isLeft) {
            baseName.remove(0, 4);
        } else if (baseName.startsWith("Right", Qt::CaseSensitive)) {
            baseName.remove(0, 5);
        }

        RotationConstraint* constraint = nullptr;
        if (0 == baseName.compare("Arm", Qt::CaseSensitive)) {
            SwingTwistConstraint* stConstraint = new SwingTwistConstraint();
            stConstraint->setReferenceRotation(_defaultRelativePoses[i].rot());
            //stConstraint->setTwistLimits(-PI / 2.0f, PI / 2.0f);
            const float TWIST_LIMIT = 5.0f * PI / 8.0f;
            stConstraint->setTwistLimits(-TWIST_LIMIT, TWIST_LIMIT);

            /* KEEP THIS CODE for future experimentation
            // these directions are approximate swing limits in root-frame
            // NOTE: they don't need to be normalized
            std::vector<glm::vec3> swungDirections;
            swungDirections.push_back(glm::vec3(mirror * 1.0f, 1.0f, 1.0f));
            swungDirections.push_back(glm::vec3(mirror * 1.0f, 0.0f, 1.0f));
            swungDirections.push_back(glm::vec3(mirror * 1.0f, -1.0f, 0.5f));
            swungDirections.push_back(glm::vec3(mirror * 0.0f, -1.0f, 0.0f));
            swungDirections.push_back(glm::vec3(mirror * 0.0f, -1.0f, -1.0f));
            swungDirections.push_back(glm::vec3(mirror * -0.5f, 0.0f, -1.0f));
            swungDirections.push_back(glm::vec3(mirror * 0.0f, 1.0f, -1.0f));
            swungDirections.push_back(glm::vec3(mirror * 0.0f, 1.0f, 0.0f));

            // rotate directions into joint-frame
            glm::quat invAbsoluteRotation = glm::inverse(absolutePoses[i].rot);
            int numDirections = (int)swungDirections.size();
            for (int j = 0; j < numDirections; ++j) {
                swungDirections[j] = invAbsoluteRotation * swungDirections[j];
            }
            stConstraint->setSwingLimits(swungDirections);
            */

            // simple cone
            std::vector<float> minDots;
            const float MAX_HAND_SWING = 5.0f * PI / 8.0f;
            minDots.push_back(cosf(MAX_HAND_SWING));
            stConstraint->setSwingLimits(minDots);

            constraint = static_cast<RotationConstraint*>(stConstraint);
        } else if (0 == baseName.compare("UpLeg", Qt::CaseSensitive)) {
            SwingTwistConstraint* stConstraint = new SwingTwistConstraint();
            stConstraint->setReferenceRotation(_defaultRelativePoses[i].rot());
            stConstraint->setTwistLimits(-PI / 2.0f, PI / 2.0f);

            std::vector<glm::vec3> swungDirections;
            float deltaTheta = PI / 4.0f;
            float theta = 0.0f;
            swungDirections.push_back(glm::vec3(mirror * cosf(theta), 1.0f, sinf(theta))); // posterior
            theta += deltaTheta;
            swungDirections.push_back(glm::vec3(mirror * cosf(theta), 0.5f, sinf(theta)));
            theta += deltaTheta;
            swungDirections.push_back(glm::vec3(mirror * cosf(theta), 0.25f, sinf(theta)));
            theta += deltaTheta;
            swungDirections.push_back(glm::vec3(mirror * cosf(theta), -1.5f, sinf(theta)));
            theta += deltaTheta;
            swungDirections.push_back(glm::vec3(mirror * cosf(theta), -3.0f, sinf(theta))); // anterior
            theta += deltaTheta;
            swungDirections.push_back(glm::vec3(mirror * cosf(theta), -1.5f, sinf(theta)));
            theta += deltaTheta;
            swungDirections.push_back(glm::vec3(mirror * cosf(theta), 0.25f, sinf(theta)));
            theta += deltaTheta;
            swungDirections.push_back(glm::vec3(mirror * cosf(theta), 0.5f, sinf(theta)));

            std::vector<float> minDots;
            for (size_t i = 0; i < swungDirections.size(); i++) {
                minDots.push_back(glm::dot(glm::normalize(swungDirections[i]), Vectors::UNIT_Y));
            }
            stConstraint->setSwingLimits(minDots);

            /*
            // simple cone
            std::vector<float> minDots;
            const float MAX_HAND_SWING = 2.9f; // 170 deg //2 * PI / 3.0f;
            minDots.push_back(cosf(MAX_HAND_SWING));
            stConstraint->setSwingLimits(minDots);
            */

            constraint = static_cast<RotationConstraint*>(stConstraint);
        } else if (0 == baseName.compare("Hand", Qt::CaseSensitive)) {
            SwingTwistConstraint* stConstraint = new SwingTwistConstraint();
            stConstraint->setReferenceRotation(_defaultRelativePoses[i].rot());
            stConstraint->setTwistLimits(0.0f, 0.0f); // max == min, disables twist limits

            /* KEEP THIS CODE for future experimentation -- twist limits for hands
            const float MAX_HAND_TWIST = 3.0f * PI / 5.0f;
            const float MIN_HAND_TWIST = -PI / 2.0f;
            if (isLeft) {
                stConstraint->setTwistLimits(-MAX_HAND_TWIST, -MIN_HAND_TWIST);
            } else {
                stConstraint->setTwistLimits(MIN_HAND_TWIST, MAX_HAND_TWIST);
            }
            */

            /* KEEP THIS CODE for future experimentation -- non-symmetrical swing limits for wrist
             * a more complicated wrist with asymmetric cone
            // these directions are approximate swing limits in parent-frame
            // NOTE: they don't need to be normalized
            std::vector<glm::vec3> swungDirections;
            swungDirections.push_back(glm::vec3(1.0f, 1.0f, 0.0f));
            swungDirections.push_back(glm::vec3(0.75f, 1.0f, -1.0f));
            swungDirections.push_back(glm::vec3(-0.75f, 1.0f, -1.0f));
            swungDirections.push_back(glm::vec3(-1.0f, 1.0f, 0.0f));
            swungDirections.push_back(glm::vec3(-0.75f, 1.0f, 1.0f));
            swungDirections.push_back(glm::vec3(0.75f, 1.0f, 1.0f));

            // rotate directions into joint-frame
            glm::quat invRelativeRotation = glm::inverse(_defaultRelativePoses[i].rot);
            int numDirections = (int)swungDirections.size();
            for (int j = 0; j < numDirections; ++j) {
                swungDirections[j] = invRelativeRotation * swungDirections[j];
            }
            stConstraint->setSwingLimits(swungDirections);
            */

            // simple cone
            std::vector<float> minDots;
            const float MAX_HAND_SWING = PI / 2.0f;
            minDots.push_back(cosf(MAX_HAND_SWING));
            stConstraint->setSwingLimits(minDots);

            constraint = static_cast<RotationConstraint*>(stConstraint);
        } else if (baseName.startsWith("Shoulder", Qt::CaseSensitive)) {
            SwingTwistConstraint* stConstraint = new SwingTwistConstraint();
            stConstraint->setReferenceRotation(_defaultRelativePoses[i].rot());
            const float MAX_SHOULDER_TWIST = PI / 10.0f;
            stConstraint->setTwistLimits(-MAX_SHOULDER_TWIST, MAX_SHOULDER_TWIST);

            std::vector<float> minDots;
            const float MAX_SHOULDER_SWING = PI / 12.0f;
            minDots.push_back(cosf(MAX_SHOULDER_SWING));
            stConstraint->setSwingLimits(minDots);

            constraint = static_cast<RotationConstraint*>(stConstraint);
        } else if (baseName.startsWith("Spine", Qt::CaseSensitive)) {
            SwingTwistConstraint* stConstraint = new SwingTwistConstraint();
            stConstraint->setReferenceRotation(_defaultRelativePoses[i].rot());
            const float MAX_SPINE_TWIST = PI / 20.0f;
            stConstraint->setTwistLimits(-MAX_SPINE_TWIST, MAX_SPINE_TWIST);

            // limit lateral swings more then forward-backward swings
            const float MAX_SPINE_LATERAL_SWING = PI / 15.0f;
            const float MAX_SPINE_ANTERIOR_SWING = PI / 10.0f;
            setEllipticalSwingLimits(stConstraint, MAX_SPINE_LATERAL_SWING, MAX_SPINE_ANTERIOR_SWING);

            if (0 == baseName.compare("Spine1", Qt::CaseSensitive)
                    || 0 == baseName.compare("Spine", Qt::CaseSensitive)) {
                stConstraint->setLowerSpine(true);
            }

            constraint = static_cast<RotationConstraint*>(stConstraint);

        } else if (0 == baseName.compare("Neck", Qt::CaseSensitive)) {
            SwingTwistConstraint* stConstraint = new SwingTwistConstraint();
            stConstraint->setReferenceRotation(_defaultRelativePoses[i].rot());
            const float MAX_NECK_TWIST = PI / 8.0f;
            stConstraint->setTwistLimits(-MAX_NECK_TWIST, MAX_NECK_TWIST);

            // limit lateral swings more then forward-backward swings
            const float MAX_NECK_LATERAL_SWING = PI / 12.0f;
            const float MAX_NECK_ANTERIOR_SWING = PI / 10.0f;
            setEllipticalSwingLimits(stConstraint, MAX_NECK_LATERAL_SWING, MAX_NECK_ANTERIOR_SWING);

            constraint = static_cast<RotationConstraint*>(stConstraint);
        } else if (0 == baseName.compare("Head", Qt::CaseSensitive)) {
            SwingTwistConstraint* stConstraint = new SwingTwistConstraint();
            stConstraint->setReferenceRotation(_defaultRelativePoses[i].rot());
            const float MAX_HEAD_TWIST = PI / 6.0f;
            stConstraint->setTwistLimits(-MAX_HEAD_TWIST, MAX_HEAD_TWIST);

            // limit lateral swings more then forward-backward swings
            const float MAX_NECK_LATERAL_SWING = PI / 4.0f;
            const float MAX_NECK_ANTERIOR_SWING = PI / 3.0f;
            setEllipticalSwingLimits(stConstraint, MAX_NECK_LATERAL_SWING, MAX_NECK_ANTERIOR_SWING);

            constraint = static_cast<RotationConstraint*>(stConstraint);
        } else if (0 == baseName.compare("ForeArm", Qt::CaseSensitive)) {
            // The elbow joint rotates about the parent-frame's zAxis (-zAxis) for the Right (Left) arm.
            ElbowConstraint* eConstraint = new ElbowConstraint();
            glm::quat referenceRotation = _defaultRelativePoses[i].rot();
            eConstraint->setReferenceRotation(referenceRotation);

            // we determine the max/min angles by rotating the swing limit lines from parent- to child-frame
            // then measure the angles to swing the yAxis into alignment
            glm::vec3 hingeAxis = - mirror * Vectors::UNIT_Z;
            const float MIN_ELBOW_ANGLE = 0.0f;
            const float MAX_ELBOW_ANGLE = 11.0f * PI / 12.0f;
            glm::quat invReferenceRotation = glm::inverse(referenceRotation);
            glm::vec3 minSwingAxis = invReferenceRotation * glm::angleAxis(MIN_ELBOW_ANGLE, hingeAxis) * Vectors::UNIT_Y;
            glm::vec3 maxSwingAxis = invReferenceRotation * glm::angleAxis(MAX_ELBOW_ANGLE, hingeAxis) * Vectors::UNIT_Y;

            // for the rest of the math we rotate hingeAxis into the child frame
            hingeAxis = referenceRotation * hingeAxis;
            eConstraint->setHingeAxis(hingeAxis);

            glm::vec3 projectedYAxis = glm::normalize(Vectors::UNIT_Y - glm::dot(Vectors::UNIT_Y, hingeAxis) * hingeAxis);
            float minAngle = acosf(glm::dot(projectedYAxis, minSwingAxis));
            if (glm::dot(hingeAxis, glm::cross(projectedYAxis, minSwingAxis)) < 0.0f) {
                minAngle = - minAngle;
            }
            float maxAngle = acosf(glm::dot(projectedYAxis, maxSwingAxis));
            if (glm::dot(hingeAxis, glm::cross(projectedYAxis, maxSwingAxis)) < 0.0f) {
                maxAngle = - maxAngle;
            }
            eConstraint->setAngleLimits(minAngle, maxAngle);

            constraint = static_cast<RotationConstraint*>(eConstraint);
        } else if (0 == baseName.compare("Leg", Qt::CaseSensitive)) {
            // The knee joint rotates about the parent-frame's -xAxis.
            ElbowConstraint* eConstraint = new ElbowConstraint();
            glm::quat referenceRotation = _defaultRelativePoses[i].rot();
            eConstraint->setReferenceRotation(referenceRotation);
            glm::vec3 hingeAxis = -1.0f * Vectors::UNIT_X;

            // we determine the max/min angles by rotating the swing limit lines from parent- to child-frame
            // then measure the angles to swing the yAxis into alignment
            const float MIN_KNEE_ANGLE = 0.0f;
            const float MAX_KNEE_ANGLE = 7.0f * PI / 8.0f; // 157.5 deg
            glm::quat invReferenceRotation = glm::inverse(referenceRotation);
            glm::vec3 minSwingAxis = invReferenceRotation * glm::angleAxis(MIN_KNEE_ANGLE, hingeAxis) * Vectors::UNIT_Y;
            glm::vec3 maxSwingAxis = invReferenceRotation * glm::angleAxis(MAX_KNEE_ANGLE, hingeAxis) * Vectors::UNIT_Y;

            // for the rest of the math we rotate hingeAxis into the child frame
            hingeAxis = referenceRotation * hingeAxis;
            eConstraint->setHingeAxis(hingeAxis);

            glm::vec3 projectedYAxis = glm::normalize(Vectors::UNIT_Y - glm::dot(Vectors::UNIT_Y, hingeAxis) * hingeAxis);
            float minAngle = acosf(glm::dot(projectedYAxis, minSwingAxis));
            if (glm::dot(hingeAxis, glm::cross(projectedYAxis, minSwingAxis)) < 0.0f) {
                minAngle = - minAngle;
            }
            float maxAngle = acosf(glm::dot(projectedYAxis, maxSwingAxis));
            if (glm::dot(hingeAxis, glm::cross(projectedYAxis, maxSwingAxis)) < 0.0f) {
                maxAngle = - maxAngle;
            }
            eConstraint->setAngleLimits(minAngle, maxAngle);

            constraint = static_cast<RotationConstraint*>(eConstraint);
        } else if (0 == baseName.compare("Foot", Qt::CaseSensitive)) {
            SwingTwistConstraint* stConstraint = new SwingTwistConstraint();
            stConstraint->setReferenceRotation(_defaultRelativePoses[i].rot());
            stConstraint->setTwistLimits(-PI / 4.0f, PI / 4.0f);

            // these directions are approximate swing limits in parent-frame
            // NOTE: they don't need to be normalized
            std::vector<glm::vec3> swungDirections;
            swungDirections.push_back(Vectors::UNIT_Y);
            swungDirections.push_back(Vectors::UNIT_X);
            swungDirections.push_back(glm::vec3(1.0f, 1.0f, 1.0f));
            swungDirections.push_back(glm::vec3(1.0f, 1.0f, -1.0f));

            // rotate directions into joint-frame
            glm::quat invRelativeRotation = glm::inverse(_defaultRelativePoses[i].rot());
            int numDirections = (int)swungDirections.size();
            for (int j = 0; j < numDirections; ++j) {
                swungDirections[j] = invRelativeRotation * swungDirections[j];
            }
            stConstraint->setSwingLimits(swungDirections);

            constraint = static_cast<RotationConstraint*>(stConstraint);
        }
        if (constraint) {
            _constraints[i] = constraint;
        }
    }
}

void AnimInverseKinematics::initLimitCenterPoses() {
    assert(_skeleton);
    _limitCenterPoses.reserve(_skeleton->getNumJoints());
    for (int i = 0; i < _skeleton->getNumJoints(); i++) {
        AnimPose pose = _skeleton->getRelativeDefaultPose(i);
        RotationConstraint* constraint = getConstraint(i);
        if (constraint) {
            pose.rot() = constraint->computeCenterRotation();
        }
        _limitCenterPoses.push_back(pose);
    }

    // The limit center rotations for the LeftArm and RightArm form a t-pose.
    // In order for the elbows to look more natural, we rotate them down by the avatar's sides
    const float UPPER_ARM_THETA = PI / 3.0f;  // 60 deg
    int leftArmIndex = _skeleton->nameToJointIndex("LeftArm");
    const glm::quat armRot = glm::angleAxis(UPPER_ARM_THETA, Vectors::UNIT_X);
    if (leftArmIndex >= 0 && leftArmIndex < (int)_limitCenterPoses.size()) {
        _limitCenterPoses[leftArmIndex].rot() = _limitCenterPoses[leftArmIndex].rot() * armRot;
    }
    int rightArmIndex = _skeleton->nameToJointIndex("RightArm");
    if (rightArmIndex >= 0 && rightArmIndex < (int)_limitCenterPoses.size()) {
        _limitCenterPoses[rightArmIndex].rot() = _limitCenterPoses[rightArmIndex].rot() * armRot;
    }
}

void AnimInverseKinematics::setSkeletonInternal(AnimSkeleton::ConstPointer skeleton) {
    AnimNode::setSkeletonInternal(skeleton);

    // invalidate all targetVars
    for (auto& targetVar: _targetVarVec) {
        targetVar.jointIndex = -1;
    }

    _maxTargetIndex = -1;

    for (auto& accumulator: _rotationAccumulators) {
        accumulator.clearAndClean();
    }
    for (auto& accumulator: _translationAccumulators) {
        accumulator.clearAndClean();
    }

    if (skeleton) {
        initConstraints();
        initLimitCenterPoses();
        _headIndex = _skeleton->nameToJointIndex("Head");
        _hipsIndex = _skeleton->nameToJointIndex("Hips");

        // also cache the _hipsParentIndex for later
        if (_hipsIndex >= 0) {
            _hipsParentIndex = _skeleton->getParentIndex(_hipsIndex);
        } else {
            _hipsParentIndex = -1;
        }

        _leftHandIndex = _skeleton->nameToJointIndex("LeftHand");
        _rightHandIndex = _skeleton->nameToJointIndex("RightHand");
    } else {
        clearConstraints();
        _headIndex = -1;
        _hipsIndex = -1;
        _hipsParentIndex = -1;
        _leftHandIndex = -1;
        _rightHandIndex = -1;
    }

    _uncontrolledLeftHandPose = AnimPose();
    _uncontrolledRightHandPose = AnimPose();
    _uncontrolledHipsPose = AnimPose();
}

static glm::vec3 sphericalToCartesian(float phi, float theta) {
    float cos_phi = cosf(phi);
    float sin_phi = sinf(phi);
    return glm::vec3(sin_phi * cosf(theta), cos_phi, sin_phi * sinf(theta));
}

void AnimInverseKinematics::debugDrawRelativePoses(const AnimContext& context) const {
    AnimPoseVec poses = _relativePoses;

    // convert relative poses to absolute
    _skeleton->convertRelativePosesToAbsolute(poses);


    mat4 geomToWorldMatrix = context.getRigToWorldMatrix() * context.getGeometryToRigMatrix();

    const vec4 RED(1.0f, 0.0f, 0.0f, 1.0f);
    const vec4 GREEN(0.0f, 1.0f, 0.0f, 1.0f);
    const vec4 BLUE(0.0f, 0.0f, 1.0f, 1.0f);
    const vec4 GRAY(0.2f, 0.2f, 0.2f, 1.0f);
    const float AXIS_LENGTH = 2.0f; // cm

    // draw each pose
    for (int i = 0; i < (int)poses.size(); i++) {

        // transform local axes into world space.
        auto pose = poses[i];
        glm::vec3 xAxis = transformVectorFast(geomToWorldMatrix, pose.rot() * Vectors::UNIT_X);
        glm::vec3 yAxis = transformVectorFast(geomToWorldMatrix, pose.rot() * Vectors::UNIT_Y);
        glm::vec3 zAxis = transformVectorFast(geomToWorldMatrix, pose.rot() * Vectors::UNIT_Z);
        glm::vec3 pos = transformPoint(geomToWorldMatrix, pose.trans());
        DebugDraw::getInstance().drawRay(pos, pos + AXIS_LENGTH * xAxis, RED);
        DebugDraw::getInstance().drawRay(pos, pos + AXIS_LENGTH * yAxis, GREEN);
        DebugDraw::getInstance().drawRay(pos, pos + AXIS_LENGTH * zAxis, BLUE);

        // draw line to parent
        int parentIndex = _skeleton->getParentIndex(i);
        if (parentIndex != -1) {
            glm::vec3 parentPos = transformPoint(geomToWorldMatrix, poses[parentIndex].trans());
            DebugDraw::getInstance().drawRay(pos, parentPos, GRAY);
        }
    }
}

void AnimInverseKinematics::debugDrawIKChain(JointChainInfo* jointChainInfos, size_t numJointChainInfos, const AnimContext& context) const {
    AnimPoseVec poses = _relativePoses;

    // copy debug joint rotations into the relative poses
    for (size_t i = 0; i < numJointChainInfos; i++) {
        const JointChainInfo& info = jointChainInfos[i];
        poses[info.jointIndex].rot() = info.relRot;
        poses[info.jointIndex].trans() = info.relTrans;
    }

    // convert relative poses to absolute
    _skeleton->convertRelativePosesToAbsolute(poses);

    mat4 geomToWorldMatrix = context.getRigToWorldMatrix() * context.getGeometryToRigMatrix();

    const vec4 RED(1.0f, 0.0f, 0.0f, 1.0f);
    const vec4 GREEN(0.0f, 1.0f, 0.0f, 1.0f);
    const vec4 BLUE(0.0f, 0.0f, 1.0f, 1.0f);
    const vec4 GRAY(0.2f, 0.2f, 0.2f, 1.0f);
    const float AXIS_LENGTH = 2.0f; // cm

    // draw each pose
    for (int i = 0; i < (int)poses.size(); i++) {
        int parentIndex = _skeleton->getParentIndex(i);
        JointChainInfo* jointInfo = nullptr;
        JointChainInfo* parentJointInfo = nullptr;
        lookupJointChainInfo(jointChainInfos, numJointChainInfos, i, parentIndex, &jointInfo, &parentJointInfo);
        if (jointInfo && parentJointInfo) {

            // transform local axes into world space.
            auto pose = poses[i];
            glm::vec3 xAxis = transformVectorFast(geomToWorldMatrix, pose.rot() * Vectors::UNIT_X);
            glm::vec3 yAxis = transformVectorFast(geomToWorldMatrix, pose.rot() * Vectors::UNIT_Y);
            glm::vec3 zAxis = transformVectorFast(geomToWorldMatrix, pose.rot() * Vectors::UNIT_Z);
            glm::vec3 pos = transformPoint(geomToWorldMatrix, pose.trans());
            DebugDraw::getInstance().drawRay(pos, pos + AXIS_LENGTH * xAxis, RED);
            DebugDraw::getInstance().drawRay(pos, pos + AXIS_LENGTH * yAxis, GREEN);
            DebugDraw::getInstance().drawRay(pos, pos + AXIS_LENGTH * zAxis, BLUE);

            // draw line to parent
            if (parentIndex != -1) {
                glm::vec3 parentPos = transformPoint(geomToWorldMatrix, poses[parentIndex].trans());
                glm::vec4 color = GRAY;

                // draw constrained joints with a RED link to their parent.
                if (parentJointInfo->constrained) {
                    color = RED;
                }
                DebugDraw::getInstance().drawRay(pos, parentPos, color);
            }
        }
    }
}

void AnimInverseKinematics::debugDrawConstraints(const AnimContext& context) const {
    if (_skeleton) {
        const vec4 RED(1.0f, 0.0f, 0.0f, 1.0f);
        const vec4 GREEN(0.0f, 1.0f, 0.0f, 1.0f);
        const vec4 BLUE(0.0f, 0.0f, 1.0f, 1.0f);
        const vec4 PURPLE(0.5f, 0.0f, 1.0f, 1.0f);
        const vec4 CYAN(0.0f, 1.0f, 1.0f, 1.0f);
        const vec4 GRAY(0.2f, 0.2f, 0.2f, 1.0f);
        const vec4 MAGENTA(1.0f, 0.0f, 1.0f, 1.0f);
        const float AXIS_LENGTH = 5.0f; // cm
        const float TWIST_LENGTH = 4.0f; // cm
        const float HINGE_LENGTH = 4.0f; // cm
        const float SWING_LENGTH = 4.0f; // cm

        AnimPoseVec poses = _relativePoses;

        // convert relative poses to absolute
        _skeleton->convertRelativePosesToAbsolute(poses);

        mat4 geomToWorldMatrix = context.getRigToWorldMatrix() * context.getGeometryToRigMatrix();

        // draw each pose and constraint
        for (int i = 0; i < (int)poses.size(); i++) {
            // transform local axes into world space.
            auto pose = poses[i];
            glm::vec3 xAxis = transformVectorFast(geomToWorldMatrix, pose.rot() * Vectors::UNIT_X);
            glm::vec3 yAxis = transformVectorFast(geomToWorldMatrix, pose.rot() * Vectors::UNIT_Y);
            glm::vec3 zAxis = transformVectorFast(geomToWorldMatrix, pose.rot() * Vectors::UNIT_Z);
            glm::vec3 pos = transformPoint(geomToWorldMatrix, pose.trans());
            DebugDraw::getInstance().drawRay(pos, pos + AXIS_LENGTH * xAxis, RED);
            DebugDraw::getInstance().drawRay(pos, pos + AXIS_LENGTH * yAxis, GREEN);
            DebugDraw::getInstance().drawRay(pos, pos + AXIS_LENGTH * zAxis, BLUE);

            // draw line to parent
            int parentIndex = _skeleton->getParentIndex(i);
            if (parentIndex != -1) {
                glm::vec3 parentPos = transformPoint(geomToWorldMatrix, poses[parentIndex].trans());
                DebugDraw::getInstance().drawRay(pos, parentPos, GRAY);
            }

            glm::quat parentAbsRot;
            if (parentIndex != -1) {
                parentAbsRot = poses[parentIndex].rot();
            }

            const RotationConstraint* constraint = getConstraint(i);
            if (constraint) {
                glm::quat refRot = constraint->getReferenceRotation();
                const ElbowConstraint* elbowConstraint = dynamic_cast<const ElbowConstraint*>(constraint);
                if (elbowConstraint) {
                    glm::vec3 hingeAxis = transformVectorFast(geomToWorldMatrix, parentAbsRot * refRot * elbowConstraint->getHingeAxis());
                    DebugDraw::getInstance().drawRay(pos, pos + HINGE_LENGTH * hingeAxis, MAGENTA);

                    // draw elbow constraints
                    glm::quat minRot = glm::angleAxis(elbowConstraint->getMinAngle(), elbowConstraint->getHingeAxis());
                    glm::quat maxRot = glm::angleAxis(elbowConstraint->getMaxAngle(), elbowConstraint->getHingeAxis());

                    const int NUM_SWING_STEPS = 10;
                    for (int i = 0; i < NUM_SWING_STEPS + 1; i++) {
                        glm::quat rot = glm::normalize(glm::lerp(minRot, maxRot, i * (1.0f / NUM_SWING_STEPS)));
                        glm::vec3 axis = transformVectorFast(geomToWorldMatrix, parentAbsRot * rot * refRot * Vectors::UNIT_Y);
                        DebugDraw::getInstance().drawRay(pos, pos + TWIST_LENGTH * axis, CYAN);
                    }

                } else {
                    const SwingTwistConstraint* swingTwistConstraint = dynamic_cast<const SwingTwistConstraint*>(constraint);
                    if (swingTwistConstraint) {
                        // twist constraints

                        glm::vec3 hingeAxis = transformVectorFast(geomToWorldMatrix, parentAbsRot * refRot * Vectors::UNIT_Y);
                        DebugDraw::getInstance().drawRay(pos, pos + HINGE_LENGTH * hingeAxis, MAGENTA);

                        glm::quat minRot = glm::angleAxis(swingTwistConstraint->getMinTwist(), refRot * Vectors::UNIT_Y);
                        glm::quat maxRot = glm::angleAxis(swingTwistConstraint->getMaxTwist(), refRot * Vectors::UNIT_Y);

                        const int NUM_SWING_STEPS = 10;
                        for (int i = 0; i < NUM_SWING_STEPS + 1; i++) {
                            glm::quat rot = glm::normalize(glm::lerp(minRot, maxRot, i * (1.0f / NUM_SWING_STEPS)));
                            glm::vec3 axis = transformVectorFast(geomToWorldMatrix, parentAbsRot * rot * refRot * Vectors::UNIT_X);
                            DebugDraw::getInstance().drawRay(pos, pos + TWIST_LENGTH * axis, CYAN);
                        }

                        // draw swing constraints.
                        const size_t NUM_MIN_DOTS = swingTwistConstraint->getMinDots().size();
                        const float D_THETA = TWO_PI / (NUM_MIN_DOTS - 1);
                        const float PI_2 = PI / 2.0f;
                        float theta = 0.0f;
                        for (size_t i = 0, j = NUM_MIN_DOTS - 2; i < NUM_MIN_DOTS - 1; j = i, i++, theta += D_THETA) {
                            // compute swing rotation from theta and phi angles.
                            float phi = acosf(swingTwistConstraint->getMinDots()[i]);
                            glm::vec3 swungAxis = sphericalToCartesian(phi, theta - PI_2);
                            glm::vec3 worldSwungAxis = transformVectorFast(geomToWorldMatrix, parentAbsRot * refRot * swungAxis);
                            glm::vec3 swingTip = pos + SWING_LENGTH * worldSwungAxis;

                            float prevPhi = acosf(swingTwistConstraint->getMinDots()[j]);
                            float prevTheta = theta - D_THETA;
                            glm::vec3 prevSwungAxis = sphericalToCartesian(prevPhi, prevTheta - PI_2);
                            glm::vec3 prevWorldSwungAxis = transformVectorFast(geomToWorldMatrix, parentAbsRot * refRot * prevSwungAxis);
                            glm::vec3 prevSwingTip = pos + SWING_LENGTH * prevWorldSwungAxis;

                            DebugDraw::getInstance().drawRay(pos, swingTip, PURPLE);
                            DebugDraw::getInstance().drawRay(prevSwingTip, swingTip, PURPLE);
                        }
                    }
                }
            }
        }
    }
}

// for bones under IK, blend between previous solution (_relativePoses) to targetPoses
// for bones NOT under IK, copy directly from underPoses.
// mutates _relativePoses.
void AnimInverseKinematics::blendToPoses(const AnimPoseVec& targetPoses, const AnimPoseVec& underPoses, float blendFactor) {
    // relax toward poses
    int numJoints = (int)_relativePoses.size();
    for (int i = 0; i < numJoints; ++i) {
        float dotSign = copysignf(1.0f, glm::dot(_relativePoses[i].rot(), targetPoses[i].rot()));
        if (_rotationAccumulators[i].isDirty()) {
            // this joint is affected by IK --> blend toward the targetPoses rotation
            _relativePoses[i].rot() = glm::normalize(glm::lerp(_relativePoses[i].rot(), dotSign * targetPoses[i].rot(), blendFactor));
        } else {
            // this joint is NOT affected by IK --> slam to underPoses rotation
            _relativePoses[i].rot() = underPoses[i].rot();
        }
        _relativePoses[i].trans() = underPoses[i].trans();
    }
}

void AnimInverseKinematics::preconditionRelativePosesToAvoidLimbLock(const AnimContext& context, const std::vector<IKTarget>& targets) {
    const int NUM_LIMBS = 4;
    std::pair<int, int> limbs[NUM_LIMBS] = {
        {_skeleton->nameToJointIndex("LeftHand"), _skeleton->nameToJointIndex("LeftArm")},
        {_skeleton->nameToJointIndex("RightHand"), _skeleton->nameToJointIndex("RightArm")},
        {_skeleton->nameToJointIndex("LeftFoot"), _skeleton->nameToJointIndex("LeftUpLeg")},
        {_skeleton->nameToJointIndex("RightFoot"), _skeleton->nameToJointIndex("RightUpLeg")}
    };
    const float MIN_AXIS_LENGTH = 1.0e-4f;

    for (auto& target : targets) {
        if (target.getIndex() != -1) {
            for (int i = 0; i < NUM_LIMBS; i++) {
                if (limbs[i].first == target.getIndex()) {
                    int tipIndex = limbs[i].first;
                    int baseIndex = limbs[i].second;

                    // TODO: as an optimization, these poses can be computed in one pass down the chain, instead of three.
                    AnimPose tipPose = _skeleton->getAbsolutePose(tipIndex, _relativePoses);
                    AnimPose basePose = _skeleton->getAbsolutePose(baseIndex, _relativePoses);
                    AnimPose baseParentPose = _skeleton->getAbsolutePose(_skeleton->getParentIndex(baseIndex), _relativePoses);

                    // to help reduce limb locking, and to help the CCD solver converge faster
                    // rotate the limbs leverArm over the targetLine.
                    glm::vec3 targetLine = target.getTranslation() - basePose.trans();
                    glm::vec3 leverArm = tipPose.trans() - basePose.trans();
                    glm::vec3 axis = glm::cross(leverArm, targetLine);
                    float axisLength = glm::length(axis);
                    if (axisLength > MIN_AXIS_LENGTH) {
                        // compute angle of rotation that brings tip to target
                        axis /= axisLength;
                        float cosAngle = glm::clamp(glm::dot(leverArm, targetLine) / (glm::length(leverArm) * glm::length(targetLine)), -1.0f, 1.0f);
                        float angle = acosf(cosAngle);
                        glm::quat newBaseRotation = glm::angleAxis(angle, axis) * basePose.rot();

                        // convert base rotation into relative space of base.
                        _relativePoses[baseIndex].rot() = glm::inverse(baseParentPose.rot()) * newBaseRotation;
                    }
                }
            }
        }
    }
}

void AnimInverseKinematics::initRelativePosesFromSolutionSource(SolutionSource solutionSource, const AnimPoseVec& underPoses) {
    const float RELAX_BLEND_FACTOR = (1.0f / 16.0f);
    const float COPY_BLEND_FACTOR = 1.0f;
    switch (solutionSource) {
    default:
    case SolutionSource::RelaxToUnderPoses:
        blendToPoses(underPoses, underPoses, RELAX_BLEND_FACTOR);
        break;
    case SolutionSource::RelaxToLimitCenterPoses:
        blendToPoses(_limitCenterPoses, underPoses, RELAX_BLEND_FACTOR);
        // special case for hips: copy over hips pose whether or not IK is enabled.
        if (_hipsIndex >= 0 && _hipsIndex < (int)_relativePoses.size()) {
            _relativePoses[_hipsIndex] = _limitCenterPoses[_hipsIndex];
        }
        break;
    case SolutionSource::PreviousSolution:
        // do nothing... _relativePoses is already the previous solution
        break;
    case SolutionSource::UnderPoses:
        _relativePoses = underPoses;
        break;
    case SolutionSource::LimitCenterPoses:
        // essentially copy limitCenterPoses over to _relativePoses.
        blendToPoses(underPoses, _limitCenterPoses, COPY_BLEND_FACTOR);
        break;
    }
}

void AnimInverseKinematics::debugDrawSpineSplines(const AnimContext& context, const std::vector<IKTarget>& targets) const {

    for (auto& target : targets) {

        if (target.getType() != IKTarget::Type::Spline) {
            continue;
        }

        const int baseIndex = _hipsIndex;

        // build spline
        AnimPose tipPose = AnimPose(glm::vec3(1.0f), target.getRotation(), target.getTranslation());
        AnimPose basePose = _skeleton->getAbsolutePose(baseIndex, _relativePoses);

        CubicHermiteSplineFunctorWithArcLength spline;
        if (target.getIndex() == _headIndex) {
            // set gain factors so that more curvature occurs near the tip of the spline.
            const float HIPS_GAIN = 0.5f;
            const float HEAD_GAIN = 1.0f;
            spline = computeSplineFromTipAndBase(tipPose, basePose, HIPS_GAIN, HEAD_GAIN);
        } else {
            spline = computeSplineFromTipAndBase(tipPose, basePose);
        }
        float totalArcLength = spline.arcLength(1.0f);

        const glm::vec4 RED(1.0f, 0.0f, 0.0f, 1.0f);
        const glm::vec4 WHITE(1.0f, 1.0f, 1.0f, 1.0f);

        // draw red and white stripped spline, parameterized by arc length.
        // i.e. each stripe should be the same length.
        AnimPose geomToWorldPose = AnimPose(context.getRigToWorldMatrix() * context.getGeometryToRigMatrix());
        const int NUM_SEGMENTS = 20;
        const float dArcLength = totalArcLength / NUM_SEGMENTS;
        float arcLength = 0.0f;
        for (int i = 0; i < NUM_SEGMENTS; i++) {
            float prevT = spline.arcLengthInverse(arcLength);
            float nextT = spline.arcLengthInverse(arcLength + dArcLength);
            DebugDraw::getInstance().drawRay(geomToWorldPose.xformPoint(spline(prevT)), geomToWorldPose.xformPoint(spline(nextT)), (i % 2) == 0 ? RED : WHITE);
            arcLength += dArcLength;
        }
    }
}
