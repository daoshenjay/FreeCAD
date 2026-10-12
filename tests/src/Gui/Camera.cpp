// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>

#include <Inventor/SbBox3f.h>
#include <Inventor/SbBox3d.h>
#include <Inventor/SbDPLine.h>
#include <Inventor/SbDPMatrix.h>
#include <Inventor/SbDPPlane.h>
#include <Inventor/SbLine.h>
#include <Inventor/SbMatrix.h>
#include <Inventor/SbPlane.h>
#include <Inventor/SbRotation.h>
#include <Inventor/SbSphere.h>
#include <Inventor/SbVec2f.h>
#include <Inventor/SbVec3d.h>
#include <Inventor/SbViewVolume.h>
#include <Inventor/SoDB.h>
#include <Inventor/actions/SoCallbackAction.h>
#include <Inventor/elements/SoViewingMatrixElement.h>
#include <Inventor/nodes/SoOrthographicCamera.h>
#include <Inventor/nodes/SoPerspectiveCamera.h>
#include <Base/Converter.h>
#include <Base/Tools.h>
#include <Gui/Camera.h>
#include <Gui/Utilities.h>
#include <Gui/ViewProvider.h>

#include <src/App/InitApplication.h>


/*
 This comment was previously used to get the hard coded axonometric view quaternions
 in the Camera class.
 This has since been replaced with unit tests that verify the correctness of the
 quaternion calculations.

 The old code is kept for reference and to show how the quaternions were calculated.

 ---

 Formulas to get quaternion for axonometric views:

 \code
from math import sqrt, degrees, asin, atan
p1=App.Rotation(App.Vector(1,0,0),90)
p2=App.Rotation(App.Vector(0,0,1),alpha)
p3=App.Rotation(p2.multVec(App.Vector(1,0,0)),beta)
p4=p3.multiply(p2).multiply(p1)

from pivy import coin
c=Gui.ActiveDocument.ActiveView.getCameraNode()
c.orientation.setValue(*p4.Q)
 \endcode

 The angles alpha and beta depend on the type of axonometry
 Isometric:
 \code
alpha=45
beta=degrees(asin(-sqrt(1.0/3.0)))
 \endcode

 Dimetric:
 \code
alpha=degrees(asin(sqrt(1.0/8.0)))
beta=degrees(-asin(1.0/3.0))
 \endcode

 Trimetric:
 \code
alpha=30.0
beta=-35.0
 \endcode

 Verification code that the axonomtries are correct:

 \code
from pivy import coin
c=Gui.ActiveDocument.ActiveView.getCameraNode()
vo=App.Vector(c.getViewVolume().getMatrix().multVecMatrix(coin.SbVec3f(0,0,0)).getValue())
vx=App.Vector(c.getViewVolume().getMatrix().multVecMatrix(coin.SbVec3f(10,0,0)).getValue())
vy=App.Vector(c.getViewVolume().getMatrix().multVecMatrix(coin.SbVec3f(0,10,0)).getValue())
vz=App.Vector(c.getViewVolume().getMatrix().multVecMatrix(coin.SbVec3f(0,0,10)).getValue())
(vx-vo).Length
(vy-vo).Length
(vz-vo).Length

# Projection
vo.z=0
vx.z=0
vy.z=0
vz.z=0

(vx-vo).Length
(vy-vo).Length
(vz-vo).Length
 \endcode

 See also:
 http://www.mathematik.uni-marburg.de/~thormae/lectures/graphics1/graphics_6_2_ger_web.html#1
 http://www.mathematik.uni-marburg.de/~thormae/lectures/graphics1/code_v2/Axonometric/qt/Axonometric.cpp
 https://de.wikipedia.org/wiki/Arkussinus_und_Arkuskosinus
*/

using Base::convertTo;
using Base::Rotation;
using Base::toRadians;
using Base::Vector3d;

