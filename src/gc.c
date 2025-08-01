/*
** gc.c - garbage collector for mruby
**
** See Copyright Notice in mruby.h
*/

#include <string.h>
#ifdef MRB_USE_MALLOC_TRIM
#include <malloc.h>
#endif
#include <mruby.h>
#include <mruby/array.h>
#include <mruby/class.h>
#include <mruby/data.h>
#include <mruby/istruct.h>
#include <mruby/hash.h>
#include <mruby/proc.h>
#include <mruby/range.h>
#include <mruby/string.h>
#include <mruby/variable.h>
#include <mruby/gc.h>
#include <mruby/error.h>
#include <mruby/throw.h>
#include <mruby/internal.h>
#include <mruby/presym.h>

#ifdef MRB_GC_STRESS
#include <stdlib.h>
#endif

/*
  = Tri-color Incremental Garbage Collection

  mruby's GC is Tri-color Incremental GC with Mark & Sweep.
  Algorithm details are omitted.
  Instead, the implementation part is described below.

  == Object's Color

  Each object can be painted in three colors:

    * White - Unmarked.
    * Gray - Marked, But the child objects are unmarked.
    * Black - Marked, the child objects are also marked.

  Extra color

    * Red - Static (ROM object) no need to be collected.
          - All child objects should be Red as well.

  == Two White Types

  There are two white color types in a flip-flop fashion: White-A and White-B,
  which respectively represent the Current White color (the newly allocated
  objects in the current GC cycle) and the Sweep Target White color (the
  dead objects to be swept).

  A and B will be switched just at the beginning of the next GC cycle. At
  that time, all the dead objects have been swept, while the newly created
  objects in the current GC cycle which finally remains White are now
  regarded as dead objects. Instead of traversing all the White-A objects and
  painting them as White-B, just switch the meaning of White-A and White-B as
  this will be much cheaper.

  As a result, the objects we sweep in the current GC cycle are always
  left from the previous GC cycle. This allows us to sweep objects
  incrementally, without the disturbance of the newly created objects.

  == Execution Timing

  GC Execution Time and Each step interval are decided by live objects count.
  List of Adjustment API:

    * gc_interval_ratio_set
    * gc_step_ratio_set

  For details, see the comments for each function.

  == Write Barrier

  mruby implementer and C extension library writer must insert a write
  barrier when updating a reference from a field of an object.
  When updating a reference from a field of object A to object B,
  two different types of write barrier are available:

    * mrb_field_write_barrier - target B object for a mark.
    * mrb_write_barrier       - target A object for a mark.

  == Generational Mode

  mruby's GC offers an Generational Mode while reusing the tri-color GC
  infrastructure. It will treat the Black objects as Old objects after each
  sweep phase, instead of painting them White. The key ideas are still the same
  as traditional generational GC:

    * Minor GC - just traverse the Young objects (Gray objects) in the mark
                 phase, then only sweep the newly created objects, and leave
                 the Old objects live.

    * Major GC - same as a full regular GC cycle.

  The difference from "traditional" generational GC is, that the major GC
  in mruby is triggered incrementally in a tri-color manner.


  For details, see the comments for each function.

*/

typedef struct RVALUE RVALUE;

struct free_obj {
  MRB_OBJECT_HEADER;
  RVALUE *next;
};

struct RVALUE_initializer {
  MRB_OBJECT_HEADER;
  char padding[sizeof(void*) * 4 - sizeof(uint32_t)];
};

struct RVALUE {
  union {
    struct RVALUE_initializer init;  /* must be first member to ensure initialization */
    struct free_obj free;
    struct RBasic basic;
    struct RObject object;
    struct RClass klass;
    struct RString string;
    struct RArray array;
    struct RHash hash;
    struct RRange range;
    struct RData data;
    struct RIStruct istruct;
    struct RProc proc;
    struct REnv env;
    struct RFiber fiber;
    struct RException exc;
    struct RBreak brk;
  } as;
};

#ifdef GC_DEBUG
#define DEBUG(x) (x)
#else
#define DEBUG(x)
#endif

#ifndef MRB_HEAP_PAGE_SIZE
#define MRB_HEAP_PAGE_SIZE 1024
#endif

typedef struct mrb_heap_page {
  RVALUE *freelist;
  struct mrb_heap_page *next;
  struct mrb_heap_page *free_next;
  mrb_bool old:1;
  RVALUE objects[MRB_HEAP_PAGE_SIZE];
} mrb_heap_page;

#define GC_STEP_SIZE 1024

/* white: 001 or 010, black: 100, gray: 000, red:111 */
#define GC_GRAY 0
#define GC_WHITE_A 1
#define GC_WHITE_B 2
#define GC_BLACK   4
#define GC_RED MRB_GC_RED
#define GC_WHITES (GC_WHITE_A | GC_WHITE_B)
#define GC_COLOR_MASK 7
mrb_static_assert(MRB_GC_RED <= GC_COLOR_MASK);

#define paint_gray(o) ((o)->gc_color = GC_GRAY)
#define paint_black(o) ((o)->gc_color = GC_BLACK)
#define paint_white(o) ((o)->gc_color = GC_WHITES)
#define paint_partial_white(s, o) ((o)->gc_color = (s)->current_white_part)
#define is_gray(o) ((o)->gc_color == GC_GRAY)
#define is_white(o) ((o)->gc_color & GC_WHITES)
#define is_black(o) ((o)->gc_color == GC_BLACK)
#define is_red(o) ((o)->gc_color == GC_RED)
#define flip_white_part(s) ((s)->current_white_part = other_white_part(s))
#define other_white_part(s) ((s)->current_white_part ^ GC_WHITES)
#define is_dead(s, o) (((o)->gc_color & other_white_part(s) & GC_WHITES) || (o)->tt == MRB_TT_FREE)

mrb_noreturn void mrb_raise_nomemory(mrb_state *mrb);

MRB_API void*
mrb_realloc_simple(mrb_state *mrb, void *p,  size_t len)
{
  void *p2;

#if defined(MRB_GC_STRESS) && defined(MRB_DEBUG)
  if (mrb->gc.state != MRB_GC_STATE_SWEEP) {
    mrb_full_gc(mrb);
  }
#endif
  p2 = mrb_basic_alloc_func(p, len);
  if (!p2 && len > 0 && mrb->gc.heaps && mrb->gc.state != MRB_GC_STATE_SWEEP) {
    mrb_full_gc(mrb);
    p2 = mrb_basic_alloc_func(p, len);
  }

  return p2;
}

