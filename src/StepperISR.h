#include <stdint.h>

#include "FastAccelStepper.h"
#include "common.h"

// Here are the global variables to interface with the interrupts

// These variables control the stepper timing behaviour
#define QUEUE_LEN_MASK (QUEUE_LEN - 1)

#ifdef SUPPORT_ESP32_MCPWM_PCNT
struct mapping_s {
  mcpwm_unit_t mcpwm_unit;
  uint8_t timer;
  mcpwm_io_signals_t pwm_output_pin;
  pcnt_unit_t pcnt_unit;
  uint8_t input_sig_index;
  uint32_t cmpr_tea_int_clr;
  uint32_t cmpr_tea_int_ena;
  uint32_t cmpr_tea_int_raw;
};
#endif

#if defined(SUPPORT_ESP32_PULSE_COUNTER)
bool _esp32_attachToPulseCounter(uint8_t pcnt_unit, FastAccelStepper* stepper,
                                 int16_t low_value, int16_t high_value);
void _esp32_clearPulseCounter(uint8_t pcnt_unit);
int16_t _esp32_readPulseCounter(uint8_t pcnt_unit);
#endif
#if defined(ARDUINO_ARCH_SAM)
typedef struct _PWMCHANNELMAPPING {
  uint8_t pin;
  uint32_t channel;
  Pio* port;
  uint32_t channelMask;
} PWMCHANNELMAPPING;
#endif
struct queue_entry {
  uint8_t steps;  // if 0,  then the command only adds a delay
  uint8_t toggle_dir : 1;
  uint8_t countUp : 1;
  uint8_t moreThanOneStep : 1;
  uint8_t hasSteps : 1;
  uint16_t ticks;
#if defined(ARDUINO_ARCH_AVR)
  uint16_t end_pos_last16;
#else
  uint16_t start_pos_last16;
#endif
};
class StepperQueue {
 public:
  struct queue_entry entry[QUEUE_LEN];

  // In case of forceStopAndNewPosition() the adding of commands has to be
  // temporarily suspended
  volatile bool ignore_commands;
  volatile uint8_t read_idx;  // ISR stops if readptr == next_writeptr
  volatile uint8_t next_write_idx;
  bool dirHighCountsUp;
  uint8_t dirPin;

  // a word to isRunning():
  //    if isRunning() is false, then the _QUEUE_ is not running.
  //
  // For esp32 this does NOT mean, that the HW is finished.
  // The timer is still counting down to zero until it stops at 0.
  // But there will be no interrupt to process another command.
  // So the queue requires startQueue() again
  //
  // Due to the rmt version of esp32, there has been the needed to
  // provide information, that device is not yet ready for new commands.
  // This has been called isReadyForCommands().
  //

#if defined(ARDUINO_ARCH_ESP32) || defined(ESP_PLATFORM)
  volatile bool _isRunning;
  bool _nextCommandIsPrepared;
  bool isRunning() { return _isRunning; }
  bool isReadyForCommands();
  bool use_rmt;
#ifdef SUPPORT_ESP32_MCPWM_PCNT
  const struct mapping_s* mapping;
  bool isReadyForCommands_mcpwm_pcnt();
#endif
#ifdef SUPPORT_ESP32_RMT
  rmt_channel_t channel;
  bool isReadyForCommands_rmt();
  bool _rmtStopped;
#endif
#elif defined(ARDUINO_ARCH_AVR)
  volatile uint8_t* _dirPinPort;
  uint8_t _dirPinMask;
  volatile bool _prepareForStop;
  volatile bool _isRunning;
  inline bool isRunning() { return _isRunning; }
  inline bool isReadyForCommands() { return true; }
  enum channels channel;
#elif defined(ARDUINO_ARCH_SAM)
  volatile uint32_t* _dirPinPort;
  uint32_t _dirPinMask;
  volatile bool _hasISRactive;
  bool isRunning();
  const PWMCHANNELMAPPING* mapping;
  bool _connected;
  inline bool isReadyForCommands() { return true; }
  volatile bool _pauseCommanded;
  volatile uint32_t timePWMInterruptEnabled;
#else
  volatile bool _isRunning;
  inline bool isReadyForCommands() { return true; }
  inline bool isRunning() { return _isRunning; }
#endif