namespace
{

Rotation buildAxonometricRotation(double alphaRad, double betaRad)
{
    const auto p1 = Rotation(Vector3d::UnitX, toRadians<float>(90.0));
    const auto p2 = Rotation(Vector3d::UnitZ, alphaRad);
    const auto p3 = Rotation(p2.multVec(Vector3d::UnitX), betaRad);
    const auto p4 = p3 * p2 * p1;

    return p4;
}

// Returns a tuple of 2D lengths of X, Y, Z unit vectors after applying rotation
std::array<double, 3> getProjectedLengths(const SbRotation& rot)
{
    // Set up a simple view volume to test the projection of the unit vectors.
    // The actual values don't matter much, as we are only interested in the
    // relative lengths of the projected vectors.
    SbViewVolume volume;
    // left, right, bottom, top, near, far
    volume.ortho(-10, 10, -10, 10, -10, 10);

    volume.rotateCamera(rot);
    const auto matrix = volume.getMatrix();

    // Get the transformed unit vectors
    SbVec3f vo, vx, vy, vz;
    matrix.multVecMatrix(SbVec3f(0, 0, 0), vo);
    matrix.multVecMatrix(SbVec3f(10, 0, 0), vx);
    matrix.multVecMatrix(SbVec3f(0, 10, 0), vy);
    matrix.multVecMatrix(SbVec3f(0, 0, 10), vz);

    // Project to XY plane by setting Z to 0
    vo[2] = 0;
    vx[2] = 0;
    vy[2] = 0;
    vz[2] = 0;

    // Return the lengths of the projected vectors
    return {(vx - vo).length(), (vy - vo).length(), (vz - vo).length()};
}

SbVec3f furthestNormalizedCorner(const SoOrthographicCamera& camera, const SbBox3f& box, float aspect)
{
    SbBox3f normalized = box;
    normalized.transform(camera.getViewVolume(aspect).getMatrix());

    const SbVec3f low = normalized.getMin();
    const SbVec3f high = normalized.getMax();
    return {
        std::max(std::abs(low[0]), std::abs(high[0])),
        std::max(std::abs(low[1]), std::abs(high[1])),
        std::max(std::abs(low[2]), std::abs(high[2])),
    };
}

float largestNormalizedExtent(const SoOrthographicCamera& camera, const SbBox3f& box, float aspect)
{
    const SbVec3f furthest = furthestNormalizedCorner(camera, box, aspect);
    return std::max(furthest[0], furthest[1]);
}

float largestNormalizedDepth(const SoOrthographicCamera& camera, const SbBox3f& box, float aspect)
{
    return furthestNormalizedCorner(camera, box, aspect)[2];
}

/// Expect the fit to decline @p box, leaving a known starting pose untouched.
void expectFitDeclines(SoOrthographicCamera& camera, const SbBox3f& box)
{
    camera.orientation.setValue(Gui::Camera::rotation(Gui::Camera::Isometric));
    camera.position.setValue(7.0F, 8.0F, 9.0F);
    camera.height.setValue(42.0F);

    Gui::Camera::fitToBox(camera, SbBox3d(box), 1.0F);

    EXPECT_FLOAT_EQ(camera.height.getValue(), 42.0F);

    const SbVec3f position(camera.position.getValue());
    EXPECT_FLOAT_EQ(position[0], 7.0F);
    EXPECT_FLOAT_EQ(position[1], 8.0F);
    EXPECT_FLOAT_EQ(position[2], 9.0F);
}

}  // namespace

class CameraPrecalculatedQuaternions: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
    }
};

TEST_F(CameraPrecalculatedQuaternions, testIsometric)
{
    // Use the formula to get the isometric rotation
    double alpha = toRadians(45.0f);
    double beta = std::asin(-std::sqrt(1.0 / 3.0));

    const Rotation actual = buildAxonometricRotation(alpha, beta);
    const Rotation expected = convertTo<Rotation>(Gui::Camera::isometric());

    EXPECT_TRUE(actual.isSame(expected, 1e-6));
}

TEST_F(CameraPrecalculatedQuaternions, testDimetric)
{
    // Use the formula to get the dimetric rotation
    double alpha = std::asin(std::sqrt(1.0 / 8.0));
    double beta = -std::asin(1.0 / 3.0);

    const Rotation actual = buildAxonometricRotation(alpha, beta);
    const Rotation expected = convertTo<Rotation>(Gui::Camera::dimetric());

    EXPECT_TRUE(actual.isSame(expected, 1e-6));
}

