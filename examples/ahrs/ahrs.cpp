#include <Eigen/Core>
#include <Eigen/Geometry>
#include <cmath>
#include "UKF/Types.h"
#include "UKF/Integrator.h"
#include "UKF/StateVector.h"
#include "UKF/MeasurementVector.h"
#include "UKF/Core.h"
#include "ahrs.h"

/*
This is an implementation of an Unscented Kalman filter for a 9-axis AHRS,
using accelerometer, gyroscope and magnetometer to estimate attitude, angular
velocity and linear acceleration.
*/

/* Value of g. */
#define G_ACCEL (9.80665)

/*
Approximate magnitude of Earth's magnetic field in microTesla. This is only
used to initialise the scale factor matrix.
*/
#define EARTH_MAG (45.0)

enum AHRS_Keys {
    /* AHRS filter fields. */
    Attitude,
    AngularVelocity,
    Acceleration,

    /* Parameter estimation filter fields. */
    AccelerometerBias,
    AccelerometerScaleFactor,
    GyroscopeBias,
    GyroscopeScaleFactor,
    MagnetometerBias,
    MagnetometerScaleFactor,

    /* AHRS measurement vector fields. */
    Accelerometer,
    Gyroscope,
    Magnetometer
};

/*
The AHRS state vector contains the following:
- Attitude as a quaternion (NED frame to body frame)
- Angular velocity (body frame, rad/s)
- Acceleration (body frame, m/s^2)
*/
using AHRS_StateVector = UKF::StateVector<
    UKF::Field<Attitude, UKF::Quaternion>,
    UKF::Field<AngularVelocity, UKF::Vector<3>>,
    UKF::Field<Acceleration, UKF::Vector<3>>
>;

static AHRS_StateVector::CovarianceMatrix process_noise;

/*
In addition to the AHRS filter, an online parameter estimation filter is also
implemented in order to calculate scale factor errors and biases in each of
the sensors.
The magnetometer scale factor is represented as a direction cosine matrix
with no normalisation constraint.
*/

using AHRS_SensorErrorVector = UKF::StateVector<
    UKF::Field<AccelerometerBias, UKF::Vector<3>>,
    UKF::Field<AccelerometerScaleFactor, UKF::Vector<3>>,
    UKF::Field<GyroscopeBias, UKF::Vector<3>>,
    UKF::Field<GyroscopeScaleFactor, UKF::Vector<3>>,
    UKF::Field<MagnetometerBias, UKF::Vector<3>>,
    UKF::Field<MagnetometerScaleFactor, UKF::Vector<9>>
>;

static AHRS_SensorErrorVector::CovarianceMatrix error_process_noise;

/* AHRS process model. */
template <> template <>
AHRS_StateVector AHRS_StateVector::derivative<>() const {
    AHRS_StateVector output;

    /* Assume constant linear acceleration. */
    output.set_field<Acceleration>(UKF::Vector<3>(0, 0, 0));

    /* Calculate change in attitude. */
    UKF::Quaternion omega_q;
    omega_q.vec() = get_field<AngularVelocity>() * 0.5;
    omega_q.w() = 0;
    output.set_field<Attitude>(omega_q.conjugate() * get_field<Attitude>());

    /* Assume constant angular velocity. */
    output.set_field<AngularVelocity>(UKF::Vector<3>(0, 0, 0));

    return output;
}

/* AHRS process noise covariance. Scale based on the sample time delta. */
template <>
AHRS_StateVector::CovarianceMatrix AHRS_StateVector::process_noise_covariance(real_t dt) {
    return process_noise * dt;
}

using AHRS_MeasurementVector = UKF::DynamicMeasurementVector<
    UKF::Field<Accelerometer, UKF::Vector<3>>,
    UKF::Field<Gyroscope, UKF::Vector<3>>,
    UKF::Field<Magnetometer, UKF::Vector<3>>
>;

