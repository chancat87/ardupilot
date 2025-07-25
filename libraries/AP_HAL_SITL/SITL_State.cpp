#include <AP_HAL/AP_HAL.h>

#if CONFIG_HAL_BOARD == HAL_BOARD_SITL && !defined(HAL_BUILD_AP_PERIPH)

#include "AP_HAL_SITL.h"
#include "AP_HAL_SITL_Namespace.h"
#include "HAL_SITL_Class.h"
#include "UARTDriver.h"
#include "Scheduler.h"
#include "CANSocketIface.h"

#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>

#include <AP_Param/AP_Param.h>
#include <SITL/SIM_JSBSim.h>
#include <AP_HAL/utility/Socket_native.h>

extern const AP_HAL::HAL& hal;

using namespace HALSITL;

void SITL_State::_set_param_default(const char *parm)
{
    char *pdup = strdup(parm);
    char *p = strchr(pdup, '=');
    if (p == nullptr) {
        printf("Please specify parameter as NAME=VALUE");
        exit(1);
    }
    float value = strtof(p+1, nullptr);
    *p = 0;
    enum ap_var_type var_type;
    AP_Param *vp = AP_Param::find(pdup, &var_type);
    if (vp == nullptr) {
        printf("Unknown parameter %s\n", pdup);
        exit(1);
    }
    if (var_type == AP_PARAM_FLOAT) {
        ((AP_Float *)vp)->set_and_save(value);
    } else if (var_type == AP_PARAM_INT32) {
        ((AP_Int32 *)vp)->set_and_save(value);
    } else if (var_type == AP_PARAM_INT16) {
        ((AP_Int16 *)vp)->set_and_save(value);
    } else if (var_type == AP_PARAM_INT8) {
        ((AP_Int8 *)vp)->set_and_save(value);
    } else {
        printf("Unable to set parameter %s\n", pdup);
        exit(1);
    }
    printf("Set parameter %s to %f\n", pdup, value);
    free(pdup);
}


/*
  setup for SITL handling
 */
void SITL_State::_sitl_setup()
{
#if !defined(__CYGWIN__) && !defined(__CYGWIN64__)
    _parent_pid = getppid();
#endif

    fprintf(stdout, "Starting SITL input\n");

    _sitl = AP::sitl();

    if (_sitl != nullptr) {
        // setup some initial values
        _update_airspeed(0);
#if AP_SIM_SOLOGIMBAL_ENABLED
        if (enable_gimbal) {
            gimbal = NEW_NOTHROW SITL::SoloGimbal();
        }
#endif

        sitl_model->set_buzzer(&_sitl->buzzer_sim);
        sitl_model->set_sprayer(&_sitl->sprayer_sim);
        sitl_model->set_gripper_servo(&_sitl->gripper_sim);
        sitl_model->set_gripper_epm(&_sitl->gripper_epm_sim);
        sitl_model->set_parachute(&_sitl->parachute_sim);
        sitl_model->set_precland(&_sitl->precland_sim);
        _sitl->i2c_sim.init();
        sitl_model->set_i2c(&_sitl->i2c_sim);
#if AP_TEST_DRONECAN_DRIVERS
        sitl_model->set_dronecan_device(&_sitl->dronecan_sim);
#endif
        if (_use_fg_view) {
            fg_socket.connect(_fg_address, _fg_view_port);
        }

        fprintf(stdout, "Using Irlock at port : %d\n", _irlock_port);
        _sitl->irlock_port = _irlock_port;

        _sitl->rcin_port = _rcin_port;
    }

    // start with non-zero clock
    hal.scheduler->stop_clock(1);
}


/*
  step the FDM by one time step
 */
void SITL_State::_fdm_input_step(void)
{
    _fdm_input_local();

    /* make sure we die if our parent dies */
    if (kill(_parent_pid, 0) != 0) {
        exit(1);
    }

    if (_scheduler->interrupts_are_blocked() || _sitl == nullptr) {
        return;
    }

    _scheduler->sitl_begin_atomic();

    if (_update_count == 0 && _sitl != nullptr) {
        HALSITL::Scheduler::timer_event();
        _scheduler->sitl_end_atomic();
        return;
    }

    if (_sitl != nullptr) {
        _update_airspeed(_sitl->state.airspeed);
        _update_rangefinder();
    }

    // trigger all APM timers.
    HALSITL::Scheduler::timer_event();
    _scheduler->sitl_end_atomic();
}