MRB_API void*
mrb_realloc(mrb_state *mrb, void *p, size_t len)
{
  void *p2;

  p2 = mrb_realloc_simple(mrb, p, len);
  if (len == 0) return p2;
  if (p2 == NULL) {
    mrb->gc.out_of_memory = TRUE;
    mrb_raise_nomemory(mrb);
  }
  else {
    mrb->gc.out_of_memory = FALSE;
  }

  return p2;
}

MRB_API void*
mrb_malloc(mrb_state *mrb, size_t len)
{
  return mrb_realloc(mrb, 0, len);
}

MRB_API void*
mrb_malloc_simple(mrb_state *mrb, size_t len)
{
  return mrb_realloc_simple(mrb, 0, len);
}

MRB_API void*
mrb_calloc(mrb_state *mrb, size_t nelem, size_t len)
{
  void *p;

  if (nelem > 0 && len > 0 &&
      nelem <= SIZE_MAX / len) {
    size_t size;
    size = nelem * len;
    p = mrb_malloc(mrb, size);

    memset(p, 0, size);
  }
  else {
    p = NULL;
  }

  return p;
}

MRB_API void
mrb_free(mrb_state *mrb, void *p)
{
  mrb_basic_alloc_func(p, 0);
}

MRB_API void*
mrb_alloca(mrb_state *mrb, size_t size)
{
  struct RString *s;
  s = MRB_OBJ_ALLOC(mrb, MRB_TT_STRING, NULL);
  return s->as.heap.ptr = (char*)mrb_malloc(mrb, size);
}

static mrb_bool
heap_p(mrb_gc *gc, const struct RBasic *object)
{
  mrb_heap_page* page;

  page = gc->heaps;
  while (page) {
    RVALUE *p;

    p = page->objects;
    if ((uintptr_t)object - (uintptr_t)p <= (MRB_HEAP_PAGE_SIZE - 1) * sizeof(RVALUE)) {
      return TRUE;
    }
    page = page->next;
  }
  return FALSE;
}

MRB_API mrb_bool
mrb_object_dead_p(mrb_state *mrb, struct RBasic *object)
{
  mrb_gc *gc = &mrb->gc;
  if (!heap_p(gc, object)) return TRUE;
  return is_dead(gc, object);
}

static void
add_heap(mrb_state *mrb, mrb_gc *gc)
{
  mrb_heap_page *page = (mrb_heap_page*)mrb_calloc(mrb, 1, sizeof(mrb_heap_page));
  RVALUE *p, *e;
  RVALUE *prev = NULL;

  for (p = page->objects, e=p+MRB_HEAP_PAGE_SIZE; p<e; p++) {
    p->as.free.tt = MRB_TT_FREE;
    p->as.free.next = prev;
    prev = p;
  }
  page->freelist = prev;

  page->next = gc->heaps;
  gc->heaps = page;

  page->free_next = gc->free_heaps;
  gc->free_heaps = page;
}

#define DEFAULT_GC_INTERVAL_RATIO 200
#define DEFAULT_GC_STEP_RATIO 200
#define MAJOR_GC_INC_RATIO 120
#define MAJOR_GC_TOOMANY 10000
#define is_generational(gc) ((gc)->generational)
#define is_major_gc(gc) (is_generational(gc) && (gc)->full)
#define is_minor_gc(gc) (is_generational(gc) && !(gc)->full)

void
mrb_gc_init(mrb_state *mrb, mrb_gc *gc)
{
#ifndef MRB_GC_FIXED_ARENA
  gc->arena = (struct RBasic**)mrb_malloc(mrb, sizeof(struct RBasic*)*MRB_GC_ARENA_SIZE);
  gc->arena_capa = MRB_GC_ARENA_SIZE;
#endif

  gc->current_white_part = GC_WHITE_A;
  gc->heaps = NULL;
  gc->free_heaps = NULL;
  add_heap(mrb, gc);
  gc->interval_ratio = DEFAULT_GC_INTERVAL_RATIO;
  gc->step_ratio = DEFAULT_GC_STEP_RATIO;
#ifndef MRB_GC_TURN_OFF_GENERATIONAL
  gc->generational = TRUE;
  gc->full = TRUE;
#endif
}

static void obj_free(mrb_state *mrb, struct RBasic *obj, mrb_bool end);

static void
free_heap(mrb_state *mrb, mrb_gc *gc)
{
  mrb_heap_page *page = gc->heaps;
  mrb_heap_page *tmp;
  RVALUE *p, *e;

  while (page) {
    tmp = page;
    page = page->next;
    for (p = tmp->objects, e=p+MRB_HEAP_PAGE_SIZE; p<e; p++) {
      if (p->as.free.tt != MRB_TT_FREE)
        obj_free(mrb, &p->as.basic, TRUE);
    }
    mrb_free(mrb, tmp);
  }
}

void
mrb_gc_destroy(mrb_state *mrb, mrb_gc *gc)
{
  free_heap(mrb, gc);
#ifndef MRB_GC_FIXED_ARENA
  mrb_free(mrb, gc->arena);
#endif
}

static void
gc_arena_keep(mrb_state *mrb, mrb_gc *gc)
{
#ifdef MRB_GC_FIXED_ARENA
  if (gc->arena_idx >= MRB_GC_ARENA_SIZE) {
    /* arena overflow error */
    gc->arena_idx = MRB_GC_ARENA_SIZE - 4; /* force room in arena */
    mrb_exc_raise(mrb, mrb_obj_value(mrb->arena_err));
  }
#else
  if (gc->arena_idx >= gc->arena_capa) {
    /* extend arena */
    int newcapa = gc->arena_capa * 3 / 2;
    gc->arena = (struct RBasic**)mrb_realloc(mrb, gc->arena, sizeof(struct RBasic*)*newcapa);
    gc->arena_capa = newcapa;
  }
#endif
}

static inline void
gc_protect(mrb_state *mrb, mrb_gc *gc, struct RBasic *p)
{
#ifdef MRB_GC_FIXED_ARENA
  mrb_assert(gc->arena_idx < MRB_GC_ARENA_SIZE);
#else
  mrb_assert(gc->arena_idx < gc->arena_capa);
#endif
  gc->arena[gc->arena_idx++] = p;
}

