#include "stepper.h"

// Max is 1023 on nodemcu, std arduino libary use 254 but constant is not defined.
#ifndef PWMRANGE
#define PWMRANGE 254
#endif

// Table for micro-steppning using pwm, computed by: tan(x*pi/32)*255, where x is angle
//   Radian |  Duty-cycle(255) | Hex
// 0pi / 32 |     255          | 0xFF
// 1pi / 32 |     230          | 0xE6
// 2pi / 32 |     204          | 0xCC
// 3pi / 32 |     178          | 0xB2
// 4pi / 32 |     149          | 0x95
// 5pi / 32 |     119          | 0x77
// 6pi / 32 |      85          | 0x55
// 7pi / 32 |      46          | 0x2E
// 8pi / 32 |       0          | 0x00
//
const unsigned short LOOKUP_STEPS[] =
{
    0xFF00, 0xFF2E, 0xFF55, 0xFF77, 0xFF95, 0xFFB2, 0xFFCC, 0xFFE6,
    0xFFFF, 0xE6FF, 0xCCFF, 0xB2FF, 0x95FF, 0x77FF, 0x55FF, 0x2EFF
};

const int NR_PATTERNS = sizeof(LOOKUP_STEPS) / sizeof(LOOKUP_STEPS[0]);
const int NR_PINS = 4;

#define pattern_for_pin(pin, pattern) (0xFF & (pattern >> (8 * pin))) * ((PWMRANGE + 1) / 255)
#define steps(mode) (NR_PATTERNS / (1 << (unsigned int)mode))

Stepper_28BYJ_48::Stepper_28BYJ_48(int _pin_1n1, int _pin_1n2, int _pin_1n3, int _pin_1n4,
                                    SteppingMode _mode)
{

    pin_1n1 = _pin_1n1;
    pin_1n2 = _pin_1n2;
    pin_1n3 = _pin_1n3;
    pin_1n4 = _pin_1n4;

    pinMode(pin_1n1, OUTPUT);
    pinMode(pin_1n2, OUTPUT);
    pinMode(pin_1n3, OUTPUT);
    pinMode(pin_1n4, OUTPUT);

    mode = _mode;
};

Stepper_28BYJ_48::Stepper_28BYJ_48(int _pin_1n1, int _pin_1n2, int _pin_1n3, int _pin_1n4)
    : Stepper_28BYJ_48::Stepper_28BYJ_48(_pin_1n1, _pin_1n2, _pin_1n3, _pin_1n4, SIXTEENTH_STEP){};

void Stepper_28BYJ_48::step(int count)
{
    while (count > 0)
    {
        for (int i = 0; i < NR_PATTERNS * NR_PINS; i += steps(mode))
        {
            setOutput(i);
            delayMicroseconds(motorSpeed >> (int)mode);
        }
        count--;
    }

    while (count < 0)
    {
        for (int i = NR_PATTERNS * NR_PINS - steps(mode); i >= 0; i -= steps(mode))
        {
            setOutput(i);
            delayMicroseconds(motorSpeed >> (int)mode);
        }
        count++;
    }
};

void Stepper_28BYJ_48::setMotorSpeed(int motorSpeed)
{
    this->motorSpeed = motorSpeed;
}

int Stepper_28BYJ_48::getMotorSpeed()
{
    return this->motorSpeed;
}

void Stepper_28BYJ_48::setOutput(int out)
{
    unsigned int p_index = (unsigned int)(out % NR_PATTERNS);
    unsigned int p_offset = (unsigned int)(out / NR_PATTERNS);
    unsigned int pattern = ((unsigned int)LOOKUP_STEPS[p_index]) << 16;
    // offset pattern with p_offset, but flip back the 8 rightmost bits if we are on the last step
    unsigned int offseted_pattern = (pattern >> (8 * p_offset)) |
                                    (((pattern & 0x00FF0000) << 8) * (p_offset == NR_PINS - 1));
    analogWrite(pin_1n1, pattern_for_pin(0, offseted_pattern));
    analogWrite(pin_1n2, pattern_for_pin(1, offseted_pattern));
    analogWrite(pin_1n3, pattern_for_pin(2, offseted_pattern));
    analogWrite(pin_1n4, pattern_for_pin(3, offseted_pattern));
};