void SITL_State::wait_clock(uint64_t wait_time_usec)
{
    float speedup = sitl_model->get_speedup();
    if (speedup < 1) {
        // for purposes of sleeps treat low speedups as 1
        speedup = 1.0;
    }
    while (AP_HAL::micros64() < wait_time_usec) {
        if (hal.scheduler->in_main_thread() ||
            Scheduler::from(hal.scheduler)->semaphore_wait_hack_required()) {
            _fdm_input_step();
        } else {
#ifdef CYGWIN_BUILD
            if (speedup > 2 && hal.util->get_soft_armed()) {
                const char *current_thread = Scheduler::from(hal.scheduler)->get_current_thread_name();
                if (current_thread && strcmp(current_thread, "Scripting") == 0) {
                    // this effectively does a yield of the CPU. The
                    // granularity of sleeps on cygwin is very high,
                    // so this is needed for good thread performance
                    // in scripting. We don't do this at low speedups
                    // as it causes the cpu to run hot
                    // We also don't do it while disarmed, as lua performance is less
                    // critical while disarmed
                    usleep(0);
                    continue;
                }
            }
#endif
            usleep(1000);
        }
    }
    // check the outbound TCP queue size.  If it is too long then
    // MAVProxy/pymavlink take too long to process packets and it ends
    // up seeing traffic well into our past and hits time-out
    // conditions.
    if (speedup > 1 && hal.scheduler->in_main_thread()) {
        while (true) {
            HALSITL::UARTDriver *uart = (HALSITL::UARTDriver*)hal.serial(0);
            const int queue_length = uart->get_system_outqueue_length();
            // ::fprintf(stderr, "queue_length=%d\n", (signed)queue_length);
            if (queue_length < 1024) {
                break;
            }
            _serial_0_outqueue_full_count++;
            uart->handle_reading_from_device_to_readbuffer();
            usleep(1000);
        }
    }
}

/*
  output current state to flightgear
 */
void SITL_State::_output_to_flightgear(void)
{
    SITL::FGNetFDM fdm {};
    const SITL::sitl_fdm &sfdm = _sitl->state;

    fdm.version = 0x18;
    fdm.padding = 0;
    fdm.longitude = DEG_TO_RAD_DOUBLE*sfdm.longitude;
    fdm.latitude = DEG_TO_RAD_DOUBLE*sfdm.latitude;
    fdm.altitude = sfdm.altitude;
    fdm.agl = sfdm.altitude;
    fdm.phi   = radians(sfdm.rollDeg);
    fdm.theta = radians(sfdm.pitchDeg);
    fdm.psi   = radians(sfdm.yawDeg);
    fdm.vcas  = sfdm.velocity_air_bf.length()/0.3048;
    if (_vehicle == ArduCopter) {
        fdm.num_engines = 4;
        for (uint8_t i=0; i<4; i++) {
            fdm.rpm[i] = constrain_float((pwm_output[i]-1000), 0, 1000);
        }
    } else {
        fdm.num_engines = 4;
        fdm.rpm[0] = constrain_float((pwm_output[2]-1000)*3, 0, 3000);
        // for quadplane
        fdm.rpm[1] = constrain_float((pwm_output[5]-1000)*12, 0, 12000);
        fdm.rpm[2] = constrain_float((pwm_output[6]-1000)*12, 0, 12000);
        fdm.rpm[3] = constrain_float((pwm_output[7]-1000)*12, 0, 12000);
    }
    fdm.ByteSwap();

    fg_socket.send(&fdm, sizeof(fdm));
}

/*
  get FDM input from a local model
 */
void SITL_State::_fdm_input_local(void)
{
    if (_sitl == nullptr) {
        return;
    }
    struct sitl_input input;

    // construct servos structure for FDM
    _simulator_servos(input);

#if AP_SIM_JSON_MASTER_ENABLED
    // read servo inputs from ride along flight controllers
    ride_along.receive(input);
#endif  // AP_SIM_JSON_MASTER_ENABLED

    // replace outputs from multicast
    multicast_servo_update(input);

    // update the model
    sitl_model->update_home();
    sitl_model->update_model(input);

    // get FDM output from the model
    sitl_model->fill_fdm(_sitl->state);

#if HAL_NUM_CAN_IFACES
    if (CANIface::num_interfaces() > 0) {
        multicast_state_send();
    }
#endif

#if AP_SIM_JSON_MASTER_ENABLED
    // output JSON state to ride along flight controllers
    ride_along.send(_sitl->state,sitl_model->get_position_relhome());
#endif  // AP_SIM_JSON_MASTER_ENABLED

    sim_update();

    if (_use_fg_view) {
        _output_to_flightgear();
    }

    // update simulation time
    hal.scheduler->stop_clock(_sitl->state.timestamp_us);

    set_height_agl();

    _update_count++;
}