  struct queue_end_s queue_end;
  uint16_t max_speed_in_ticks;

  void init(uint8_t queue_num, uint8_t step_pin);
#ifdef SUPPORT_ESP32_MCPWM_PCNT
  void init_mcpwm_pcnt(uint8_t channel_num, uint8_t step_pin);
#endif
#ifdef SUPPORT_ESP32_RMT
  void init_rmt(uint8_t channel_num, uint8_t step_pin);
#endif
  inline uint8_t queueEntries() {
    fasDisableInterrupts();
    uint8_t rp = read_idx;
    uint8_t wp = next_write_idx;
    fasEnableInterrupts();
    inject_fill_interrupt(0);
    return (uint8_t)(wp - rp);
  }
  inline bool isQueueFull() { return queueEntries() == QUEUE_LEN; }
  inline bool isQueueEmpty() { return queueEntries() == 0; }

  int8_t addQueueEntry(const struct stepper_command_s* cmd, bool start) {
    // Just to check if, if the struct has the correct size
    // if (sizeof(entry) != 6 * QUEUE_LEN) {
    //  return -1;
    //}
	if (!isReadyForCommands()) {
		return AQE_DEVICE_NOT_READY;
	}
    if (cmd == NULL) {
      if (start && !isRunning()) {
        if (next_write_idx == read_idx) {
          return AQE_ERROR_EMPTY_QUEUE_TO_START;
        }
        startQueue();
      }
      return AQE_OK;
    }
    if (isQueueFull()) {
      return AQE_QUEUE_FULL;
    }
    uint16_t period = cmd->ticks;
    uint8_t steps = cmd->steps;
    // Serial.print(period);
    // Serial.print(" ");
    // Serial.println(steps);

    uint32_t command_rate_ticks = period;
    if (steps > 1) {
      command_rate_ticks *= steps;
    }
    if (command_rate_ticks < MIN_CMD_TICKS) {
      return AQE_ERROR_TICKS_TOO_LOW;
    }

    uint8_t wp = next_write_idx;
    struct queue_entry* e = &entry[wp & QUEUE_LEN_MASK];
    bool dir = (cmd->count_up == dirHighCountsUp);
    bool toggle_dir = false;
    if (dirPin != PIN_UNDEFINED) {
      if (isQueueEmpty()) {
        // set the dirPin here. Necessary with shared direction pins
        digitalWrite(dirPin, dir);
#ifdef ARDUINO_ARCH_SAM
        delayMicroseconds(30);  // Make sure the driver has enough time to see
                                // the dir pin change
#endif
        queue_end.dir = dir;
      } else {
        toggle_dir = (dir != queue_end.dir);
      }
    }
    e->steps = steps;
    e->toggle_dir = toggle_dir;
    e->countUp = cmd->count_up ? 1 : 0;
    e->moreThanOneStep = steps > 1 ? 1 : 0;
    e->hasSteps = steps > 0 ? 1 : 0;
    e->ticks = period;
    struct queue_end_s next_queue_end = queue_end;
#if !defined(ARDUINO_ARCH_AVR)
    e->start_pos_last16 = (uint32_t)next_queue_end.pos & 0xffff;
#endif
    next_queue_end.pos += cmd->count_up ? steps : -steps;
#if defined(ARDUINO_ARCH_AVR)
    e->end_pos_last16 = (uint32_t)next_queue_end.pos & 0xffff;
#endif
    next_queue_end.dir = dir;
    next_queue_end.count_up = cmd->count_up;

    // Advance write pointer
    fasDisableInterrupts();
    if (!ignore_commands) {
		if (isReadyForCommands()) {
		  next_write_idx++;
		  queue_end = next_queue_end;
		}
		else {
			fasEnableInterrupts();
			return AQE_DEVICE_NOT_READY;
		}
    }
    fasEnableInterrupts();

    if (!isRunning() && start) {
      // stepper is not yet running and start is requested
      startQueue();
    }
    return AQE_OK;
  }

