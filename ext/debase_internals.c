#include <debase_internals.h>

static VALUE mDebase;                 /* Ruby Debase Module object */
static VALUE cContext;
static VALUE cDebugThread;

static VALUE debug = Qfalse;
static VALUE locker = Qnil;
static VALUE contexts;
static VALUE catchpoints;
static VALUE breakpoints;

static VALUE tpLine;
static VALUE tpCall;
static VALUE tpReturn;
static VALUE tpRaise;

static VALUE idAlive;
static VALUE idAtLine;
static VALUE idAtBreakpoint;
static VALUE idAtCatchpoint;

static VALUE
Debase_thread_context(VALUE self, VALUE thread)
{
  VALUE context;

  context = rb_hash_aref(contexts, thread);
  if (context == Qnil) {
    context = context_create(thread, cDebugThread);
    rb_hash_aset(contexts, thread, context);
  }
  return context;
}

static VALUE
Debase_current_context(VALUE self)
{
  return Debase_thread_context(self, rb_thread_current());	
}

static int
remove_dead_threads(VALUE thread, VALUE context, VALUE ignored)
{
  return (IS_THREAD_ALIVE(thread)) ? ST_CONTINUE : ST_DELETE;
}

static void 
cleanup(debug_context_t *context)
{
  VALUE thread;

  context->stop_reason = CTX_STOP_NONE;

  /* release a lock */
  locker = Qnil;
  
  /* let the next thread to run */
  thread = remove_from_locked();
  if(thread != Qnil)
    rb_thread_run(thread);
}

static int
check_start_processing(debug_context_t *context, VALUE thread)
{
  /* return if thread is marked as 'ignored'.
    debugger's threads are marked this way
  */
  if(CTX_FL_TEST(context, CTX_FL_IGNORE)) return 0;

  while(1)
  {
    /* halt execution of the current thread if the debugger
       is activated in another
    */
    while(locker != Qnil && locker != thread)
    {
      add_to_locked(thread);
      rb_thread_stop();
    }

    /* stop the current thread if it's marked as suspended */
    if(CTX_FL_TEST(context, CTX_FL_SUSPEND) && locker != thread)
    {
      CTX_FL_SET(context, CTX_FL_WAS_RUNNING);
      rb_thread_stop();
    }
    else break;
  }

  /* return if the current thread is the locker */
  if(locker != Qnil) return 0;

  /* only the current thread can proceed */
  locker = thread;

  /* ignore a skipped section of code */
  if(CTX_FL_TEST(context, CTX_FL_SKIPPED)) {
    cleanup(context);
    return 0;
  }
  return 1;
}

inline void
load_frame_info(VALUE trace_point, VALUE *path, VALUE *lineno, VALUE *binding, VALUE *self)
{
  rb_trace_point_t *tp;

  tp = rb_tracearg_from_tracepoint(trace_point);

  *path = rb_tracearg_path(tp);
  *lineno = rb_tracearg_lineno(tp);
  *binding = rb_tracearg_binding(tp);
  *self = rb_tracearg_self(tp);
}

static void 
call_at_line(debug_context_t *context, char *file, int line, VALUE context_object, VALUE path, VALUE lineno)
{
  CTX_FL_UNSET(context, CTX_FL_STEPPED);
  CTX_FL_UNSET(context, CTX_FL_FORCE_MOVE);
  context->last_file = file;
  context->last_line = line;
  rb_funcall(context_object, idAtLine, 2, path, lineno);
}

static void
process_line_event(VALUE trace_point, void *data)
{
  VALUE path;
  VALUE lineno;
  VALUE binding;
  VALUE self;
  VALUE context_object;
  VALUE breakpoint;
  debug_context_t *context;
  char *file;
  int line;
  int moved;

  context_object = Debase_current_context(mDebase);
  Data_Get_Struct(context_object, debug_context_t, context);
  if (!check_start_processing(context, rb_thread_current())) return;

  load_frame_info(trace_point, &path, &lineno, &binding, &self);
  file = RSTRING_PTR(path);
  line = FIX2INT(lineno);
  update_frame(context_object, file, line, binding, self);

  if (debug == Qtrue)
    fprintf(stderr, "line: file=%s, line=%d, stack_size=%d\n", file, line, context->stack_size);

  moved = context->last_line != line || context->last_file == NULL ||
          strcmp(context->last_file, file) != 0;

  if(context->dest_frame == -1 || context->stack_size == context->dest_frame)
  {
      if(moved || !CTX_FL_TEST(context, CTX_FL_FORCE_MOVE))
          context->stop_next--;
      if(context->stop_next < 0)
          context->stop_next = -1;
      if(moved || (CTX_FL_TEST(context, CTX_FL_STEPPED) && !CTX_FL_TEST(context, CTX_FL_FORCE_MOVE)))
      {
          context->stop_line--;
          CTX_FL_UNSET(context, CTX_FL_STEPPED);
      }
  }
  else if(context->stack_size < context->dest_frame)
  {
      context->stop_next = 0;
  }

  breakpoint = breakpoint_find(breakpoints, path, lineno, binding);
  if(context->stop_next == 0 || context->stop_line == 0 || breakpoint != Qnil) {
    context->stop_reason = CTX_STOP_STEP;
    if (breakpoint != Qnil) {
      rb_funcall(context_object, idAtBreakpoint, 1, breakpoint);
    }
    reset_stepping_stop_points(context);
    call_at_line(context, file, line, context_object, path, lineno);
  }
  cleanup(context);
}