/*
  create sitl_input structure for sending to FDM
 */
void SITL_State::_simulator_servos(struct sitl_input &input)
{
    if (_sitl == nullptr) {
        return;
    }
    static uint32_t last_update_usec;

    /* this maps the registers used for PWM outputs. The RC
     * driver updates these whenever it wants the channel output
     * to change */

    if (last_update_usec == 0 || !output_ready) {
        for (uint8_t i=0; i<SITL_NUM_CHANNELS; i++) {
            pwm_output[i] = 1000;
        }
        if (_vehicle == ArduPlane) {
            pwm_output[0] = pwm_output[1] = pwm_output[3] = 1500;
        }
        if (_vehicle == Rover) {
            pwm_output[0] = pwm_output[1] = pwm_output[2] = pwm_output[3] = 1500;
        }
        if (_vehicle == ArduSub) {
            pwm_output[0] = pwm_output[1] = pwm_output[2] = pwm_output[3] =
                    pwm_output[4] = pwm_output[5] = pwm_output[6] = pwm_output[7] = 1500;
        }
    }

    // output at chosen framerate
    uint32_t now = AP_HAL::micros();
    last_update_usec = now;

    float wind_speed = 0;
    float wind_direction = 0;
    float wind_dir_z = 0;

    // give 5 seconds to calibrate airspeed sensor at 0 wind speed
    if (wind_start_delay_micros == 0) {
        wind_start_delay_micros = now;
    } else if ((now - wind_start_delay_micros) > 5000000 ) {
        // The EKF does not like step inputs so this LPF keeps it happy.
        uint32_t dt_us = now - last_wind_update_us;
        if (dt_us > 1000) {
            last_wind_update_us = now;
            // slew wind based on the configured time constant
            const float dt = dt_us * 1.0e-6;
            const float tc = MAX(_sitl->wind_change_tc, 0.1);
            const float alpha = calc_lowpass_alpha_dt(dt, 1.0/tc);
            _sitl->wind_speed_active     += (_sitl->wind_speed - _sitl->wind_speed_active) * alpha;
            _sitl->wind_direction_active += (wrap_180(_sitl->wind_direction - _sitl->wind_direction_active)) * alpha;
            _sitl->wind_dir_z_active     += (_sitl->wind_dir_z - _sitl->wind_dir_z_active) * alpha;
            _sitl->wind_direction_active = wrap_180(_sitl->wind_direction_active);
        }
        wind_speed =     _sitl->wind_speed_active;
        wind_direction = _sitl->wind_direction_active;
        wind_dir_z =     _sitl->wind_dir_z_active;
        
        // pass wind into simulators using different wind types via param SIM_WIND_T*.
        const float altitude = _sitl->state.height_agl;
        switch (_sitl->wind_type) {
        case SITL::SIM::WIND_TYPE_SQRT:
            if (altitude < _sitl->wind_type_alt) {
                wind_speed *= sqrtf(MAX(altitude / _sitl->wind_type_alt, 0));
            }
            break;

        case SITL::SIM::WIND_TYPE_COEF:
            wind_speed += (altitude - _sitl->wind_type_alt) * _sitl->wind_type_coef;
            break;

        case SITL::SIM::WIND_TYPE_NO_LIMIT:
        default:
            break;
        }

        // never allow negative wind velocity
        wind_speed = MAX(wind_speed, 0);
    }

    input.wind.speed = wind_speed;
    input.wind.direction = wind_direction;
    input.wind.turbulence = _sitl->wind_turbulance;
    input.wind.dir_z = wind_dir_z;

    for (uint8_t i=0; i<SITL_NUM_CHANNELS; i++) {
        if (pwm_output[i] == 0xFFFF) {
            input.servos[i] = 0;
        } else {
            input.servos[i] = pwm_output[i];
        }
    }

    // FETtec ESC simulation support.  Input signals of 1000-2000
    // are positive thrust, 0 to 1000 are negative thrust.  Deeper
    // changes required to support negative thrust - potentially
    // adding a field to input.
    if (_sitl->fetteconewireesc_sim.enabled()) {
        _sitl->fetteconewireesc_sim.update_sitl_input_pwm(input);
        for (uint8_t i=0; i<ARRAY_SIZE(input.servos); i++) {
            if (input.servos[i] != 0 && input.servos[i] < 1000) {
                AP_HAL::panic("Bad input servo value (%u)", input.servos[i]);
            }
        }
    }

#if AP_SIM_VOLZ_ENABLED
    // update simulation input based on data received via "serial" to
    // Volz servos:
    if (_sitl->volz_sim.enabled()) {
        _sitl->volz_sim.update_sitl_input_pwm(input);
        for (uint8_t i=0; i<ARRAY_SIZE(input.servos); i++) {
            if (input.servos[i] != 0 && input.servos[i] < 1000) {
                AP_HAL::panic("Bad input servo value (%u)", input.servos[i]);
            }
        }
    }
#endif

    const float engine_mul = _sitl->engine_mul.get();
    const uint32_t engine_fail = _sitl->engine_fail.get();

    // apply engine multiplier to motor defined by the SIM_ENGINE_FAIL parameter
    for (uint8_t i=0; i<ARRAY_SIZE(input.servos); i++) {
        if (engine_fail & (1<<i)) {
            if (_vehicle != Rover) {
                input.servos[i] = ((input.servos[i]-1000) * engine_mul) + 1000;
            } else {
                input.servos[i] = static_cast<uint16_t>(((input.servos[i] - 1500) * engine_mul) + 1500);
            }
        }
    }

    float throttle = 0.0f;
    if (_vehicle == ArduPlane) {
        float forward_throttle = constrain_float((input.servos[2] - 1000) / 1000.0f, 0.0f, 1.0f);
        // do a little quadplane dance
        float hover_throttle = 0.0f;
        uint8_t running_motors = 0;
        uint32_t mask = _sitl->state.motor_mask;
        uint8_t bit;
        while ((bit = __builtin_ffs(mask)) != 0) {
            uint8_t motor = bit-1;
            mask &= ~(1U<<motor);
            float motor_throttle = constrain_float((input.servos[motor] - 1000) / 1000.0f, 0.0f, 1.0f);
            // update motor_on flag
            if (!is_zero(motor_throttle)) {
                hover_throttle += motor_throttle;
                running_motors++;
            }
        }
        if (running_motors > 0) {
            hover_throttle /= running_motors;
        }
        if (!is_zero(forward_throttle)) {
            throttle = forward_throttle;
        } else {
            throttle = hover_throttle;
        }
    } else if (_vehicle == Rover) {
        if (input.servos[2] != 0) {
            input.servos[2] = static_cast<uint16_t>(constrain_int16(input.servos[2], 1000, 2000));
            throttle = fabsf((input.servos[2] - 1500) / 500.0f);
        } else {
            throttle = 0;
        }
        input.servos[0] = static_cast<uint16_t>(constrain_int16(input.servos[0], 1000, 2000));
    } else {
        // run checks on each motor
        uint8_t running_motors = 0;
        uint32_t mask = _sitl->state.motor_mask;
        uint8_t bit;
        while ((bit = __builtin_ffs(mask)) != 0) {
            const uint8_t motor = bit-1;
            mask &= ~(1U<<motor);
            float motor_throttle = constrain_float((input.servos[motor] - 1000) / 1000.0f, 0.0f, 1.0f);
            // update motor_on flag
            if (!is_zero(motor_throttle)) {
                throttle += motor_throttle;
                running_motors++;
            }
        }
        if (running_motors > 0) {
            throttle /= running_motors;
        }
    }
    _sitl->throttle = throttle;

    update_voltage_current(input, throttle);
}