/* mrb_gc_protect() leaves the object in the arena */
MRB_API void
mrb_gc_protect(mrb_state *mrb, mrb_value obj)
{
  if (mrb_immediate_p(obj)) return;
  struct RBasic *p = mrb_basic_ptr(obj);
  if (is_red(p)) return;
  gc_arena_keep(mrb, &mrb->gc);
  gc_protect(mrb, &mrb->gc, p);
}

#define GC_ROOT_SYM MRB_SYM(_gc_root_)

/* mrb_gc_register() keeps the object from GC.

   Register your object when it's exported to C world,
   without reference from Ruby world, e.g. callback
   arguments.  Don't forget to remove the object using
   mrb_gc_unregister, otherwise your object will leak.
*/

MRB_API void
mrb_gc_register(mrb_state *mrb, mrb_value obj)
{
  mrb_value table;

  if (mrb_immediate_p(obj)) return;
  table = mrb_gv_get(mrb, GC_ROOT_SYM);
  int ai = mrb_gc_arena_save(mrb);
  mrb_gc_protect(mrb, obj);
  if (!mrb_array_p(table)) {
    table = mrb_ary_new(mrb);
    mrb_obj_ptr(table)->c = NULL; /* hide from ObjectSpace.each_object */
    mrb_gv_set(mrb, GC_ROOT_SYM, table);
  }
  mrb_ary_push(mrb, table, obj);
  mrb_gc_arena_restore(mrb, ai);
}

/* mrb_gc_unregister() removes the object from GC root. */
MRB_API void
mrb_gc_unregister(mrb_state *mrb, mrb_value obj)
{
  mrb_value table;
  struct RArray *a;

  if (mrb_immediate_p(obj)) return;
  table = mrb_gv_get(mrb, GC_ROOT_SYM);
  if (!mrb_array_p(table)) return;
  a = mrb_ary_ptr(table);
  mrb_ary_modify(mrb, a);
  mrb_int len = ARY_LEN(a)-1;
  mrb_value *ptr = ARY_PTR(a);
  for (mrb_int i = 0; i <= len; i++) {
    if (mrb_ptr(ptr[i]) == mrb_ptr(obj)) {
      ARY_SET_LEN(a, len);
      memmove(&ptr[i], &ptr[i + 1], (len - i) * sizeof(mrb_value));
      break;
    }
  }
}

MRB_API struct RBasic*
mrb_obj_alloc(mrb_state *mrb, enum mrb_vtype ttype, struct RClass *cls)
{
  static const RVALUE RVALUE_zero = { { { NULL, NULL, MRB_TT_FALSE } } };
  mrb_gc *gc = &mrb->gc;

  if (cls) {
    enum mrb_vtype tt;

    switch (cls->tt) {
    case MRB_TT_CLASS:
    case MRB_TT_SCLASS:
    case MRB_TT_MODULE:
    case MRB_TT_ENV:
      break;
    default:
      mrb_raise(mrb, E_TYPE_ERROR, "allocation failure");
    }
    tt = MRB_INSTANCE_TT(cls);
    if (ttype != MRB_TT_SCLASS &&
        ttype != MRB_TT_ICLASS &&
        ttype != MRB_TT_ENV &&
        ttype != MRB_TT_BIGINT &&
        ttype != tt &&
        !(cls == mrb->object_class && (ttype == MRB_TT_CPTR || ttype == MRB_TT_CDATA || ttype == MRB_TT_ISTRUCT))) {
      mrb_raisef(mrb, E_TYPE_ERROR, "allocation failure of %C", cls);
    }
  }
  if (ttype <= MRB_TT_FREE) {
    mrb_raisef(mrb, E_TYPE_ERROR, "allocation failure of %C (type %d)", cls, (int)ttype);
  }

#ifdef MRB_GC_STRESS
  mrb_full_gc(mrb);
#endif
  if (gc->threshold < gc->live) {
    mrb_incremental_gc(mrb);
  }
  gc_arena_keep(mrb, gc);
  if (gc->free_heaps == NULL) {
    add_heap(mrb, gc);
  }

  RVALUE *p = gc->free_heaps->freelist;
  gc->free_heaps->freelist = p->as.free.next;
  if (gc->free_heaps->freelist == NULL) {
    gc->free_heaps = gc->free_heaps->free_next;
  }

  gc->live++;
  gc_protect(mrb, gc, &p->as.basic);
  *p = RVALUE_zero;
  p->as.basic.tt = ttype;
  p->as.basic.c = cls;
  paint_partial_white(gc, &p->as.basic);
  return &p->as.basic;
}

static inline void
add_gray_list(mrb_gc *gc, struct RBasic *obj)
{
#ifdef MRB_GC_STRESS
  if (obj->tt > MRB_TT_MAXDEFINE) {
    abort();
  }
#endif
  paint_gray(obj);
  obj->gcnext = gc->gray_list;
  gc->gray_list = obj;
}

static void
mark_context_stack(mrb_state *mrb, struct mrb_context *c)
{
  size_t i, e;

  if (c->stbase == NULL) return;
  if (c->ci) {
    e = (c->ci->stack ? c->ci->stack - c->stbase : 0);
    e += mrb_ci_nregs(c->ci);
  }
  else {
    e = 0;
  }
  if (c->stbase + e > c->stend) e = c->stend - c->stbase;
  for (i=0; i<e; i++) {
    mrb_value v = c->stbase[i];

    if (!mrb_immediate_p(v)) {
      mrb_gc_mark(mrb, mrb_basic_ptr(v));
    }
  }
  e = c->stend - c->stbase;
  for (; i<e; i++) {
    SET_NIL_VALUE(c->stbase[i]);
  }
}

static void
mark_context(mrb_state *mrb, struct mrb_context *c)
{
  mrb_callinfo *ci;

 start:
  if (c->status == MRB_FIBER_TERMINATED) return;

  /* mark VM stack */
  mark_context_stack(mrb, c);

  /* mark call stack */
  if (c->cibase) {
    for (ci = c->cibase; ci <= c->ci; ci++) {
      mrb_gc_mark(mrb, (struct RBasic*)ci->proc);
      mrb_gc_mark(mrb, (struct RBasic*)ci->u.target_class);
    }
  }
  /* mark fibers */
  mrb_gc_mark(mrb, (struct RBasic*)c->fib);
  if (c->prev) {
    c = c->prev;
    goto start;
  }
}