/*
AHRS measurement model.
TODO: Work out why we need to put the versions with no inputs arguments in.
For some reason, if it's not there, we get a strange compilation eroor which
seems to be the Detail::FieldTypes helper returning 'void' for everything.
*/
template <> template <>
UKF::Vector<3> AHRS_MeasurementVector::expected_measurement
<AHRS_StateVector, Accelerometer>(const AHRS_StateVector& state) {
    return state.get_field<Acceleration>() + state.get_field<Attitude>() * UKF::Vector<3>(0, 0, -G_ACCEL);
}

template <> template <>
UKF::Vector<3> AHRS_MeasurementVector::expected_measurement
<AHRS_StateVector, Gyroscope>(const AHRS_StateVector& state) {
    return state.get_field<AngularVelocity>();
}

template <> template <>
UKF::Vector<3> AHRS_MeasurementVector::expected_measurement
<AHRS_StateVector, Magnetometer>(const AHRS_StateVector& state) {
    return state.get_field<Attitude>() * UKF::Vector<3>(1.0, 0.0, 0.0);
}

/*
This is the measurement model that's actually used in the filter, because
it's the one which takes the parameter estimation filter state as an input.
*/
template <> template <>
UKF::Vector<3> AHRS_MeasurementVector::expected_measurement
<AHRS_StateVector, Accelerometer, AHRS_SensorErrorVector>(
        const AHRS_StateVector& state, const AHRS_SensorErrorVector& input) {
    return input.get_field<AccelerometerBias>().array() + input.get_field<AccelerometerScaleFactor>().array() *
        (state.get_field<Acceleration>() + state.get_field<Attitude>() * UKF::Vector<3>(0, 0, -G_ACCEL)).array();
}

template <> template <>
UKF::Vector<3> AHRS_MeasurementVector::expected_measurement
<AHRS_StateVector, Gyroscope, AHRS_SensorErrorVector>(
        const AHRS_StateVector& state, const AHRS_SensorErrorVector& input) {
    return input.get_field<GyroscopeBias>().array() +
        input.get_field<GyroscopeScaleFactor>().array() * state.get_field<AngularVelocity>().array();
}

template <> template <>
UKF::Vector<3> AHRS_MeasurementVector::expected_measurement
<AHRS_StateVector, Magnetometer, AHRS_SensorErrorVector>(
        const AHRS_StateVector& state, const AHRS_SensorErrorVector& input) {
    Eigen::Map<UKF::Matrix<3, 3>> mag_scale(input.get_field<MagnetometerScaleFactor>().data());
    return input.get_field<MagnetometerBias>() + 
        mag_scale * (state.get_field<Attitude>() * UKF::Vector<3>(1.0, 0.0, 0.0));
}

using AHRS_Filter = UKF::Core<
    AHRS_StateVector,
    AHRS_MeasurementVector,
    UKF::IntegratorRK4
>;

/*
AHRS parameter estimation filter process model. Since the evolution of sensor
errors is by definition unpredictable, this does nothing.
*/
template <> template <>
AHRS_SensorErrorVector AHRS_SensorErrorVector::derivative<>() const {
    return AHRS_SensorErrorVector::Zero();
}

/* AHRS parameter estimation filter process noise covariance. */
template <>
AHRS_SensorErrorVector::CovarianceMatrix AHRS_SensorErrorVector::process_noise_covariance(real_t dt) {
    return error_process_noise * dt;
}

/*
AHRS parameter estimation filter measurement model. These take in the current
state estimate and sensor scale factor and bias estimates, and use them to
calculate predicted measurements.
These functions are just the same as the state measurement model, but with
their arguments flipped.
*/
template <> template <>
UKF::Vector<3> AHRS_MeasurementVector::expected_measurement
<AHRS_SensorErrorVector, Accelerometer, AHRS_StateVector>(
        const AHRS_SensorErrorVector& state, const AHRS_StateVector& input) {
    return state.get_field<AccelerometerBias>().array() + state.get_field<AccelerometerScaleFactor>().array() *
        (input.get_field<Acceleration>() + input.get_field<Attitude>() * UKF::Vector<3>(0, 0, -G_ACCEL)).array();
}