void SITL_State::init(int argc, char * const argv[])
{
    _scheduler = Scheduler::from(hal.scheduler);
    _parse_command_line(argc, argv);
}

/*
  set height above the ground in meters
 */
void SITL_State::set_height_agl(void)
{
    static float home_alt = -1;

    if (!_sitl) {
        // in example program
        return;
    }

    if (is_equal(home_alt, -1.0f) && _sitl->state.altitude > 0) {
        // remember home altitude as first non-zero altitude
        home_alt = _sitl->state.altitude;
    }

#if AP_TERRAIN_AVAILABLE
    if (_sitl->terrain_enable) {
        // get height above terrain from AP_Terrain. This assumes
        // AP_Terrain is working
        float terrain_height_amsl;
        Location location;
        location.lat = _sitl->state.latitude*1.0e7;
        location.lng = _sitl->state.longitude*1.0e7;

        AP_Terrain *_terrain = AP_Terrain::get_singleton();
        if (_terrain != nullptr &&
            _terrain->height_amsl(location, terrain_height_amsl, false)) {
            _sitl->state.height_agl = _sitl->state.altitude - terrain_height_amsl;
            return;
        }
    }
#endif

    // fall back to flat earth model
    _sitl->state.height_agl = _sitl->state.altitude - home_alt;
}