static void
process_return_event(VALUE trace_point, void *data)
{
  VALUE path;
  VALUE lineno;
  VALUE binding;
  VALUE self;
  VALUE context_object;
  debug_context_t *context;

  context_object = Debase_current_context(mDebase);
  Data_Get_Struct(context_object, debug_context_t, context);
  if (!check_start_processing(context, rb_thread_current())) return;

  if(context->stack_size == context->stop_frame)
  {
      context->stop_next = 1;
      context->stop_frame = 0;
  }

  load_frame_info(trace_point, &path, &lineno, &binding, &self);
  if (debug == Qtrue)
    fprintf(stderr, "return: file=%s, line=%d, stack_size=%d\n", RSTRING_PTR(path), FIX2INT(lineno), context->stack_size);
  // rb_funcall(context_object, idAtReturn, 2, path, lineno);
  pop_frame(context_object);
  cleanup(context);
}

static void
process_call_event(VALUE trace_point, void *data)
{
  VALUE path;
  VALUE lineno;
  VALUE binding;
  VALUE self;
  VALUE context_object;
  debug_context_t *context;

  context_object = Debase_current_context(mDebase);
  Data_Get_Struct(context_object, debug_context_t, context);
  if (!check_start_processing(context, rb_thread_current())) return;
  
  load_frame_info(trace_point, &path, &lineno, &binding, &self);
  if (debug == Qtrue)
    fprintf(stderr, "call: file=%s, line=%d, stack_size=%d\n", RSTRING_PTR(path), FIX2INT(lineno), context->stack_size);
  push_frame(context_object, RSTRING_PTR(path), FIX2INT(lineno), binding, self);
  cleanup(context);
}

static void
process_raise_event(VALUE trace_point, void *data)
{
  VALUE path;
  VALUE lineno;
  VALUE binding;
  VALUE self;
  VALUE context_object;
  VALUE hit_count;
  VALUE exception_name;
  debug_context_t *context;
  char *file;
  int line;
  int c_hit_count;

  context_object = Debase_current_context(mDebase);
  Data_Get_Struct(context_object, debug_context_t, context);
  if (!check_start_processing(context, rb_thread_current())) return;

  load_frame_info(trace_point, &path, &lineno, &binding, &self);
  file = RSTRING_PTR(path);
  line = FIX2INT(lineno);
  update_frame(context_object, file, line, binding, self);

  if (catchpoint_hit_count(catchpoints, rb_errinfo(), &exception_name) != Qnil) {
    /* On 64-bit systems with gcc and -O2 there seems to be
       an optimization bug in running INT2FIX(FIX2INT...)..)
       So we do this in two steps.
      */
    c_hit_count = FIX2INT(rb_hash_aref(catchpoints, exception_name)) + 1;
    hit_count = INT2FIX(c_hit_count);
    rb_hash_aset(catchpoints, exception_name, hit_count);
    context->stop_reason = CTX_STOP_CATCHPOINT;
    rb_funcall(context_object, idAtCatchpoint, 1, rb_errinfo());
    call_at_line(context, file, line, context_object, path, lineno);
  }

  cleanup(context);
}

static VALUE
Debase_setup_tracepoints(VALUE self)
{	
  if (catchpoints != Qnil) return Qnil;
  contexts = rb_hash_new();
  breakpoints = rb_ary_new();
  catchpoints = rb_hash_new();

  tpLine = rb_tracepoint_new(Qnil, RUBY_EVENT_LINE, process_line_event, NULL);
  rb_tracepoint_enable(tpLine);
  tpReturn = rb_tracepoint_new(Qnil, RUBY_EVENT_RETURN | RUBY_EVENT_C_RETURN | RUBY_EVENT_B_RETURN | RUBY_EVENT_CLASS, 
                               process_return_event, NULL);
  rb_tracepoint_enable(tpReturn);
  tpCall = rb_tracepoint_new(Qnil, RUBY_EVENT_CALL | RUBY_EVENT_C_CALL | RUBY_EVENT_B_CALL | RUBY_EVENT_END, 
                             process_call_event, NULL);
  rb_tracepoint_enable(tpCall);
  tpRaise = rb_tracepoint_new(Qnil, RUBY_EVENT_RAISE, process_raise_event, NULL);
  rb_tracepoint_enable(tpRaise);
  return Qnil;
}