static size_t
gc_mark_children(mrb_state *mrb, mrb_gc *gc, struct RBasic *obj)
{
  size_t children = 0;

  mrb_assert(is_gray(obj));
  paint_black(obj);
  mrb_gc_mark(mrb, (struct RBasic*)obj->c);
  switch (obj->tt) {
  case MRB_TT_ICLASS:
    {
      struct RClass *c = (struct RClass*)obj;
      if (MRB_FLAG_TEST(c, MRB_FL_CLASS_IS_ORIGIN)) {
        children += mrb_gc_mark_mt(mrb, c);
      }
      mrb_gc_mark(mrb, (struct RBasic*)((struct RClass*)obj)->super);
      children++;
    }
    break;

  case MRB_TT_CLASS:
  case MRB_TT_MODULE:
  case MRB_TT_SCLASS:
    {
      struct RClass *c = (struct RClass*)obj;

      mrb_gc_mark_mt(mrb, c);
      mrb_gc_mark(mrb, (struct RBasic*)c->super);
      children += mrb_gc_mark_mt(mrb, c);
      children++;
    }
    /* fall through */

  case MRB_TT_OBJECT:
  case MRB_TT_CDATA:
    children += mrb_gc_mark_iv(mrb, (struct RObject*)obj);
    break;

  case MRB_TT_PROC:
    {
      struct RProc *p = (struct RProc*)obj;

      mrb_gc_mark(mrb, (struct RBasic*)p->upper);
      mrb_gc_mark(mrb, (struct RBasic*)p->e.env);
      children+=2;
    }
    break;

  case MRB_TT_ENV:
    {
      struct REnv *e = (struct REnv*)obj;

      // The data stack must always be protected from GC regardless of the MRB_ENV_CLOSE flag.
      // This is because the data stack is not protected if the fiber is GC'd.
      mrb_int len = MRB_ENV_LEN(e);
      for (mrb_int i=0; i<len; i++) {
        mrb_gc_mark_value(mrb, e->stack[i]);
      }
      children += len;
    }
    break;

  case MRB_TT_FIBER:
    {
      struct mrb_context *c = ((struct RFiber*)obj)->cxt;

      if (!c || c->status == MRB_FIBER_TERMINATED) break;
      mark_context(mrb, c);
      if (!c->ci) break;

      /* mark stack */
      size_t i = c->ci->stack - c->stbase;
      i += mrb_ci_nregs(c->ci);
      if (c->stbase + i > c->stend) i = c->stend - c->stbase;
      children += i;

      /* mark closure */
      if (c->cibase) {
        children += c->ci - c->cibase + 1;
      }
    }
    break;

  case MRB_TT_STRUCT:
  case MRB_TT_ARRAY:
    {
      struct RArray *a = (struct RArray*)obj;
      size_t len = ARY_LEN(a);
      mrb_value *p = ARY_PTR(a);

      for (size_t i=0; i<len; i++) {
        mrb_gc_mark_value(mrb, p[i]);
      }
      children += len;
    }
    break;

  case MRB_TT_HASH:
    children += mrb_gc_mark_iv(mrb, (struct RObject*)obj);
    children += mrb_gc_mark_hash(mrb, (struct RHash*)obj);
    break;

  case MRB_TT_STRING:
    if (RSTR_FSHARED_P(obj)) {
      struct RString *s = (struct RString*)obj;
      mrb_gc_mark(mrb, (struct RBasic*)s->as.heap.aux.fshared);
    }
    break;

  case MRB_TT_RANGE:
    children += mrb_gc_mark_range(mrb, (struct RRange*)obj);
    break;

  case MRB_TT_BREAK:
    {
      struct RBreak *brk = (struct RBreak*)obj;
      mrb_gc_mark_value(mrb, mrb_break_value_get(brk));
      children++;
    }
    break;

  case MRB_TT_EXCEPTION:
    children += mrb_gc_mark_iv(mrb, (struct RObject*)obj);
    if (((struct RException*)obj)->mesg) {
      mrb_gc_mark(mrb, (struct RBasic*)((struct RException*)obj)->mesg);
      children++;
    }
    if (((struct RException*)obj)->backtrace) {
      mrb_gc_mark(mrb, (struct RBasic*)((struct RException*)obj)->backtrace);
      children++;
    }
    break;

  case MRB_TT_BACKTRACE:
    children += ((struct RBacktrace*)obj)->len;
    break;

#if defined(MRB_USE_RATIONAL) && defined(MRB_USE_BIGINT)
  case MRB_TT_RATIONAL:
    children += mrb_rational_mark(mrb, obj);
    break;
#endif
#ifdef MRB_USE_SET
  case MRB_TT_SET:
    children += mrb_gc_mark_set(mrb, obj);
    break;
#endif

  default:
    break;
  }
  return children;
}

MRB_API void
mrb_gc_mark(mrb_state *mrb, struct RBasic *obj)
{
  if (obj == 0) return;
  if (!is_white(obj)) return;
  if (is_red(obj)) return;
  mrb_assert((obj)->tt != MRB_TT_FREE);
  add_gray_list(&mrb->gc, obj);
}

