cmake_minimum_required(VERSION 3.16)
project(SmartDehumidifierCPP CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(include)

add_executable(smart_dehumidifier src/main.cpp)
add_executable(servo_calibrator src/calibrate_servo.cpp)