template <> template <>
UKF::Vector<3> AHRS_MeasurementVector::expected_measurement
<AHRS_SensorErrorVector, Gyroscope, AHRS_StateVector>(
        const AHRS_SensorErrorVector& state, const AHRS_StateVector& input) {
    return state.get_field<GyroscopeBias>().array() +
        state.get_field<GyroscopeScaleFactor>().array() * input.get_field<AngularVelocity>().array();
}

template <> template <>
UKF::Vector<3> AHRS_MeasurementVector::expected_measurement
<AHRS_SensorErrorVector, Magnetometer, AHRS_StateVector>(
        const AHRS_SensorErrorVector& state, const AHRS_StateVector& input) {
    Eigen::Map<UKF::Matrix<3, 3>> mag_scale(state.get_field<MagnetometerScaleFactor>().data());
    return state.get_field<MagnetometerBias>() + 
        mag_scale * (input.get_field<Attitude>() * UKF::Vector<3>(1.0, 0.0, 0.0));
}

/* Just use the Euler integrator since there's no process model. */
using AHRS_ParameterEstimationFilter = UKF::Core<
    AHRS_SensorErrorVector,
    AHRS_MeasurementVector,
    UKF::IntegratorEuler
>;

static AHRS_Filter ahrs;
static AHRS_ParameterEstimationFilter ahrs_errors;
static AHRS_MeasurementVector meas;

/*
Set the initial measurement covariance vector. This is calculated from the
noise figures given in the datasheet.
*/
template <>
AHRS_MeasurementVector::CovarianceVector AHRS_MeasurementVector::measurement_covariance(
    (AHRS_MeasurementVector::CovarianceVector() << 0.12, 0.12, 0.12, 0.003, 0.003, 0.003, 0.3, 0.3, 0.3).finished());

/*
The following functions provide a ctypes-compatible interface for ease of
testing.
*/