static void
obj_free(mrb_state *mrb, struct RBasic *obj, mrb_bool end)
{
  DEBUG(fprintf(stderr, "obj_free(%p,tt=%d)\n",obj,obj->tt));
  switch (obj->tt) {
  case MRB_TT_OBJECT:
    mrb_gc_free_iv(mrb, (struct RObject*)obj);
    break;

  case MRB_TT_EXCEPTION:
    mrb_gc_free_iv(mrb, (struct RObject*)obj);
    break;

  case MRB_TT_CLASS:
  case MRB_TT_MODULE:
  case MRB_TT_SCLASS:
    mrb_gc_free_mt(mrb, (struct RClass*)obj);
    mrb_gc_free_iv(mrb, (struct RObject*)obj);
    if (!end)
      mrb_mc_clear_by_class(mrb, (struct RClass*)obj);
    break;
  case MRB_TT_ICLASS:
    if (MRB_FLAG_TEST(obj, MRB_FL_CLASS_IS_ORIGIN))
      mrb_gc_free_mt(mrb, (struct RClass*)obj);
    if (!end)
      mrb_mc_clear_by_class(mrb, (struct RClass*)obj);
    break;
  case MRB_TT_ENV:
    {
      struct REnv *e = (struct REnv*)obj;

      if (!MRB_ENV_ONSTACK_P(e)) {
        mrb_free(mrb, e->stack);
      }
    }
    break;

  case MRB_TT_FIBER:
    {
      struct mrb_context *c = ((struct RFiber*)obj)->cxt;

      if (c && c != mrb->root_c) {
        if (!end && c->status != MRB_FIBER_TERMINATED) {
          mrb_callinfo *ci = c->ci;
          mrb_callinfo *ce = c->cibase;

          while (ce <= ci) {
            struct REnv *e = ci->u.env;
            if (e && heap_p(&mrb->gc, (struct RBasic*)e) && !is_dead(&mrb->gc, (struct RBasic*)e) &&
                e->tt == MRB_TT_ENV && MRB_ENV_ONSTACK_P(e)) {
              mrb_env_unshare(mrb, e, TRUE);
            }
            ci--;
          }
        }
        mrb_free_context(mrb, c);
      }
    }
    break;

  case MRB_TT_STRUCT:
  case MRB_TT_ARRAY:
    if (ARY_SHARED_P(obj))
      mrb_ary_decref(mrb, ((struct RArray*)obj)->as.heap.aux.shared);
    else if (!ARY_EMBED_P(obj))
      mrb_free(mrb, ((struct RArray*)obj)->as.heap.ptr);
    break;

  case MRB_TT_HASH:
    mrb_gc_free_iv(mrb, (struct RObject*)obj);
    mrb_gc_free_hash(mrb, (struct RHash*)obj);
    break;

  case MRB_TT_STRING:
    mrb_gc_free_str(mrb, (struct RString*)obj);
    break;

  case MRB_TT_PROC:
    {
      struct RProc *p = (struct RProc*)obj;

      if (!MRB_PROC_CFUNC_P(p) && !MRB_PROC_ALIAS_P(p) && p->body.irep) {
        mrb_irep *irep = (mrb_irep*)p->body.irep;
        if (end) {
          mrb_irep_cutref(mrb, irep);
        }
        mrb_irep_decref(mrb, irep);
      }
    }
    break;

  case MRB_TT_RANGE:
    mrb_gc_free_range(mrb, ((struct RRange*)obj));
    break;

#ifdef MRB_USE_SET
  case MRB_TT_SET:
    mrb_gc_free_set(mrb, obj);
    break;
#endif

  case MRB_TT_CDATA:
    {
      struct RData *d = (struct RData*)obj;
      if (d->type && d->type->dfree) {
        d->type->dfree(mrb, d->data);
      }
      mrb_gc_free_iv(mrb, (struct RObject*)obj);
    }
    break;

#if defined(MRB_USE_RATIONAL) && defined(MRB_INT64) && defined(MRB_32BIT)
  case MRB_TT_RATIONAL:
    {
      struct RData *o = (struct RData*)obj;
      mrb_free(mrb, o->iv);
    }
    break;
#endif

#if defined(MRB_USE_COMPLEX) && defined(MRB_32BIT) && !defined(MRB_USE_FLOAT32)
  case MRB_TT_COMPLEX:
    {
      struct RData *o = (struct RData*)obj;
      mrb_free(mrb, o->iv);
    }
    break;
#endif

#ifdef MRB_USE_BIGINT
  case MRB_TT_BIGINT:
    mrb_gc_free_bint(mrb, obj);
    break;
#endif

  case MRB_TT_BACKTRACE:
    {
      struct RBacktrace *bt = (struct RBacktrace*)obj;
      for (size_t i = 0; i < bt->len; i++) {
        const mrb_irep *irep = bt->locations[i].irep;
        if (irep == NULL) continue;
        mrb_irep_decref(mrb, (mrb_irep*)irep);
      }
      mrb_free(mrb, bt->locations);
    }

  default:
    break;
  }
#if defined(MRB_GC_STRESS) && defined(MRB_DEBUG)
  memset(obj, -1, sizeof(RVALUE));
  paint_white(obj);
#endif
  obj->tt = MRB_TT_FREE;
}

static void
root_scan_phase(mrb_state *mrb, mrb_gc *gc)
{
  int i, e;

  if (!is_minor_gc(gc)) {
    gc->gray_list = NULL;
    gc->atomic_gray_list = NULL;
  }

  mrb_gc_mark_gv(mrb);
  /* mark arena */
  for (i=0,e=gc->arena_idx; i<e; i++) {
    mrb_gc_mark(mrb, gc->arena[i]);
  }
  /* mark class hierarchy */
  mrb_gc_mark(mrb, (struct RBasic*)mrb->object_class);

  /* mark built-in classes */
  mrb_gc_mark(mrb, (struct RBasic*)mrb->class_class);
  mrb_gc_mark(mrb, (struct RBasic*)mrb->module_class);
  mrb_gc_mark(mrb, (struct RBasic*)mrb->proc_class);
  mrb_gc_mark(mrb, (struct RBasic*)mrb->string_class);
  mrb_gc_mark(mrb, (struct RBasic*)mrb->array_class);
  mrb_gc_mark(mrb, (struct RBasic*)mrb->hash_class);
  mrb_gc_mark(mrb, (struct RBasic*)mrb->range_class);

#ifndef MRB_NO_FLOAT
  mrb_gc_mark(mrb, (struct RBasic*)mrb->float_class);
#endif
  mrb_gc_mark(mrb, (struct RBasic*)mrb->integer_class);
  mrb_gc_mark(mrb, (struct RBasic*)mrb->true_class);
  mrb_gc_mark(mrb, (struct RBasic*)mrb->false_class);
  mrb_gc_mark(mrb, (struct RBasic*)mrb->nil_class);
  mrb_gc_mark(mrb, (struct RBasic*)mrb->symbol_class);
  mrb_gc_mark(mrb, (struct RBasic*)mrb->kernel_module);

  mrb_gc_mark(mrb, (struct RBasic*)mrb->eException_class);
  mrb_gc_mark(mrb, (struct RBasic*)mrb->eStandardError_class);

  /* mark top_self */
  mrb_gc_mark(mrb, (struct RBasic*)mrb->top_self);
  /* mark exception */
  mrb_gc_mark(mrb, (struct RBasic*)mrb->exc);

  mark_context(mrb, mrb->c);
  if (mrb->root_c != mrb->c) {
    mark_context(mrb, mrb->root_c);
  }
}