static VALUE
Debase_remove_tracepoints(VALUE self)
{ 
  contexts = Qnil;
  breakpoints = Qnil;
  catchpoints = Qnil;

  if (tpLine != Qnil) rb_tracepoint_disable(tpLine);
  tpLine = Qnil;
  if (tpReturn != Qnil) rb_tracepoint_disable(tpReturn);
  tpReturn = Qnil;
  if (tpCall != Qnil) rb_tracepoint_disable(tpCall);
  tpCall = Qnil;
  if (tpRaise != Qnil) rb_tracepoint_disable(tpRaise);
  tpRaise = Qnil;
  return Qnil;
}

static VALUE
debase_prepare_context(VALUE self, VALUE file, VALUE stop)
{
  VALUE context_object;
  debug_context_t *context;

  context_object = Debase_current_context(self);
  Data_Get_Struct(context_object, debug_context_t, context);

  if(RTEST(stop)) context->stop_next = 1;
  ruby_script(RSTRING_PTR(file));
  return self;
}

static VALUE
Debase_debug_load(int argc, VALUE *argv, VALUE self)
{
  VALUE file, stop, increment_start;
  int state;

  if(rb_scan_args(argc, argv, "12", &file, &stop, &increment_start) == 1) 
  {
      stop = Qfalse;
      increment_start = Qtrue;
  }
  Debase_setup_tracepoints(self);
  debase_prepare_context(self, file, stop);
  rb_load_protect(file, 0, &state);
  if (0 != state) 
  {
      return rb_errinfo();
  }
  return Qnil;
}

static int
values_i(VALUE key, VALUE value, VALUE ary)
{
    rb_ary_push(ary, value);
    return ST_CONTINUE;
}

static VALUE
Debase_contexts(VALUE self)
{
  VALUE ary;

  ary = rb_ary_new();
  /* check that all contexts point to alive threads */
  rb_hash_foreach(contexts, remove_dead_threads, 0);
 
  rb_hash_foreach(contexts, values_i, ary);

  return ary;
}

static VALUE
Debase_breakpoints(VALUE self)
{
  return breakpoints;
}

static VALUE
Debase_catchpoints(VALUE self)
{
  if (catchpoints == Qnil)
    rb_raise(rb_eRuntimeError, "Debugger.start is not called yet.");
  return catchpoints; 
}

static VALUE
Debase_started(VALUE self)
{
  return catchpoints != Qnil ? Qtrue : Qfalse; 
}
/*
 *   Document-class: Debase
 *
 *   == Summary
 *
 *   This is a singleton class allows controlling the debugger. Use it to start/stop debugger,
 *   set/remove breakpoints, etc.
 */
void
Init_debase_internals()
{
  mDebase = rb_define_module("Debase");
  rb_define_module_function(mDebase, "setup_tracepoints", Debase_setup_tracepoints, 0);
  rb_define_module_function(mDebase, "remove_tracepoints", Debase_remove_tracepoints, 0);
  rb_define_module_function(mDebase, "current_context", Debase_current_context, 0);
  rb_define_module_function(mDebase, "debug_load", Debase_debug_load, -1);
  rb_define_module_function(mDebase, "contexts", Debase_contexts, 0);
  rb_define_module_function(mDebase, "breakpoints", Debase_breakpoints, 0);
  rb_define_module_function(mDebase, "catchpoints", Debase_catchpoints, 0);
  rb_define_module_function(mDebase, "started?", Debase_started, 0);

  idAlive = rb_intern("alive?");
  idAtLine = rb_intern("at_line");
  idAtBreakpoint = rb_intern("at_breakpoint");
  idAtCatchpoint = rb_intern("at_catchpoint");

  cContext = Init_context(mDebase);
  Init_breakpoint(mDebase);
  cDebugThread  = rb_define_class_under(mDebase, "DebugThread", rb_cThread);
  contexts = Qnil;
  catchpoints = Qnil;
  breakpoints = Qnil;

  rb_global_variable(&locker);
  rb_global_variable(&breakpoints);
  rb_global_variable(&catchpoints);
  rb_global_variable(&contexts);
}