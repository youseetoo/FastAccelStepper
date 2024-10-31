#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "FastAccelStepper.h"
#include "StepperISR.h"

char TCCR1A;
char TCCR1B;
char TCCR1C;
char TIMSK1;
char TIFR1;
unsigned short OCR1A;
unsigned short OCR1B;

StepperQueue fas_queue[NUM_QUEUES];

void inject_fill_interrupt(int mark) {}
void noInterrupts() {}
void interrupts() {}

#include "RampChecker.h"

class FastAccelStepperTest {
 public:
  void init_queue() {
    fas_queue[0].read_idx = 0;
    fas_queue[1].read_idx = 0;
    fas_queue[0].next_write_idx = 0;
    fas_queue[1].next_write_idx = 0;
  }

  void ramp(uint8_t forward_planning, uint32_t expected_steps) {
    init_queue();
    FastAccelStepper s = FastAccelStepper();
    s.init(NULL, 0, 0);
    RampChecker rc = RampChecker();
    assert(0 == s.getCurrentPosition());

    uint32_t speed_us = 1000000 / 3600;
    assert(s.isQueueEmpty());
    s.setSpeedInUs(speed_us);
    s.setAcceleration(320);
    s.setForwardPlanningTimeInMs(forward_planning);
    s.fill_queue();
    assert(s.isQueueEmpty());
    float old_planned_time_in_buffer = 0;

    char fname[100];
    float sum_planning_time = 0;
    float points = 0;
    snprintf(fname, 100, "test_15_%dms.gnuplot", forward_planning);
    rc.start_plot(fname);
    s.runForward();
    for (int i = 0; i < 2000; i++) {
      if (i == 1000) {
        printf("Change speed\n");
        s.setSpeedInUs(10000);
        s.applySpeedAcceleration();
      }
      if (true) {
        printf(
            "Loop %d: Queue read/write = %d/%d    Target pos = %d, Queue End "
            "pos = %d  QueueEmpty=%s\n",
            i, fas_queue[0].read_idx, fas_queue[0].next_write_idx,
            s.targetPos(), s.getPositionAfterCommandsCompleted(),
            s.isQueueEmpty() ? "yes" : "no");
      }
      if (!s.isRampGeneratorActive()) {
        break;
      }
      s.fill_queue();
      uint32_t from_dt = rc.total_ticks;
      while (!s.isQueueEmpty()) {
        rc.increase_ok = true;
        rc.decrease_ok = true;
        rc.check_section(
            &fas_queue[0].entry[fas_queue[0].read_idx & QUEUE_LEN_MASK]);
        fas_queue[0].read_idx++;
      }
      uint32_t to_dt = rc.total_ticks;
      float planned_time = (to_dt - from_dt) * 1.0 / 16000000;
      printf("planned time in buffer: %.6fs\n", planned_time);
      sum_planning_time += planned_time;
      points += 1.0;
      // This must be ensured, so that the stepper does not run out of
      // commands
      assert((i == 0) || (old_planned_time_in_buffer > 0.005));
      old_planned_time_in_buffer = planned_time;
      // stop after
      if (rc.total_ticks > TICKS_PER_S * 40) {
        break;
      }
    }
    rc.finish_plot();
    // test(!s.isRampGeneratorActive(), "too many commands created");
    printf("current position = %d\n", s.getCurrentPosition());
    test(s.getCurrentPosition() > expected_steps - 10, "stepper runs too slow");
    test(s.getCurrentPosition() < expected_steps + 10, "stepper runs too fast");
    printf("Total time  %f\n", rc.total_ticks / 16000000.0);
    float avg_time = sum_planning_time / points * 1000.0;
    printf("Average planning time: %f ms\n", avg_time);
    test(avg_time < forward_planning + 1, "too much forward planning");

#if (TEST_CREATE_QUEUE_CHECKSUM == 1)
    printf("CHECKSUM for %d/%d/%d: %d\n", steps, travel_dt, accel, s.checksum);
#endif
  }
};
int main() {
  FastAccelStepperTest test;
  // run the ramp twice with 20 and with 5ms planning time.
  // the ramp will change speed after half of the loops.
  // The 5ms ramp will not have 20ms coasting in the buffer and as such runs much shorter.
  test.ramp(20, 76936);
  test.ramp(5, 11273);
  printf("TEST_15 PASSED\n");
  return 0;
}