static void
gc_mark_gray_list(mrb_state *mrb, mrb_gc *gc) {
  while (gc->gray_list) {
    struct RBasic *obj = gc->gray_list;
    gc->gray_list = obj->gcnext;
    obj->gcnext = NULL;
    gc_mark_children(mrb, gc, obj);
  }
}

static size_t
incremental_marking_phase(mrb_state *mrb, mrb_gc *gc, size_t limit)
{
  size_t tried_marks = 0;

  while (gc->gray_list && tried_marks < limit) {
    struct RBasic *obj = gc->gray_list;
    gc->gray_list = obj->gcnext;
    obj->gcnext = NULL;
    tried_marks += gc_mark_children(mrb, gc, obj);
  }

  return tried_marks;
}

static void
clear_error_object(mrb_state *mrb, struct RObject *obj)
{
  if (obj == 0) return;
  if (!is_white(obj)) return;
  paint_black(obj);
  mrb_gc_mark(mrb, (struct RBasic*)obj->c);
  mrb_gc_free_iv(mrb, obj);
  struct RException *err = (struct RException*)obj;
  err->iv = NULL;
  err->mesg = NULL;
  err->backtrace = NULL;
}

static void
final_marking_phase(mrb_state *mrb, mrb_gc *gc)
{
  int i, e;

  /* mark arena */
  for (i=0,e=gc->arena_idx; i<e; i++) {
    mrb_gc_mark(mrb, gc->arena[i]);
  }
  mrb_gc_mark_gv(mrb);
  mark_context(mrb, mrb->c);
  if (mrb->c != mrb->root_c) {
    mark_context(mrb, mrb->root_c);
  }
  mrb_gc_mark(mrb, (struct RBasic*)mrb->exc);

  /* mark pre-allocated exception */
  clear_error_object(mrb, mrb->nomem_err);
  clear_error_object(mrb, mrb->stack_err);
#ifdef MRB_GC_FIXED_ARENA
  clear_error_object(mrb, mrb->arena_err);
#endif

  gc_mark_gray_list(mrb, gc);
  mrb_assert(gc->gray_list == NULL);
  gc->gray_list = gc->atomic_gray_list;
  gc->atomic_gray_list = NULL;
  gc_mark_gray_list(mrb, gc);
  mrb_assert(gc->gray_list == NULL);
}

static void
prepare_incremental_sweep(mrb_state *mrb, mrb_gc *gc)
{
  //  mrb_assert(gc->atomic_gray_list == NULL);
  //  mrb_assert(gc->gray_list == NULL);
  gc->state = MRB_GC_STATE_SWEEP;
  gc->sweeps = NULL;
  gc->live_after_mark = gc->live;
}

static size_t
incremental_sweep_phase(mrb_state *mrb, mrb_gc *gc, size_t limit)
{
  mrb_heap_page *prev = gc->sweeps;
  mrb_heap_page *page = prev ? prev->next : gc->heaps;
  size_t tried_sweep = 0;

  while (page && (tried_sweep < limit)) {
    RVALUE *p = page->objects;
    RVALUE *e = p + MRB_HEAP_PAGE_SIZE;
    size_t freed = 0;
    mrb_bool dead_slot = TRUE;

    if (is_minor_gc(gc) && page->old) {
      /* skip a slot which doesn't contain any young object */
      p = e;
      dead_slot = FALSE;
    }
    while (p<e) {
      if (is_dead(gc, &p->as.basic)) {
        if (p->as.basic.tt != MRB_TT_FREE) {
          obj_free(mrb, &p->as.basic, FALSE);
          if (p->as.basic.tt == MRB_TT_FREE) {
            p->as.free.next = page->freelist;
            page->freelist = p;
            freed++;
          }
          else {
            dead_slot = FALSE;
          }
        }
      }
      else {
        if (!is_generational(gc))
          paint_partial_white(gc, &p->as.basic); /* next gc target */
        dead_slot = FALSE;
      }
      p++;
    }

    /* free dead slot */
    if (dead_slot) {
      mrb_heap_page *next = page->next;

      if (prev) prev->next = next;
      if (gc->heaps == page)
        gc->heaps = page->next;

      mrb_free(mrb, page);
      page = next;
    }
    else {
      if (page->freelist == NULL && is_minor_gc(gc))
        page->old = TRUE;
      else
        page->old = FALSE;
      prev = page;
      page = page->next;
    }
    tried_sweep += MRB_HEAP_PAGE_SIZE;
    gc->live -= freed;
    gc->live_after_mark -= freed;
  }
  gc->sweeps = prev;

  /* rebuild free_heaps link */
  gc->free_heaps = NULL;
  for (mrb_heap_page *p = gc->heaps; p; p=p->next) {
    if (p->freelist) {
      p->free_next = gc->free_heaps;
      gc->free_heaps = p;
    }
  }

  return tried_sweep;
}

static size_t
incremental_gc(mrb_state *mrb, mrb_gc *gc, size_t limit)
{
  switch (gc->state) {
  case MRB_GC_STATE_ROOT:
    root_scan_phase(mrb, gc);
    gc->state = MRB_GC_STATE_MARK;
    flip_white_part(gc);
    return 0;
  case MRB_GC_STATE_MARK:
    if (gc->gray_list) {
      return incremental_marking_phase(mrb, gc, limit);
    }
    else {
      final_marking_phase(mrb, gc);
      prepare_incremental_sweep(mrb, gc);
      return 0;
    }
  case MRB_GC_STATE_SWEEP: {
     size_t tried_sweep = 0;
     tried_sweep = incremental_sweep_phase(mrb, gc, limit);
     if (tried_sweep == 0)
       gc->state = MRB_GC_STATE_ROOT;
     return tried_sweep;
  }
  default:
    /* unknown state */
    mrb_assert(0);
    return 0;
  }
}

static void
incremental_gc_finish(mrb_state *mrb, mrb_gc *gc)
{
  do {
    incremental_gc(mrb, gc, SIZE_MAX);
  } while (gc->state != MRB_GC_STATE_ROOT);
}

