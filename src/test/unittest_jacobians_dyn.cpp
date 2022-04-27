#include <gtest/gtest.h>
//#include <unsupported/Eigen/MatrixFunctions>

#define private public

#include "estimator.h"

#include "unittest_helpers.h"



//using namespace Eigen;
using namespace xivo;

/* Class to check dxc(now)_d[everything else]. We wil */
class DynamicsJacobiansTest : public ::testing::Test {
  protected:
    void SetUp() override {

        // Create Estimator with tumvi benchmark parameters
        auto cfg = LoadJson("cfg/tumvi.json");
        est = CreateSystem(LoadJson(cfg["estimator_cfg"].asString()));
        delta = 1e-6;
        tol = 1e-5;

    }

    void SetRandomState () {
    }

    void SetIdentityState() {
        est->X_.Rsb = SO3();
        est->X_.Tsb.setZero();
        est->X_.Vsb.setZero();
        est->X_.ba.setZero();
        est->X_.bg.setZero();
        est->X_.Rbc = SO3();
        est->X_.Tbc.setZero();
        est->X_.Rg = SO3();

#ifdef USE_ONLINE_TEMPORAL_CALIB
        est->X_.td = 0;
#endif

#ifdef USE_ONLINE_IMU_CALIB
        est->imu_.X_.Ca.setIdentity();
        est->imu_.X_.Cg.setIdentity();
#endif

#ifdef USE_ONLINE_CAMERA_CALIB
        // We will leave camera calibration as is... (do nothing)
#endif

        // no imu input
        imu_input.setZero();

        // default gravity
        est->g_ = {0, 0, -9.8};
    }

    void PerturbElement(int j, number_t delta, State &X, IMUState &imu) {
#ifdef USE_ONLINE_IMU_CALIB
        State::Tangent X_tangent;
        IMUState::Tangent IMU_tangent;
        if (j >= Index::Cg) {
            IMU_tangent(j - Index::Cg) += delta;
        } else {
            X_tangent(j) += delta;
        }
        X += X_tangent;
        imu += IMU_tangent;
#else
        State::Tangent X_tangent;
        X_tangent(j) += delta;
        X += X_tangent;
#endif
    }

    void NonlinearDynamicsFcn(State::Tangent &xdot,
                              IMUState::Tangent &imuderiv) {
        Vec3 gyro_input = imu_input.head<3>();
        Vec3 accel_input = imu_input.tail<3>();

        Vec3 gyro_calib = est->Cg() * gyro_input - est->bg();
        Vec3 accel_calib = est->Ca() * accel_input - est->ba();

        SE3 gsb = est->gsb();
        Mat3 Rsb = gsb.R().matrix();

        xdot.segment<3>(Index::Wsb) = Rsb * gyro_calib;
        xdot.segment<3>(Index::Tsb) = est->Vsb();
        xdot.segment<3>(Index::Vsb) = Rsb * accel_calib + est->Rg() * est->g_;
        xdot.segment<3>(Index::ba).setZero();
        xdot.segment<3>(Index::bg).setZero();
        xdot.segment<3>(Index::Wbc).setZero();
        xdot.segment<3>(Index::Tbc).setZero();
        xdot.segment<2>(Index::Wg).setZero();

#ifdef USE_ONLINE_TEMPORAL_CALIB
        xdot(Index::td) = 0.0;
#endif
    }

    void RunTests(std::string errmsg_start) {
        // Compute Analytical Jacobian
        est->ComputeMotionJacobianAt(est->X_, imu_input);

        // Compute nonlinear xdot at the original state
        State::Tangent x_deriv0;
        IMUState::Tangent imu_deriv0;
        NonlinearDynamicsFcn(x_deriv0, imu_deriv0);

        // Variables to hold perturbed derivatives and backup state
        State::Tangent x_deriv1;
        IMUState::Tangent imu_deriv1;
        State X_backup = est->X_;
        IMUState imu_backup = est->imu_.X_;

        // Compute numerical Jacobians in F_ one at a time
        // We are numerically approximating the derivative of element i with
        // respect to element j
        // Note that since kMotionSize includes td, Cg, Ca, we will only end up
        // testing those derivatives if they are part of the state
        for (int i=0; i<kMotionSize; i++) {
            for (int j=0; j<kMotionSize; j++) {

                // make perturbation in element j of the total state vector
                PerturbElement(j, delta, est->X_, est->imu_.X_);

                // compute the nonlinear dynamics
                NonlinearDynamicsFcn(x_deriv1, imu_deriv1);

                // Compute numerical jacobian of state i dynamics w.r.t. state j
                number_t num_jac = (x_deriv1(i) - x_deriv0(i)) / delta;
                EXPECT_NEAR(num_jac, est->F_.coeff(i,j), tol) <<
                    errmsg_start <<
                    "State jacobian error at state " << i << ", state " << j;

                // Put state back where it was
                est->X_ = X_backup;
                est->imu_.X_ = imu_backup;
            }
        }

        // Compute numerical Jacobians in G_ one at a time
        Vec6 imu_input_backup = imu_input;

        for (int i=0; i<kMotionSize; i++) {
            for (int j=0; j<6; j++) {
                imu_input(j) += delta;

                NonlinearDynamicsFcn(x_deriv1, imu_deriv1);
                number_t num_jac = (x_deriv1(i) - x_deriv0(i)) / delta;
                EXPECT_NEAR(num_jac, est->G_.coeff(i,j), tol) <<
                    errmsg_start <<
                    "Input jacobian error at state " << i << ", input " << j;

                imu_input = imu_input_backup;
            }
        }
    }

    // Estimator Object
    EstimatorPtr est;
    Vec6 imu_input;
    
    number_t tol;   // numerical tolerance for checks
    number_t delta; // finite difference
};


TEST_F(DynamicsJacobiansTest, Zero) {
    SetIdentityState();
    RunTests("Identity State Jacobians: ");
}