TEST_F(CameraPrecalculatedQuaternions, testTrimetric)
{
    // Use the formula to get the trimetric rotation
    double alpha = toRadians(30.0);
    double beta = toRadians(-35.0);

    const Rotation actual = buildAxonometricRotation(alpha, beta);
    const Rotation expected = convertTo<Rotation>(Gui::Camera::trimetric());

    EXPECT_TRUE(actual.isSame(expected, 1e-6));
}


class CameraRotation: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
    }
};

TEST_F(CameraRotation, testIsometricProjection)
{
    auto rot = Gui::Camera::isometric();
    auto lengths = getProjectedLengths(rot);

    // In isometric, expect all lengths to be roughly equal
    EXPECT_NEAR(lengths[0], lengths[1], 1e-6);  // X == Y
    EXPECT_NEAR(lengths[0], lengths[2], 1e-6);  // X == Z
    EXPECT_NEAR(lengths[1], lengths[2], 1e-6);  // Y == Z
}

TEST_F(CameraRotation, testDimetricProjection)
{
    const auto rot = Gui::Camera::dimetric();
    const auto lengths = getProjectedLengths(rot);

    // In dimetric, expect two lengths to be roughly equal, one different
    const std::initializer_list<std::pair<double, double>> pairs = {
        {lengths[0], lengths[1]},
        {lengths[1], lengths[2]},
        {lengths[0], lengths[2]},
    };

    constexpr double tolerance = 1e-6;
    const auto isSimilar = [&](std::pair<double, double> lengths) -> bool {
        return std::abs(lengths.first - lengths.second) < tolerance;
    };

    unsigned similarCount = std::ranges::count_if(pairs, isSimilar);

    EXPECT_EQ(similarCount, 1);  // Exactly two are equal
}

TEST_F(CameraRotation, testTrimetricProjection)
{
    auto rot = Gui::Camera::trimetric();
    auto lengths = getProjectedLengths(rot);

    // In trimetric, all should differ significantly
    EXPECT_GT(std::abs(lengths[0] - lengths[1]), 1e-3);
    EXPECT_GT(std::abs(lengths[1] - lengths[2]), 1e-3);
    EXPECT_GT(std::abs(lengths[0] - lengths[2]), 1e-3);
}


class CameraFitToBox: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
        SoDB::init();  // idempotent; constructing a Coin node needs the type database
    }

    void SetUp() override
    {
        camera.reset(new SoOrthographicCamera);
    }

    Gui::CoinPtr<SoOrthographicCamera> camera;
};

TEST_F(CameraFitToBox, aCubeIsFramedWithTheMargin)
{
    // The default orientation looks down -Z, so camera space is world space.
    const SbBox3f box(-1.0F, -1.0F, -1.0F, 1.0F, 1.0F, 1.0F);

    Gui::Camera::fitToBox(*camera, SbBox3d(box), 1.0F);

    EXPECT_FLOAT_EQ(camera->height.getValue(), 2.0F * Gui::Camera::fitMargin);

    const SbVec3f position(camera->position.getValue());
    EXPECT_FLOAT_EQ(position[0], 0.0F);
    EXPECT_FLOAT_EQ(position[1], 0.0F);
    EXPECT_FLOAT_EQ(position[2], std::sqrt(3.0F));
}

TEST_F(CameraFitToBox, noCornerIsClipped)
{
    // A different orientation and a non-square frame from theContentReachesTheMargin below.
    camera->orientation.setValue(Gui::Camera::rotation(Gui::Camera::Dimetric));
    const SbBox3f box(-30.0F, -8.0F, -12.0F, 70.0F, 22.0F, 5.0F);

    Gui::Camera::fitToBox(*camera, SbBox3d(box), 1.6F);

    EXPECT_LE(largestNormalizedExtent(*camera, box, 1.6F), 1.0F);
}