static void
incremental_gc_step(mrb_state *mrb, mrb_gc *gc)
{
  size_t limit = 0, result = 0;
  limit = (GC_STEP_SIZE/100) * gc->step_ratio;
  while (result < limit) {
    result += incremental_gc(mrb, gc, limit);
    if (gc->state == MRB_GC_STATE_ROOT)
      break;
  }

  gc->threshold = gc->live + GC_STEP_SIZE;
}

static void
clear_all_old(mrb_state *mrb, mrb_gc *gc)
{
  mrb_assert(is_generational(gc));
  if (gc->full) {
    /* finish the half baked GC */
    incremental_gc_finish(mrb, gc);
  }
  /* Sweep the dead objects, then reset all the live objects
   * (including all the old objects, of course) to white. */
  gc->generational = FALSE;
  prepare_incremental_sweep(mrb, gc);
  incremental_gc_finish(mrb, gc);
  gc->generational = TRUE;
  /* The gray objects have already been painted as white */
  gc->atomic_gray_list = gc->gray_list = NULL;
}

MRB_API void
mrb_incremental_gc(mrb_state *mrb)
{
  mrb_gc *gc = &mrb->gc;

  if (gc->disabled || gc->iterating) return;

  if (is_minor_gc(gc)) {
    incremental_gc_finish(mrb, gc);
  }
  else {
    incremental_gc_step(mrb, gc);
  }

  if (gc->state == MRB_GC_STATE_ROOT) {
    mrb_assert(gc->live >= gc->live_after_mark);
    gc->threshold = (gc->live_after_mark/100) * gc->interval_ratio;
    if (gc->threshold < GC_STEP_SIZE) {
      gc->threshold = GC_STEP_SIZE;
    }

    if (is_major_gc(gc)) {
      size_t threshold = gc->live_after_mark/100 * MAJOR_GC_INC_RATIO;

      gc->full = FALSE;
      if (threshold < MAJOR_GC_TOOMANY) {
        gc->oldgen_threshold = threshold;
      }
      else {
        /* too many objects allocated during incremental GC, */
        /* instead of increasing threshold, invoke full GC. */
        mrb_full_gc(mrb);
      }
    }
    else if (is_minor_gc(gc) && gc->live > gc->oldgen_threshold) {
      clear_all_old(mrb, gc);
      gc->full = TRUE;
    }
  }
}

/* Perform a full gc cycle */
MRB_API void
mrb_full_gc(mrb_state *mrb)
{
  mrb_gc *gc = &mrb->gc;

  if (!mrb->c) return;
  if (gc->disabled || gc->iterating) return;

  if (is_generational(gc)) {
    /* clear all the old objects back to young */
    clear_all_old(mrb, gc);
    gc->full = TRUE;
  }
  else if (gc->state != MRB_GC_STATE_ROOT) {
    /* finish half baked GC cycle */
    incremental_gc_finish(mrb, gc);
  }

  incremental_gc_finish(mrb, gc);
  gc->threshold = (gc->live_after_mark/100) * gc->interval_ratio;

  if (is_generational(gc)) {
    gc->oldgen_threshold = gc->live_after_mark/100 * MAJOR_GC_INC_RATIO;
    gc->full = FALSE;
  }

#ifdef MRB_USE_MALLOC_TRIM
  malloc_trim(0);
#endif
}

MRB_API void
mrb_garbage_collect(mrb_state *mrb)
{
  mrb_full_gc(mrb);
}

/*
 * Field write barrier
 *   Paint obj(Black) -> value(White) to obj(Black) -> value(Gray).
 */

MRB_API void
mrb_field_write_barrier(mrb_state *mrb, struct RBasic *obj, struct RBasic *value)
{
  mrb_gc *gc = &mrb->gc;

  if (!value) return;
  if (!is_black(obj)) return;
  if (!is_white(value)) return;
  if (is_red(value)) return;

  mrb_assert(gc->state == MRB_GC_STATE_MARK || (!is_dead(gc, value) && !is_dead(gc, obj)));
  mrb_assert(is_generational(gc) || gc->state != MRB_GC_STATE_ROOT);

  if (is_generational(gc) || gc->state == MRB_GC_STATE_MARK) {
    add_gray_list(gc, value);
  }
  else {
    mrb_assert(gc->state == MRB_GC_STATE_SWEEP);
    paint_partial_white(gc, obj); /* for never write barriers */
  }
}

/*
 * Write barrier
 *   Paint obj(Black) to obj(Gray).
 *
 *   The object that is painted gray will be traversed atomically in final
 *   mark phase. So you use this write barrier if it's frequency written spot.
 *   e.g. Set element on Array.
 */

MRB_API void
mrb_write_barrier(mrb_state *mrb, struct RBasic *obj)
{
  mrb_gc *gc = &mrb->gc;

  if (!is_black(obj)) return;

  mrb_assert(!is_dead(gc, obj));
  mrb_assert(is_generational(gc) || gc->state != MRB_GC_STATE_ROOT);
  paint_gray(obj);
  obj->gcnext = gc->atomic_gray_list;
  gc->atomic_gray_list = obj;
}

/*
 *  call-seq:
 *     GC.start                     -> nil
 *
 *  Initiates full garbage collection.
 *
 */

static mrb_value
gc_start(mrb_state *mrb, mrb_value obj)
{
  mrb_full_gc(mrb);
  return mrb_nil_value();
}

/*
 *  call-seq:
 *     GC.enable    -> true or false
 *
 *  Enables garbage collection, returning <code>true</code> if garbage
 *  collection was previously disabled.
 *
 *     GC.disable   #=> false
 *     GC.enable    #=> true
 *     GC.enable    #=> false
 *
 */

static mrb_value
gc_enable(mrb_state *mrb, mrb_value obj)
{
  mrb_bool old = mrb->gc.disabled;

  mrb->gc.disabled = FALSE;

  return mrb_bool_value(old);
}

/*
 *  call-seq:
 *     GC.disable    -> true or false
 *
 *  Disables garbage collection, returning <code>true</code> if garbage
 *  collection was already disabled.
 *
 *     GC.disable   #=> false
 *     GC.disable   #=> true
 *
 */

static mrb_value
gc_disable(mrb_state *mrb, mrb_value obj)
{
  mrb_bool old = mrb->gc.disabled;

  mrb->gc.disabled = TRUE;

  return mrb_bool_value(old);
}

