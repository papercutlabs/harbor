#pragma once
// Stable C ABI for the Ultraviolet event adapter. Negative key codes are
// navigation keys; nonnegative codes are Unicode code points, never wire bytes.
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
  // For mouse wheel events, code is the number of consecutive queued ticks.
  int kind, code, mods, x, y, button, action;
  char* text;
} HarborInputEvent;
int HarborInputStart(void);
void HarborInputStop(void);
HarborInputEvent HarborInputPoll(int timeout_ms);
#ifdef __cplusplus
}
#endif