TEST_F(CameraFitToBox, theClippingPlanesClearTheScene)
{
    // The isometric direction runs along a cube's body diagonal, so a cubic box puts its nearest
    // and farthest corners hard against tangent clipping planes.
    camera->orientation.setValue(Gui::Camera::rotation(Gui::Camera::Isometric));
    const SbBox3f box(-1.0F, -1.0F, -1.0F, 1.0F, 1.0F, 1.0F);

    Gui::Camera::fitToBox(*camera, SbBox3d(box), 1.0F);

    EXPECT_LT(largestNormalizedDepth(*camera, box, 1.0F), 1.0F);
}

TEST_F(CameraFitToBox, theContentReachesTheMargin)
{
    camera->orientation.setValue(Gui::Camera::rotation(Gui::Camera::Isometric));
    const SbBox3f box(-30.0F, -8.0F, -12.0F, 70.0F, 22.0F, 5.0F);

    Gui::Camera::fitToBox(*camera, SbBox3d(box), 1.0F);

    // A circumscribing-sphere fit leaves this well short of the margin.
    EXPECT_NEAR(largestNormalizedExtent(*camera, box, 1.0F), 1.0F / Gui::Camera::fitMargin, 1e-4F);
}

TEST_F(CameraFitToBox, aTranslatedBoxFramesIdentically)
{
    camera->orientation.setValue(Gui::Camera::rotation(Gui::Camera::Isometric));
    const SbBox3f box(-1.0F, -2.0F, -3.0F, 4.0F, 5.0F, 6.0F);
    const SbVec3f offset(100.0F, -50.0F, 25.0F);
    const SbBox3f moved(box.getMin() + offset, box.getMax() + offset);

    Gui::Camera::fitToBox(*camera, SbBox3d(box), 1.0F);
    const float heightAtOrigin = camera->height.getValue();
    const SbVec3f positionAtOrigin(camera->position.getValue());

    Gui::Camera::fitToBox(*camera, SbBox3d(moved), 1.0F);
    const SbVec3f movedPosition(camera->position.getValue());

    EXPECT_FLOAT_EQ(camera->height.getValue(), heightAtOrigin);
    EXPECT_NEAR(movedPosition[0], positionAtOrigin[0] + offset[0], 1e-3F);
    EXPECT_NEAR(movedPosition[1], positionAtOrigin[1] + offset[1], 1e-3F);
    EXPECT_NEAR(movedPosition[2], positionAtOrigin[2] + offset[2], 1e-3F);
}

TEST_F(CameraFitToBox, anElongatedRodBeatsTheSphereFit)
{
    camera->orientation.setValue(Gui::Camera::rotation(Gui::Camera::Isometric));
    const SbBox3f rod(-0.5F, -0.5F, -50.0F, 0.5F, 0.5F, 50.0F);

    Gui::Camera::fitToBox(*camera, SbBox3d(rod), 1.0F);

    SbSphere circumscribed;
    circumscribed.circumscribe(rod);

    // Coin's viewBoundingBox would hand the rod a frame of 2 * radius and leave it swimming.
    EXPECT_LT(camera->height.getValue(), 2.0F * circumscribed.getRadius());
    EXPECT_NEAR(largestNormalizedExtent(*camera, rod, 1.0F), 1.0F / Gui::Camera::fitMargin, 1e-4F);
}

TEST_F(CameraFitToBox, aLargeCoordinateRodKeepsSubULPDetail)
{
    // An 8 mm rod at x = y = 1e9 mm: the rod is degenerate in X and Y, so the
    // circumscribing-sphere radius is dominated by the float32 rounding of the
    // absolute coordinates. The fit must keep the camera on the rod rather
    // than let the float32 rounding of the corner coordinates collapse the
    // camera position to the world origin.
    const SbBox3d rod(1000000000.0, 1000000000.0, -4.0,
                      1000000000.0, 1000000000.0, 4.0);
    camera->position.setValue(7.0F, 8.0F, 9.0F);

    Gui::Camera::fitToBox(*camera, rod, 1.0F);

    // The rod is degenerate in X and Y, so the fit declines and leaves the
    // camera alone: it must not collapse the camera to the world origin.
    const SbVec3d position(camera->position.getValue());
    EXPECT_FLOAT_EQ(position[0], 7.0F);
    EXPECT_FLOAT_EQ(position[1], 8.0F);
    EXPECT_FLOAT_EQ(position[2], 9.0F);
}