/*
  open multicast UDP
 */
void SITL_State::multicast_state_open(void)
{
    struct sockaddr_in sockaddr {};
    int ret;

#ifdef HAVE_SOCK_SIN_LEN
    sockaddr.sin_len = sizeof(sockaddr);
#endif
    sockaddr.sin_port = htons(SITL_MCAST_PORT);
    sockaddr.sin_family = AF_INET;
    sockaddr.sin_addr.s_addr = inet_addr(SITL_MCAST_IP);

    mc_out_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (mc_out_fd == -1) {
        fprintf(stderr, "socket failed - %s\n", strerror(errno));
        exit(1);
    }
    ret = fcntl(mc_out_fd, F_SETFD, FD_CLOEXEC);
    if (ret == -1) {
        fprintf(stderr, "fcntl failed on setting FD_CLOEXEC - %s\n", strerror(errno));
        exit(1);
    }

    // try to setup for broadcast, this may fail if insufficient privileges
    int one = 1;
    setsockopt(mc_out_fd,SOL_SOCKET,SO_BROADCAST,(char *)&one,sizeof(one));

    ret = connect(mc_out_fd, (struct sockaddr *)&sockaddr, sizeof(sockaddr));
    if (ret == -1) {
        fprintf(stderr, "udp connect failed on port %u - %s\n",
                (unsigned)ntohs(sockaddr.sin_port),
                strerror(errno));
        exit(1);
    }

    /*
      open servo input socket
     */
    servo_in_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (servo_in_fd == -1) {
        fprintf(stderr, "socket failed - %s\n", strerror(errno));
        exit(1);
    }
    ret = fcntl(servo_in_fd, F_SETFD, FD_CLOEXEC);
    if (ret == -1) {
        fprintf(stderr, "fcntl failed on setting FD_CLOEXEC - %s\n", strerror(errno));
        exit(1);
    }

    sockaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    sockaddr.sin_port = htons(SITL_SERVO_PORT + _instance);

    ret = bind(servo_in_fd, (struct sockaddr *)&sockaddr, sizeof(sockaddr));
    if (ret == -1) {
        fprintf(stderr, "udp servo connect failed\n");
        exit(1);
    }
    ::printf("multicast initialised\n");
}

/*
  send out SITL state as multicast UDP
 */
void SITL_State::multicast_state_send(void)
{
    if (_sitl == nullptr) {
        return;
    }
    if (mc_out_fd == -1) {
        multicast_state_open();
    }
    const auto &sfdm = _sitl->state;
    send(mc_out_fd, (void*)&sfdm, sizeof(sfdm), 0);

    check_servo_input();
}

/*
  check for servo data from peripheral
 */
void SITL_State::check_servo_input(void)
{
    // drain any pending packets
    float mc_servo_float[SITL_NUM_CHANNELS];
    // we loop to ensure we drain all packets from all nodes
    while (recv(servo_in_fd, (void*)mc_servo_float, sizeof(mc_servo_float), MSG_DONTWAIT) == sizeof(mc_servo_float)) {
        for (uint8_t i=0; i<SITL_NUM_CHANNELS; i++) {
            // nan means that node is not outputting this channel
            if (!isnan(mc_servo_float[i])) {
                mc_servo[i] = uint16_t(mc_servo_float[i]);
            }
        }
    }
}

/*
  overwrite input structure with multicast values
 */
void SITL_State::multicast_servo_update(struct sitl_input &input)
{
    for (uint8_t i=0; i<SITL_NUM_CHANNELS; i++) {
        const uint32_t mask = (1U<<i);
        const uint32_t can_mask = uint32_t(_sitl->can_servo_mask.get());
        if (can_mask & mask) {
            input.servos[i] = mc_servo[i];
        }
    }
}
#endif