void ukf_init() {
    /* Initialise state vector and covariance. */
    ahrs.state.set_field<Attitude>(UKF::Quaternion(1, 0, 0, 0));
    ahrs.state.set_field<AngularVelocity>(UKF::Vector<3>(0, 0, 0));
    ahrs.state.set_field<Acceleration>(UKF::Vector<3>(0, 0, 0));
    ahrs.covariance = AHRS_StateVector::CovarianceMatrix::Zero();
    ahrs.covariance.diagonal() << 1e0, 1e0, 1e0, 1e0, 1e0, 1e0, 5e0, 5e0, 5e0;

    /* Set process noise covariance. */
    process_noise = AHRS_StateVector::CovarianceMatrix::Zero();
    process_noise.diagonal() <<
        7e-5 * UKF::Vector<3>::Ones(),
        1e0 * UKF::Vector<3>::Ones(),
        2e1 * UKF::Vector<3>::Ones();

    /* Initialise scale factor and bias errors. */
    ahrs_errors.state.set_field<AccelerometerBias>(UKF::Vector<3>(0, 0, 0));
    ahrs_errors.state.set_field<AccelerometerScaleFactor>(UKF::Vector<3>(1, 1, 1));
    ahrs_errors.state.set_field<GyroscopeBias>(UKF::Vector<3>(0, 0, 0));
    ahrs_errors.state.set_field<GyroscopeScaleFactor>(UKF::Vector<3>(1, 1, 1));
    ahrs_errors.state.set_field<MagnetometerBias>(UKF::Vector<3>(0, 0, 0));
    UKF::Matrix<3, 3> init_scale = UKF::Matrix<3, 3>::Zero();
    init_scale.diagonal() << EARTH_MAG, EARTH_MAG, EARTH_MAG;
    Eigen::Map<UKF::Vector<9>> init_scale_map(init_scale.data());
    ahrs_errors.state.set_field<MagnetometerScaleFactor>(init_scale_map);

    /*
    Initialise scale factor and bias error covariance. These covariances are
    derived from the switch-on biases given in the MPU-6050 and HMC5883
    datasheets.

    Covariance for the magnetometer scale factor matrix might need some
    tweaking, because it's not just compensating for the scale factor error,
    but also needs to capture the uncertainty as to the local magnetic field
    vector.
    */
    ahrs_errors.covariance = AHRS_SensorErrorVector::CovarianceMatrix::Zero();
    ahrs_errors.covariance.diagonal() <<
        0.49, 0.49, 0.784, 3.0e-2 * UKF::Vector<3>::Ones(),
        0.35 * UKF::Vector<3>::Ones(), 3.0e-2 * UKF::Vector<3>::Ones(),
        1.0e1 * UKF::Vector<3>::Ones(), 5.0e-2 * EARTH_MAG * UKF::Vector<9>::Ones();

    /*
    Set scale factor and bias error process noise. For biases, this is
    derived from bias instability. For scale factors, this is set to zero
    since scale factor instability is swamped by other sources of error in
    cheap inertial sensors.

    For MPU-6050: Allan variance was used to determine bias instability.
    Gyro bias instability is about 0.003 deg/s (5.2e-5 rad/s).
    Accelerometer bias instability is about 0.003 m/s^2 – similarity to gyro
    is a coincidence!
    HMC5883 Magnetometer bias instability is about 0.015 microTesla.

    Bias instability is actually characterised as a 1/f flicker noise, so the
    function to calculate process noise covariance given a time delta should
    actually be a bunch more complex than it is. With these cheap sensors,
    though, this is all very approximate, so we'll just treat it as additive
    white noise.

    Note that these figures are probably too low because we're not explicitly
    compensating for bias change with temperature, which will have a similar
    effect.
    */
    error_process_noise = AHRS_SensorErrorVector::CovarianceMatrix::Zero();
    error_process_noise.diagonal() <<
        5.2e-5 * UKF::Vector<3>::Ones(), UKF::Vector<3>::Zero(),
        3.0e-3 * UKF::Vector<3>::Ones(), UKF::Vector<3>::Zero(),
        1.5e-2 * UKF::Vector<3>::Ones(), UKF::Vector<9>::Zero();
}

void ukf_set_acceleration(real_t x, real_t y, real_t z) {
    ahrs.state.set_field<Acceleration>(UKF::Vector<3>(x, y, z));
}

void ukf_set_attitude(real_t w, real_t x, real_t y, real_t z) {
    ahrs.state.set_field<Attitude>(UKF::Quaternion(w, x, y, z));
}

void ukf_set_angular_velocity(real_t x, real_t y, real_t z) {
    ahrs.state.set_field<AngularVelocity>(UKF::Vector<3>(x, y, z));
}

void ukf_get_state(struct ukf_state_t *in) {
    in->acceleration[0] = ahrs.state.get_field<Acceleration>()[0];
    in->acceleration[1] = ahrs.state.get_field<Acceleration>()[1];
    in->acceleration[2] = ahrs.state.get_field<Acceleration>()[2];
    in->attitude[0] = ahrs.state.get_field<Attitude>().x();
    in->attitude[1] = ahrs.state.get_field<Attitude>().y();
    in->attitude[2] = ahrs.state.get_field<Attitude>().z();
    in->attitude[3] = ahrs.state.get_field<Attitude>().w();
    in->angular_velocity[0] = ahrs.state.get_field<AngularVelocity>()[0];
    in->angular_velocity[1] = ahrs.state.get_field<AngularVelocity>()[1];
    in->angular_velocity[2] = ahrs.state.get_field<AngularVelocity>()[2];
}