TEST_F(CameraFitToBox, aLargeCoordinatePointRodDeclinesTheDegenerateFit)
{
    // A point rod (X, Y and Z all degenerate in camera space after projection)
    // must leave the camera alone rather than collapse the view volume.
    const SbBox3d point(1000000000.0, 1000000000.0, 5.0,
                        1000000000.0, 1000000000.0, 5.0);
    camera->orientation.setValue(Gui::Camera::rotation(Gui::Camera::Isometric));
    camera->position.setValue(7.0F, 8.0F, 9.0F);
    camera->height.setValue(42.0F);

    Gui::Camera::fitToBox(*camera, point, 1.0F);

    EXPECT_FLOAT_EQ(camera->height.getValue(), 42.0F);
    const SbVec3d position(camera->position.getValue());
    EXPECT_FLOAT_EQ(position[0], 7.0F);
    EXPECT_FLOAT_EQ(position[1], 8.0F);
    EXPECT_FLOAT_EQ(position[2], 9.0F);
}

TEST_F(CameraFitToBox, theAspectRatioDecidesWhichExtentConstrains)
{
    // Half extents are 2 across and 1 up in camera space.
    const SbBox3f box(-2.0F, -1.0F, -1.0F, 2.0F, 1.0F, 1.0F);

    Gui::Camera::fitToBox(*camera, SbBox3d(box), 1.0F);
    EXPECT_FLOAT_EQ(camera->height.getValue(), 4.0F * Gui::Camera::fitMargin);

    Gui::Camera::fitToBox(*camera, SbBox3d(box), 2.0F);
    EXPECT_FLOAT_EQ(camera->height.getValue(), 2.0F * Gui::Camera::fitMargin);

    Gui::Camera::fitToBox(*camera, SbBox3d(box), 0.5F);
    EXPECT_FLOAT_EQ(camera->height.getValue(), 8.0F * Gui::Camera::fitMargin);
}

TEST_F(CameraFitToBox, anEmptyBoxLeavesTheCameraAlone)
{
    expectFitDeclines(*camera, SbBox3f());
}

TEST_F(CameraFitToBox, aPointBoxLeavesTheCameraAlone)
{
    // A single-point scene is a valid bounding box, not an empty one, so it reaches the fit.
    expectFitDeclines(*camera, SbBox3f(3.0F, 4.0F, 5.0F, 3.0F, 4.0F, 5.0F));
}


class CameraDoublePrecision: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
        SoDB::init();  // idempotent; constructing a Coin node needs the type database
    }

    void SetUp() override
    {
        camera.reset(new SoPerspectiveCamera);
    }

    Gui::CoinPtr<SoPerspectiveCamera> camera;
};

TEST_F(CameraDoublePrecision, thePositionKeepsSubULPDetailAtLargeCoordinates)
{
    // The float32 grid at 1e9 mm has a spacing of 64 mm; the camera position
    // must not be quantized to it.
    camera->position.setValue(1000000008.0, 1000000000.0, -0.005);

    const SbVec3d actual(camera->position.getValue());
    EXPECT_DOUBLE_EQ(actual[0], 1000000008.0);
    EXPECT_DOUBLE_EQ(actual[1], 1000000000.0);
    EXPECT_DOUBLE_EQ(actual[2], -0.005);
}

TEST_F(CameraDoublePrecision, theViewingMatrixCarriesThePositionInFull)
{
    camera->position.setValue(1000000008.0, 1000000000.0, 0.0);

    SoCallbackAction action;
    action.apply(&*camera);

    const SbDPMatrix & viewing = SoViewingMatrixElement::getDP(action.getState());
    // The default orientation looks down -Z, so the viewing matrix is a plain
    // inverse translation of the camera position, in row-vector convention.
    EXPECT_DOUBLE_EQ(viewing[3][0], -1000000008.0);
    EXPECT_DOUBLE_EQ(viewing[3][1], -1000000000.0);
    EXPECT_DOUBLE_EQ(viewing[3][2], 0.0);

    // The float32 copy that legacy consumers read has already lost the detail.
    const SbMatrix & lossy = SoViewingMatrixElement::get(action.getState());
    EXPECT_NE(static_cast<double>(lossy[3][0]), -1000000008.0);
}

