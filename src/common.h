#ifndef COMMON_H
#define COMMON_H

//	ticks is multiplied by (1/TICKS_PER_S) in s
//	If steps is 0, then a pause is generated
struct stepper_command_s {
  uint16_t ticks;
  uint8_t steps;
  bool count_up;
};

struct queue_end_s {
  int32_t pos;  // in steps
  bool count_up;
  bool dir;
};

#if defined(TEST)
#define fasEnableInterrupts()
#define fasDisableInterrupts()
#elif defined(ARDUINO_ARCH_ESP32)
#define fasEnableInterrupts interrupts
#define fasDisableInterrupts noInterrupts
#endif
#elif defined(ARDUINO_ARCH_AVR)
#define fasEnableInterrupts interrupts
#define fasDisableInterrupts noInterrupts
#else
#error "Unsupported derivate"
#endif

#endif /* COMMON_H */