void ukf_set_state(struct ukf_state_t *in) {
    ahrs.state.set_field<Acceleration>(
        UKF::Vector<3>(in->acceleration[0], in->acceleration[1], in->acceleration[2]));
    ahrs.state.set_field<Attitude>(
        UKF::Quaternion(in->attitude[3], in->attitude[0], in->attitude[1], in->attitude[2]));
    ahrs.state.set_field<AngularVelocity>(
        UKF::Vector<3>(in->angular_velocity[0], in->angular_velocity[1], in->angular_velocity[2]));
}

void ukf_get_state_covariance(
        real_t state_covariance[AHRS_StateVector::covariance_size()*AHRS_StateVector::covariance_size()]) {
    Eigen::Map<typename AHRS_StateVector::CovarianceMatrix> covariance_map(state_covariance);
    covariance_map = ahrs.covariance;
}

void ukf_get_state_covariance_diagonal(
        real_t state_covariance_diagonal[AHRS_StateVector::covariance_size()]) {
    Eigen::Map<UKF::Vector<AHRS_StateVector::covariance_size()>> covariance_map(state_covariance_diagonal);
    covariance_map = ahrs.covariance.diagonal();
}

void ukf_get_state_error(real_t state_error[AHRS_StateVector::covariance_size()]) {
    Eigen::Map<typename AHRS_StateVector::StateVectorDelta> error_map(state_error);
    error_map = ahrs.covariance.cwiseAbs().rowwise().sum().cwiseSqrt();
}

void ukf_sensor_clear() {
    meas = AHRS_MeasurementVector();
}

void ukf_sensor_set_accelerometer(real_t x, real_t y, real_t z) {
    meas.set_field<Accelerometer>(UKF::Vector<3>(x, y, z));
}

void ukf_sensor_set_gyroscope(real_t x, real_t y, real_t z) {
    meas.set_field<Gyroscope>(UKF::Vector<3>(x, y, z));
}

void ukf_sensor_set_magnetometer(real_t x, real_t y, real_t z) {
    meas.set_field<Magnetometer>(UKF::Vector<3>(x, y, z));
}

void ukf_set_params(struct ukf_sensor_params_t *in) {
    AHRS_MeasurementVector::measurement_covariance <<
        in->accel_covariance[0], in->accel_covariance[1], in->accel_covariance[2],
        in->gyro_covariance[0], in->gyro_covariance[1], in->gyro_covariance[2],
        in->mag_covariance[0], in->mag_covariance[1], in->mag_covariance[2];
}

void ukf_iterate(float dt) {
    /*
    Do a normal iteration for the AHRS filter, with the current state of
    the parameter estimation filter as the measurement input.
    */
    ahrs.a_priori_step(dt);
    ahrs.innovation_step(meas, ahrs_errors.state);
    ahrs.a_posteriori_step();

    /* Do the a priori step for the parameter estimation filter. */
    ahrs_errors.a_priori_step(dt);

    /*
    Call the innovation step using the AHRS filter a priori mean as the
    measurement.
    */
    ahrs_errors.innovation_step(meas, ahrs.state);

    /*
    Adjust the innovation covariance of the parameter estimation filter by
    adding the innovation covariance of the AHRS filter, to properly account
    for state uncertainty.
    */
    ahrs_errors.innovation_covariance += ahrs.innovation_covariance;

    /* Do the a posteriori step. */
    ahrs_errors.a_posteriori_step();
}

void ukf_set_process_noise(real_t process_noise_covariance[AHRS_StateVector::covariance_size()]) {
    Eigen::Map<typename AHRS_StateVector::StateVectorDelta> covariance_map(process_noise_covariance);
    process_noise = AHRS_StateVector::CovarianceMatrix::Zero();
    process_noise.diagonal() << covariance_map;
}

uint32_t ukf_config_get_state_dim() {
    return AHRS_StateVector::covariance_size();
}

uint32_t ukf_config_get_measurement_dim() {
    return AHRS_MeasurementVector::max_size();
}

enum ukf_precision_t ukf_config_get_precision() {
    if(sizeof(real_t) == 8) {
        return UKF_PRECISION_DOUBLE;
    } else {
        return UKF_PRECISION_FLOAT;
    }
}