TEST_F(CameraDoublePrecision, fittingALargeCoordinateBoxReachesTheBox)
{
    // An 8 mm segment at x = y = 1e9 mm. The float32 bounding box collapses
    // the lateral extents, so the fit frames the segment along Z.
    const SbBox3f box(1000000000.0F, 1000000000.0F, -4.0F,
                      1000000000.0F, 1000000000.0F, 4.0F);

    camera->viewBoundingBox(box, 1.0F, 1.0F);

    const SbVec3d position(camera->position.getValue());
    const double reach = 4.0 / std::tan(camera->heightAngle.getValue() / 2.0);

    EXPECT_DOUBLE_EQ(position[0], 1000000000.0);
    EXPECT_DOUBLE_EQ(position[1], 1000000000.0);
    EXPECT_NEAR(position[2], reach, 1e-6);

    // The view distance must be on the order of the object, not a fixed
    // guard distance such as the former 2 * float32 ULP (128 mm).
    EXPECT_LT(std::abs(position[2]), 100.0);
}

TEST_F(CameraDoublePrecision, fittingAFlatXYBoxAtLargeCoordinates)
{
    // The blind spot of fittingALargeCoordinateBoxReachesTheBox: that box is
    // degenerate along X/Y, so the fit could only ever prove the Z axis. A
    // 6.76 mm box lying in the XY plane at 1e9 mm would collapse to a point
    // with float32 box coordinates: the camera would end up at the box
    // center with near == far == focalDistance == 0 -- an invisible scene
    // and a zoom that cannot recover.
    const SbBox3d box(1000000000.0, 1000000000.0, -1.0,
                      1000000006.76, 1000000003.0, 1.0);

    camera->viewBoundingBox(box, 1.0, 1.0);

    const SbVec3d position(camera->position.getValue());
    const SbVec3d center(1000000003.38, 1000000001.5, 0.0);
    const SbVec3d halfdiag(6.76, 3.0, 2.0);
    const double radius = 0.5 * halfdiag.length();
    const double reach = radius / std::tan(camera->heightAngle.getValue() / 2.0);

    // The default orientation looks down -Z, so the camera sits exactly on
    // the box center line and one bounding sphere radius away in direction.
    EXPECT_DOUBLE_EQ(position[0], center[0]);
    EXPECT_DOUBLE_EQ(position[1], center[1]);
    EXPECT_NEAR(position[2], reach, 1e-6);

    // The fit must stay on the order of the geometry, not of the coordinate.
    EXPECT_LT(reach, 100.0);

    // The view volume must actually exist.
    EXPECT_GT(camera->nearDistance.getValue(), 0.0F);
    EXPECT_GT(camera->farDistance.getValue(), camera->nearDistance.getValue());
    EXPECT_GT(camera->focalDistance.getValue(), 0.0F);
}

TEST_F(CameraDoublePrecision, aDegeneratePointBoxKeepsTheViewVolumeAlive)
{
    // A point box must not collapse the view volume to
    // near == far == focalDistance == 0.
    const SbBox3d box(1000000000.0, 1000000000.0, 5.0,
                      1000000000.0, 1000000000.0, 5.0);

    camera->viewBoundingBox(box, 1.0, 1.0);

    const SbVec3d position(camera->position.getValue());
    EXPECT_DOUBLE_EQ(position[0], 1000000000.0);
    EXPECT_DOUBLE_EQ(position[1], 1000000000.0);

    // The scale relative radius floor (1e-9 * |coordinate| = 1 mm at 1e9)
    // puts the camera a few millimeters off the point.
    EXPECT_GT(position[2], 5.0);
    EXPECT_LT(position[2] - 5.0, 10.0);

    EXPECT_GT(camera->nearDistance.getValue(), 0.0F);
    EXPECT_GT(camera->farDistance.getValue(), camera->nearDistance.getValue());
    EXPECT_GT(camera->focalDistance.getValue(), 0.0F);
}


