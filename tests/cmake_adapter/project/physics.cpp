// Heterogeneous unit A: a different compile configuration (DEMO_PHYSICS_VARIANT)
// from unit B, yet both commit atomically through one group.

static int s_physics = 0;

extern "C" int mixed_physics() {
  ++s_physics;
  return s_physics * 100 + DEMO_PHYSICS_VARIANT; // the variant digit proves
                                                 // unit A's defines are live
}
