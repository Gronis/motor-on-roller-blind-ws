/*
Extends the Serial class to encode SLIP over serial
*/

#ifndef Stepper_28BYJ_48_h
#define Stepper_28BYJ_48_h

#if defined(ARDUINO) && ARDUINO >= 100
#include "Arduino.h"
#else
#include "WProgram.h"
#endif

enum SteppingMode{
  FULL_STEP = 0,
  HALF_STEP = 1,
  QUATER_STEP = 2,
  EIGHT_STEP = 3,
  SIXTEENTH_STEP = 4
};

class Stepper_28BYJ_48
{
  public:
    Stepper_28BYJ_48(int pin_1n1, int pin_1n2, int pin_1n3, int pin_1n4);
    Stepper_28BYJ_48(int pin_1n1, int pin_1n2, int pin_1n3, int pin_1n4, SteppingMode mode);
    void step(int);
    void setMotorSpeed(int motorSpeed);
    int getMotorSpeed();

  private:
    void setOutput(int out);
    int motorSpeed = 1200;  //variable to set stepper speed
    int countsperrev = 512; // number of steps per full revolution
    int pin_1n1;
    int pin_1n2;
    int pin_1n3;
    int pin_1n4;
    SteppingMode mode;
};

#endif