/*
 *  call-seq:
 *     GC.interval_ratio      -> int
 *
 *  Returns ratio of GC interval. Default value is 200(%).
 *
 */

static mrb_value
gc_interval_ratio_get(mrb_state *mrb, mrb_value obj)
{
  return mrb_int_value(mrb, mrb->gc.interval_ratio);
}

/*
 *  call-seq:
 *     GC.interval_ratio = int    -> nil
 *
 *  Updates ratio of GC interval. Default value is 200(%).
 *  GC start as soon as after end all step of GC if you set 100(%).
 *
 */

static mrb_value
gc_interval_ratio_set(mrb_state *mrb, mrb_value obj)
{
  mrb_int ratio;

  mrb_get_args(mrb, "i", &ratio);
  mrb->gc.interval_ratio = (int)ratio;
  return mrb_nil_value();
}

/*
 *  call-seq:
 *     GC.step_ratio    -> int
 *
 *  Returns step span ratio of Incremental GC. Default value is 200(%).
 *
 */

static mrb_value
gc_step_ratio_get(mrb_state *mrb, mrb_value obj)
{
  return mrb_int_value(mrb, mrb->gc.step_ratio);
}

/*
 *  call-seq:
 *     GC.step_ratio = int   -> nil
 *
 *  Updates step span ratio of Incremental GC. Default value is 200(%).
 *  1 step of incrementalGC becomes long if a rate is big.
 *
 */

static mrb_value
gc_step_ratio_set(mrb_state *mrb, mrb_value obj)
{
  mrb_int ratio;

  mrb_get_args(mrb, "i", &ratio);
  mrb->gc.step_ratio = (int)ratio;
  return mrb_nil_value();
}

static void
change_gen_gc_mode(mrb_state *mrb, mrb_gc *gc, mrb_bool enable)
{
  if (gc->disabled || gc->iterating) {
    mrb_raise(mrb, E_RUNTIME_ERROR, "generational mode changed when GC disabled");
    return;
  }
  if (is_generational(gc) && !enable) {
    clear_all_old(mrb, gc);
    mrb_assert(gc->state == MRB_GC_STATE_ROOT);
    gc->full = FALSE;
  }
  else if (!is_generational(gc) && enable) {
    incremental_gc_finish(mrb, gc);
    gc->oldgen_threshold = gc->live_after_mark/100 * MAJOR_GC_INC_RATIO;
    gc->full = FALSE;
  }
  gc->generational = enable;
}

/*
 *  call-seq:
 *     GC.generational_mode -> true or false
 *
 *  Returns generational or normal gc mode.
 *
 */

static mrb_value
gc_generational_mode_get(mrb_state *mrb, mrb_value self)
{
  return mrb_bool_value(mrb->gc.generational);
}

/*
 *  call-seq:
 *     GC.generational_mode = true or false -> true or false
 *
 *  Changes to generational or normal gc mode.
 *
 */

static mrb_value
gc_generational_mode_set(mrb_state *mrb, mrb_value self)
{
  mrb_bool enable;

  mrb_get_args(mrb, "b", &enable);
  if (mrb->gc.generational != enable)
    change_gen_gc_mode(mrb, &mrb->gc, enable);

  return mrb_bool_value(enable);
}


static void
gc_each_objects(mrb_state *mrb, mrb_gc *gc, mrb_each_object_callback *callback, void *data)
{
  mrb_heap_page* page;

  page = gc->heaps;
  while (page != NULL) {
    RVALUE *p;

    p = page->objects;
    for (int i=0; i < MRB_HEAP_PAGE_SIZE; i++) {
      if ((*callback)(mrb, &p[i].as.basic, data) == MRB_EACH_OBJ_BREAK)
        return;
    }
    page = page->next;
  }
}

void
mrb_objspace_each_objects(mrb_state *mrb, mrb_each_object_callback *callback, void *data)
{
  mrb_bool iterating = mrb->gc.iterating;

  mrb_full_gc(mrb);
  mrb->gc.iterating = TRUE;
  if (iterating) {
    gc_each_objects(mrb, &mrb->gc, callback, data);
  }
  else {
    struct mrb_jmpbuf *prev_jmp = mrb->jmp;
    struct mrb_jmpbuf c_jmp;

    MRB_TRY(&c_jmp) {
      mrb->jmp = &c_jmp;
      gc_each_objects(mrb, &mrb->gc, callback, data);
      mrb->jmp = prev_jmp;
      mrb->gc.iterating = iterating;
    } MRB_CATCH(&c_jmp) {
      mrb->gc.iterating = iterating;
      mrb->jmp = prev_jmp;
      MRB_THROW(prev_jmp);
    } MRB_END_EXC(&c_jmp);
  }
}

size_t
mrb_objspace_page_slot_size(void)
{
  return sizeof(RVALUE);
}


void
mrb_init_gc(mrb_state *mrb)
{
  struct RClass *gc;

  mrb_static_assert_object_size(RVALUE);

  gc = mrb_define_module_id(mrb, MRB_SYM(GC));

  mrb_define_class_method_id(mrb, gc, MRB_SYM(start), gc_start, MRB_ARGS_NONE());
  mrb_define_class_method_id(mrb, gc, MRB_SYM(enable), gc_enable, MRB_ARGS_NONE());
  mrb_define_class_method_id(mrb, gc, MRB_SYM(disable), gc_disable, MRB_ARGS_NONE());
  mrb_define_class_method_id(mrb, gc, MRB_SYM(interval_ratio), gc_interval_ratio_get, MRB_ARGS_NONE());
  mrb_define_class_method_id(mrb, gc, MRB_SYM_E(interval_ratio), gc_interval_ratio_set, MRB_ARGS_REQ(1));
  mrb_define_class_method_id(mrb, gc, MRB_SYM(step_ratio), gc_step_ratio_get, MRB_ARGS_NONE());
  mrb_define_class_method_id(mrb, gc, MRB_SYM_E(step_ratio), gc_step_ratio_set, MRB_ARGS_REQ(1));
  mrb_define_class_method_id(mrb, gc, MRB_SYM_E(generational_mode), gc_generational_mode_set, MRB_ARGS_REQ(1));
  mrb_define_class_method_id(mrb, gc, MRB_SYM(generational_mode), gc_generational_mode_get, MRB_ARGS_NONE());
}