  int32_t getCurrentPosition() {
    fasDisableInterrupts();
    uint32_t pos = (uint32_t)queue_end.pos;
    uint8_t rp = read_idx;
    bool is_empty = (rp == next_write_idx);
    struct queue_entry* e = &entry[rp & QUEUE_LEN_MASK];
#if defined(ARDUINO_ARCH_AVR)
    uint16_t pos_last16 = e->end_pos_last16;
#else
    uint16_t pos_last16 = e->start_pos_last16;
#endif
    uint8_t steps = e->steps;
#if defined(ARDUINO_ARCH_ESP32) || defined(ESP_PLATFORM)
    // pulse counter should go max up to 255 with perhaps few pulses overrun, so
    // this conversion is safe
    int16_t done_p = (int16_t)_getPerformedPulses();
#endif
    fasEnableInterrupts();
#if defined(ARDUINO_ARCH_ESP32) || defined(ESP_PLATFORM)
    if (done_p == 0) {
      // fix for possible race condition described in issue #68
      fasDisableInterrupts();
      rp = read_idx;
      is_empty = (rp == next_write_idx);
      e = &entry[rp & QUEUE_LEN_MASK];
      pos_last16 = e->start_pos_last16;
      steps = e->steps;
      done_p = (int16_t)_getPerformedPulses();
      fasEnableInterrupts();
    }
#endif
    if (!is_empty) {
      int16_t adjust = 0;

      uint16_t pos16 = pos & 0xffff;
      uint8_t transition = ((pos16 >> 12) & 0x0c) | (pos_last16 >> 14);
      switch (transition) {
        case 0:   // 00 00
        case 5:   // 01 01
        case 10:  // 10 10
        case 15:  // 11 11
          break;
        case 1:   // 00 01
        case 6:   // 01 10
        case 11:  // 10 11
        case 12:  // 11 00
          pos += 0x4000;
          break;
        case 4:   // 01 00
        case 9:   // 10 01
        case 14:  // 11 10
        case 3:   // 00 11
          pos -= 0x4000;
          break;
        case 2:   // 00 10
        case 7:   // 01 11
        case 8:   // 10 00
        case 13:  // 11 01
          break;  // TODO: ERROR
      }
      pos = (int32_t)((pos & 0xffff0000) | pos_last16);

      if (steps != 0) {
        if (e->countUp) {
#if defined(ARDUINO_ARCH_AVR) || defined(ARDUINO_ARCH_SAM)
          adjust = -steps;
#elif defined(ARDUINO_ARCH_ESP32) || defined(ESP_PLATFORM)
          adjust = done_p;
#endif
        } else {
#if defined(ARDUINO_ARCH_AVR) || defined(ARDUINO_ARCH_SAM)
          adjust = steps;
#elif defined(ARDUINO_ARCH_ESP32) || defined(ESP_PLATFORM)
          adjust = -done_p;
#endif
        }
        pos += adjust;
      }
    }
    return pos;
  }
  uint32_t ticksInQueue() {
    fasDisableInterrupts();
    uint8_t rp = read_idx;
    uint8_t wp = next_write_idx;
    fasEnableInterrupts();
    if (wp == rp) {
      return 0;
    }
    uint32_t ticks = 0;
    rp++;  // ignore currently processed entry
    while (wp != rp) {
      struct queue_entry* e = &entry[rp++ & QUEUE_LEN_MASK];
      ticks += e->ticks;
      uint8_t steps = e->steps;
      if (steps > 1) {
        uint32_t tmp = e->ticks;
        tmp *= steps - 1;
        ticks += tmp;
      }
    }
    return ticks;
  }
  bool hasTicksInQueue(uint32_t min_ticks) {
    fasDisableInterrupts();
    uint8_t rp = read_idx;
    uint8_t wp = next_write_idx;
    fasEnableInterrupts();
    if (wp == rp) {
      return false;
    }
    rp++;  // ignore currently processed entry
    while (wp != rp) {
      struct queue_entry* e = &entry[rp & QUEUE_LEN_MASK];
      uint32_t tmp = e->ticks;
      uint8_t steps = max(e->steps, (uint8_t)1);
      tmp *= steps;
      if (tmp >= min_ticks) {
        return true;
      }
      min_ticks -= tmp;
      rp++;
    }
    return false;
  }
  uint16_t getActualTicks() {
    // Retrieve current step rate from the current view.
    // This is valid only, if the command describes more than one step
    fasDisableInterrupts();
    uint8_t rp = read_idx;
    uint8_t wp = next_write_idx;
    fasEnableInterrupts();
    if (wp == rp) {
      return 0;
    }
    struct queue_entry* e = &entry[rp & QUEUE_LEN_MASK];
    if (e->hasSteps) {
      if (e->moreThanOneStep) {
        return e->ticks;
      }
      if (wp != ++rp) {
        if (entry[rp & QUEUE_LEN_MASK].hasSteps) {
          return e->ticks;
        }
      }
    }
    return 0;
  }

