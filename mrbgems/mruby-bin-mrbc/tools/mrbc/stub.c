#include <mruby.h>

/*
  functions defined in mrbgems referenced from the core should be listed here
  to avoid link errors, since mrbc does not link any mrbgem ignoring configuration.
*/

#ifdef MRB_USE_COMPLEX
mrb_value mrb_complex_new(mrb_state *mrb, mrb_float x, mrb_float y)
{
  return mrb_nil_value();
}
mrb_value mrb_complex_add(mrb_state *mrb, mrb_value x, mrb_value y)
{
  return mrb_nil_value();
}
mrb_value mrb_complex_sub(mrb_state *mrb, mrb_value x, mrb_value y)
{
  return mrb_nil_value();
}
mrb_value mrb_complex_mul(mrb_state *mrb, mrb_value x, mrb_value y)
{
  return mrb_nil_value();
}
mrb_value mrb_complex_div(mrb_state *mrb, mrb_value x, mrb_value y)
{
  return mrb_nil_value();
}
mrb_value mrb_complex_to_i(mrb_state *mrb, mrb_value x)
{
  return mrb_nil_value();
}
mrb_value mrb_complex_to_f(mrb_state *mrb, mrb_value x)
{
  return mrb_nil_value();
}
void mrb_complex_copy(mrb_state *mrb, mrb_value x, mrb_value y)
{
}
#endif

#ifdef MRB_USE_RATIONAL
mrb_value mrb_rational_new(mrb_state *mrb, mrb_int x, mrb_int y)
{
  return mrb_nil_value();
}
mrb_value mrb_rational_add(mrb_state *mrb, mrb_value x, mrb_value y)
{
  return mrb_nil_value();
}
mrb_value mrb_rational_sub(mrb_state *mrb, mrb_value x, mrb_value y)
{
  return mrb_nil_value();
}
mrb_value mrb_rational_mul(mrb_state *mrb, mrb_value x, mrb_value y)
{
  return mrb_nil_value();
}
mrb_value mrb_rational_div(mrb_state *mrb, mrb_value x, mrb_value y)
{
  return mrb_nil_value();
}
mrb_value mrb_rational_to_i(mrb_state *mrb, mrb_value x)
{
  return mrb_nil_value();
}
mrb_value mrb_rational_to_f(mrb_state *mrb, mrb_value x)
{
  return mrb_nil_value();
}
mrb_value
mrb_as_rational(mrb_state *mrb, mrb_value x)
{
  return mrb_nil_value();
}
void mrb_rational_copy(mrb_state *mrb, mrb_value x, mrb_value y)
{
}
int mrb_rational_mark(mrb_state *mrb, struct RBasic *x)
{
  return 2;
}
#endif

#ifdef MRB_USE_SET
size_t mrb_gc_mark_set(mrb_state *mrb, struct RBasic *obj)
{
  /* stub for mrbc */
  return 0;
}

void mrb_gc_free_set(mrb_state *mrb, struct RBasic *obj)
{
  /* stub for mrbc */
}
#endif
