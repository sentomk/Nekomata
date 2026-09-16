#include "state_api.hpp"

extern "C" int state_marker() {
  return 1;
}

extern "C" int state_a_value() {
  return -1;
}

extern "C" int state_shared_value() {
  return -1;
}

extern "C" int state_retry_value() {
  return -1;
}