  volatile uint16_t getMaxSpeedInTicks() { return max_speed_in_ticks; }

  // startQueue is always called
  void startQueue();
  void forceStop();
#ifdef SUPPORT_ESP32_MCPWM_PCNT
  void startQueue_mcpwm_pcnt();
  void forceStop_mcpwm_pcnt();
#endif
#ifdef SUPPORT_ESP32_RMT
  void startQueue_rmt();
  void forceStop_rmt();
#endif
  void _initVars() {
    dirPin = PIN_UNDEFINED;
#ifndef TEST
    max_speed_in_ticks =
        TICKS_PER_S / 1000;  // use a default value 1_000 steps/s
#else
    max_speed_in_ticks =
        TICKS_PER_S / 50000;  // use a default value 50_000 steps/s
#endif
    ignore_commands = false;
    read_idx = 0;
    next_write_idx = 0;
    queue_end.dir = true;
    queue_end.count_up = true;
    queue_end.pos = 0;
    dirHighCountsUp = true;
#if defined(ARDUINO_ARCH_AVR)
    _isRunning = false;
    _prepareForStop = false;
#elif defined(ARDUINO_ARCH_ESP32) || defined(ESP_PLATFORM)
    _isRunning = false;
    _nextCommandIsPrepared = false;
#if defined(SUPPORT_ESP32_RMT)
	_rmtStopped = true;
#endif
#elif defined(ARDUINO_ARCH_SAM)
    _hasISRactive = false;
    // we cannot clear the PWM interrupt when switching to a pause, but we'll
    // get a double interrupt if we do nothing.  So this tells us that on a
    // transition from a pulse to a pause to skip the next interrupt.
    _pauseCommanded = false;
    timePWMInterruptEnabled = 0;
#else
    _isRunning = false;
#endif
  }
#if defined(ARDUINO_ARCH_ESP32) || defined(ESP_PLATFORM)
  uint8_t _step_pin;
  uint16_t _getPerformedPulses();
#endif
#ifdef SUPPORT_ESP32_MCPWM_PCNT
  uint16_t _getPerformedPulses_mcpwm_pcnt();
#endif
#ifdef SUPPORT_ESP32_RMT
  uint16_t _getPerformedPulses_rmt();
#endif
#if defined(ARDUINO_ARCH_SAM)
  uint8_t _step_pin;
  uint8_t _queue_num;
#endif
  void connect();
  void disconnect();
#ifdef SUPPORT_ESP32_MCPWM_PCNT
  void connect_mcpwm_pcnt();
  void disconnect_mcpwm_pcnt();
#endif
#ifdef SUPPORT_ESP32_RMT
  void connect_rmt();
  void disconnect_rmt();
#endif
  void setDirPin(uint8_t dir_pin, bool _dirHighCountsUp) {
    dirPin = dir_pin;
    dirHighCountsUp = _dirHighCountsUp;
#if defined(ARDUINO_ARCH_AVR) || defined(ARDUINO_ARCH_SAM)
    if (dir_pin != PIN_UNDEFINED) {
      _dirPinPort = portOutputRegister(digitalPinToPort(dir_pin));
      _dirPinMask = digitalPinToBitMask(dir_pin);
    }
#endif
  }
  void adjustSpeedToStepperCount(uint8_t steppers);
  static bool isValidStepPin(uint8_t step_pin);
  static int8_t queueNumForStepPin(uint8_t step_pin);
};

extern StepperQueue fas_queue[NUM_QUEUES];

void fas_init_engine(FastAccelStepperEngine* engine, uint8_t cpu_core);
