#pragma once

#include <AP_HAL/AP_HAL_Boards.h>

/*
  macros to allow code to build on multiple platforms more easily
 */

#ifdef ALLOW_DOUBLE_MATH_FUNCTIONS
#error ALLOW_DOUBLE_MATH_FUNCTIONS has been replaced with AP_MATH_ALLOW_DOUBLE_FUNCTIONS (which must be defined).  So replace "define ALLOW_DOUBLE_MATH_FUNCTIONS" with "define AP_MATH_ALLOW_DOUBLE_FUNCTIONS 1"
#endif  // ALLOW_DOUBLE_MATH_FUNCTIONS

#ifndef AP_MATH_ALLOW_DOUBLE_FUNCTIONS
/*
  allow double maths on Linux and SITL to avoid problems with system headers
 */
#define AP_MATH_ALLOW_DOUBLE_FUNCTIONS (CONFIG_HAL_BOARD == HAL_BOARD_SITL || CONFIG_HAL_BOARD == HAL_BOARD_LINUX || HAL_WITH_EKF_DOUBLE || AP_SIM_ENABLED)
#endif  // AP_MATH_ALLOW_DOUBLE_FUNCTIONS

// we need to include math.h here for newer compilers (eg. g++ 7.3.1 for stm32)
#include <math.h>

#if !AP_MATH_ALLOW_DOUBLE_FUNCTIONS
/* give warnings if we use double precision maths functions without
   specifying ALLOW_DOUBLE_TRIG_FUNCTIONS. Code should use the
   equivalent f function instead (eg. use cosf() instead of
   cos()). Individual cpp files that really do need double precision
   should define ALLOW_DOUBLE_TRIG_FUNCTIONS before including
   AP_Math.h
*/
#define sin(x) DO_NOT_USE_DOUBLE_MATHS()
#define cos(x) DO_NOT_USE_DOUBLE_MATHS()
#define tan(x) DO_NOT_USE_DOUBLE_MATHS()
#define acos(x) DO_NOT_USE_DOUBLE_MATHS()
#define asin(x) DO_NOT_USE_DOUBLE_MATHS()
#define atan(x) DO_NOT_USE_DOUBLE_MATHS()
#define atan2(x,y) DO_NOT_USE_DOUBLE_MATHS()
#define exp(x) DO_NOT_USE_DOUBLE_MATHS()
#define pow(x,y) DO_NOT_USE_DOUBLE_MATHS()
#define sqrt(x) DO_NOT_USE_DOUBLE_MATHS()
#define log2(x) DO_NOT_USE_DOUBLE_MATHS()
#define log10(x) DO_NOT_USE_DOUBLE_MATHS()
#define ceil(x) DO_NOT_USE_DOUBLE_MATHS()
#define floor(x) DO_NOT_USE_DOUBLE_MATHS()
#define round(x) DO_NOT_USE_DOUBLE_MATHS()
#define fmax(x,y) DO_NOT_USE_DOUBLE_MATHS()
#if !HAL_NUM_CAN_IFACES
// we should do log() and fabs() as well, but can't because of a conflict in uavcan
#define log(x) DO_NOT_USE_DOUBLE_MATHS()
#define fabs(x) DO_NOT_USE_DOUBLE_MATHS()
#endif
#endif