// Reproduces the NavigationStyle::panCamera unprojection math: a one-pixel
// drag step must move the camera by ~one pixel of view height, not by a
// float32 ULP of the absolute coordinates (64 mm at 1e9 mm).
class CameraPanAtLargeCoordinates: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
        SoDB::init();
    }

    void SetUp() override
    {
        camera.reset(new SoOrthographicCamera);
        // Fit-like state at (1e9, 1e9, 0): view height 6.76 mm, 849 px tall.
        camera->position.setValue(1000000000.0, 1000000000.0, 10.0);
        camera->height = 6.762075901031494F;
        camera->focalDistance = 10.0F;
        camera->nearDistance = 1.0F;
        camera->farDistance = 100.0F;
        aspect = 1524.0F / 849.0F;
        camera->aspectRatio = aspect;
    }

    // One horizontal pixel in normalized [0,1] viewport coordinates.
    static constexpr float pixelStepU = 1.0F / 1524.0F;

    Gui::CoinPtr<SoOrthographicCamera> camera;
    float aspect {};
};

TEST_F(CameraPanAtLargeCoordinates, onePixelDragsByOnePixelOfViewHeight)
{
    SbViewVolume vv = camera->getViewVolume(aspect);
    const SbDPPlane panplane = vv.getPlaneDP(camera->focalDistance.getValue());

    // The panning plane sits at the focal distance in front of the eye at
    // (1e9, 1e9, 0): z == 0.
    const SbVec3d planeNormalBefore(panplane.getNormal());
    EXPECT_NEAR(planeNormalBefore[2], 1.0, 1e-12);

    const SbVec2f prev(0.5F, 0.5F);
    const SbVec2f curr(0.5F + pixelStepU, 0.5F);

    SbDPLine line;
    SbVec3d currentPlanept;
    vv.projectPointToLine(curr, line);
    ASSERT_TRUE(panplane.intersect(line, currentPlanept));

    SbVec3d oldPlanept;
    vv.projectPointToLine(prev, line);
    ASSERT_TRUE(panplane.intersect(line, oldPlanept));

    const SbVec3d delta(currentPlanept - oldPlanept);
    const double expected = camera->height.getValue() / 849.0;

    // 1e-5 mm covers the float32 viewport position ((0.5 + step) - 0.5);
    // the world-coordinate math itself is double and stays sub-ULP.
    EXPECT_NEAR(delta[0], expected, 1e-5);
    EXPECT_NEAR(delta[1], 0.0, 1e-5);
    EXPECT_NEAR(delta[2], 0.0, 1e-5);

    // The step must be sub-ULP of the absolute coordinate, not a jump.
    EXPECT_LT(std::abs(delta[0]), 64.0);
}

TEST_F(CameraPanAtLargeCoordinates, theF32UnprojectionPathQuantizesToSixtyFourMillimeters)
{
    // Documents the bug this suite guards: the legacy float32 path snaps the
    // one-pixel drag to the 64 mm float32 grid at 1e9 mm.
    SbViewVolume vv = camera->getViewVolume(aspect);
    const SbPlane panplane = vv.getPlane(camera->focalDistance.getValue());

    const SbVec2f prev(0.5F, 0.5F);
    const SbVec2f curr(0.5F + pixelStepU, 0.5F);

    SbLine line;
    SbVec3f currentPlanept;
    vv.projectPointToLine(curr, line);
    panplane.intersect(line, currentPlanept);

    SbVec3f oldPlanept;
    vv.projectPointToLine(prev, line);
    panplane.intersect(line, oldPlanept);

    const SbVec3f delta(currentPlanept - oldPlanept);

    // One 8 µm step cannot survive the 64 mm float32 grid at 1e9 mm: both
    // unprojections snap to the same grid point and the drag is lost (or,
    // crossing a grid boundary, it jumps a full ULP away). Either way the
    // sub-pixel truth -- height / 849 px -- is unrepresentable.
    const double expected = camera->height.getValue() / 849.0;
    EXPECT_TRUE(std::abs(delta[0]) < 1e-3 || std::abs(delta[0]) > 32.0);
    EXPECT_NE(static_cast<double>(delta[0]), expected);
}
