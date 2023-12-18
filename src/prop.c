#define R_NO_REMAP
#include <R.h>
#include <Rinternals.h>

extern SEXP sym_S7_class;

extern SEXP sym_name;
extern SEXP sym_parent;
extern SEXP sym_package;
extern SEXP sym_properties;
extern SEXP sym_abstract;
extern SEXP sym_constructor;
extern SEXP sym_validator;

extern SEXP ns_S7;
extern SEXP sym_dot_should_validate;

static inline
int name_idx(SEXP list, const char* name) {
  SEXP names = Rf_getAttrib(list, R_NamesSymbol);

  if (TYPEOF(names) == STRSXP)
    for (int i = 0, n = Rf_length(names); i < n; i++)
      if (strcmp(CHAR(STRING_ELT(names, i)), name) == 0)
        return i;
  return -1;
}

static inline
SEXP extract_name(SEXP list, const char* name) {
  int i = name_idx(list, name);
  return i == -1 ? R_NilValue : VECTOR_ELT(list, i);
}

static inline
Rboolean has_name(SEXP list, const char* name) {
  return (Rboolean) name_idx(list, name) != -1;
}

static inline
Rboolean inherits2(SEXP object, const char* name) {
  // like inherits in R, but iterates over the class STRSXP vector
  // in reverse, since S7_* is typically at the tail.
  SEXP klass = Rf_getAttrib(object, R_ClassSymbol);
  if (TYPEOF(klass) == STRSXP) {
    for (int i = Rf_length(klass)-1; i >= 0; i--) {
      if (strcmp(CHAR(STRING_ELT(klass, i)), name) == 0)
        return TRUE;
    }
  }
  return FALSE;
}

inline static
Rboolean is_s7_object(SEXP object) {
  return inherits2(object, "S7_object");
}

inline static
Rboolean is_s7_class(SEXP object) {
  return inherits2(object, "S7_class");
}

static
__attribute__ ((noreturn))
void signal_prop_error_unknown_(SEXP object, SEXP name) {
  static SEXP signal_prop_error_unknown = NULL;
  if (signal_prop_error_unknown == NULL)
    signal_prop_error_unknown =
      Rf_findVarInFrame(ns_S7, Rf_install("signal_prop_error_unknown"));

  Rf_eval(Rf_lang3(signal_prop_error_unknown, object, name), ns_S7);
  while(1);
}

SEXP prop_(SEXP object, SEXP name) {

  if (!is_s7_object(object))
    goto error;

  SEXP name_rchar = STRING_ELT(name, 0);
  const char* name_char = CHAR(name_rchar);
  SEXP name_sym = Rf_installTrChar(name_rchar);

  SEXP S7_class = Rf_getAttrib(object, sym_S7_class);
  SEXP properties = Rf_getAttrib(S7_class, sym_properties);
  SEXP value = Rf_getAttrib(object, name_sym);

  // if value was accessed as an attr, we still need to validate to make sure
  // the attr is actually a known class property
  if (value != R_NilValue)
    goto validate;

  // property not in attrs, try to get value using the getter()
  if (properties == R_NilValue) goto validate;

  SEXP property = extract_name(properties, name_char);
  if (property == R_NilValue) goto validate;

  SEXP getter = extract_name(property, "getter");
  if (getter == R_NilValue) goto validate;

  if (TYPEOF(getter) == CLOSXP)
    // we validated property is in properties list when accessing getter()
    return Rf_eval(Rf_lang2(getter, object), ns_S7);


  validate:

  if(has_name(properties, name_char))
    return value;

  if (S7_class == R_NilValue &&
      is_s7_class(object) && (
          name_sym == sym_name  ||
          name_sym == sym_parent  ||
          name_sym == sym_package  ||
          name_sym == sym_properties  ||
          name_sym == sym_abstract  ||
          name_sym == sym_constructor  ||
          name_sym == sym_validator
    ))
      return value;

  error:

  signal_prop_error_unknown_(object, name);
  return R_NilValue; // unreachable, for compiler
}

static inline
void check_is_S7_(SEXP object) {
  if (is_s7_object(object))
    return;

  static SEXP check_is_S7 = NULL;
  if (check_is_S7 == NULL)
    check_is_S7 = Rf_findVarInFrame(ns_S7, Rf_install("check_is_S7"));

  // will signal error
  Rf_eval(Rf_lang2(check_is_S7, object), ns_S7);
}

__attribute__ ((noreturn))
void signal_prop_error_read_only(SEXP object, SEXP name) {
  static SEXP fn = NULL;
  if (fn == NULL)
    fn = Rf_findVarInFrame(ns_S7, Rf_install("signal_prop_error_read_only"));

  Rf_eval(Rf_lang3(fn, object, name), ns_S7);
  while(1);
}


// maintain a stack of property setters being called,
// to make sure that property setters don't call themselves
// recursively via `prop<-`().
static SEXP setters_stack = NULL;

static inline
void prop_setters_stack_push(SEXP name_sym, SEXP setter) {
  if (setters_stack == NULL) {
    setters_stack = Rf_cons(R_NilValue, R_NilValue);
    R_PreserveObject(setters_stack);
  }

  // SEXP cell = Rf_cons( R_body_no_src(setter), CDR(setters_stack));
  SEXP cell = Rf_cons(setter, CDR(setters_stack));
  SETCDR(setters_stack, cell);
  SET_TAG(cell, name_sym);
}

// registered w/ on.exit() on prop<-() eval frame.
SEXP prop_setters_stack_pop_() {
  SETCDR(setters_stack, CDDR(setters_stack));
  return R_NilValue;
}

static inline
Rboolean prop_setters_stack_contains(SEXP name_sym, SEXP setter) {
  if(setters_stack == NULL) return FALSE;

  static int flags = 0 |
    IDENT_USE_BYTECODE |
    IDENT_USE_CLOENV |
    // IDENT_USE_SRCREF |
    IDENT_ATTR_BY_ORDER ;

  for (SEXP c = CDR(setters_stack); c != R_NilValue; c = CDR(c)) {
    if (TAG(c) == name_sym &&
        R_compute_identical(CAR(c), setter, flags)) {
      return TRUE;
    }
  }
  return FALSE;
}


static inline
SEXP call_setter(SEXP name_sym, SEXP setter, SEXP object, SEXP value, SEXP frame) {
  // make sure we don't infinitely recursively call the same setter if `prop<-`()
  // is called from within a setter.
  prop_setters_stack_push(name_sym, setter);
  static SEXP register_on_exit_pop = NULL;
  if (register_on_exit_pop == NULL) {
    register_on_exit_pop = R_ParseString(
      "on.exit(.Call(prop_setters_stack_pop_), add = TRUE)"
    );
    R_PreserveObject(register_on_exit_pop);
  }

  Rf_eval(register_on_exit_pop, frame);
  return Rf_eval(Rf_lang3(setter, object, value), ns_S7);
}

static inline
Rboolean prop_setter_stack_is_empty() {
  return (Rboolean) (setters_stack == NULL || CDR(setters_stack) == R_NilValue);
}

SEXP prop_set_(SEXP object, SEXP name, SEXP check_sexp, SEXP value, SEXP frame) {

  check_is_S7_(object);

  SEXP name_rchar = STRING_ELT(name, 0);
  const char* name_char = CHAR(name_rchar);
  SEXP name_sym = Rf_installTrChar(name_rchar);

  Rboolean check = Rf_asLogical(check_sexp);

  SEXP S7_class = Rf_getAttrib(object, sym_S7_class);
  SEXP properties = Rf_getAttrib(S7_class, sym_properties);
  SEXP property = extract_name(properties, name_char);

  if (property == R_NilValue)
    signal_prop_error_unknown_(object, name);

  SEXP setter = extract_name(property, "setter");
  SEXP getter = extract_name(property, "getter");

  if(getter != R_NilValue && setter == R_NilValue)
    signal_prop_error_read_only(object, name);

  if (TYPEOF(setter) == CLOSXP &&
      !prop_setters_stack_contains(name_sym, setter)) {

    // // alternative approach: effectively call `validate_eventially()` on custom setters,
    // // to  avoid validating if we're calling a setter() from a setter().,
    // // and only run validate() once from the topmost prop<- call
    // // (this approach, like the current one, doesn't handle the edge case of a setter()
    // // calling prop(not_self)<- )
    //
    // Rboolean is_toplevel_propset_call = prop_setter_stack_is_empty();
    // SEXP old_dot_should_validate = NULL;
    // if (is_toplevel_propset_call) {
    //   old_dot_should_validate = Rf_getAttrib(object, sym_dot_should_validate);
    //   Rf_setAttrib(object, old_dot_should_validate, Rf_ScalarLogical(FALSE));
    //   // micro-optimization opportunity: getsetAttrib() that returns the "old" value
    //   // it is replacing.
    // } else {
    //   check = FALSE;
    // }

    object = call_setter(name_sym, setter, object, value, frame);
    PROTECT(object);
    // if (is_toplevel_propset_call) {
    //    Rf_setAttrib(object, sym_dot_should_validate, old_dot_should_validate);
    // }

    // Current snapshots / reference R implementation skips validation
    // if prop<- invokes a custom property setter().
    // That behavior seems inconsistent with the docs.
    // Which is correct?
    // Early return here to match the current reference `prop<-` def.
    UNPROTECT(1);
    return object;

    PROTECT(object);

  } else {

    if (check) {
      static SEXP prop_validate = NULL;
      if (prop_validate == NULL)
        prop_validate = Rf_findVarInFrame(ns_S7, Rf_install("prop_validate"));
      SEXP errmsg = Rf_eval(Rf_lang4(prop_validate, property, value, object), ns_S7);
      if (errmsg != R_NilValue) {
        if (TYPEOF(errmsg) != STRSXP)
          Rf_error("prop_validate() returned unknown value");
        //// Maybe we shouldn't suppress the prop<- call here?
        // Rf_error(CHAR(STRING_ELT(errmsg, 0)));
        //// snapshot diff:
        //// -       Error:
        //// +       Error in `prop<-`:
        Rf_errorcall(R_NilValue, CHAR(STRING_ELT(errmsg, 0)));
      }
    }

    object = Rf_duplicate(object);
    PROTECT(object);
    Rf_setAttrib(object, name_sym, value);
  }

  // see comment above about validation w/ custom setters()
  // currently, validation is skipped w/ custom setters().
  // This line makes the C impl match the current snapshot, but probably
  // merits some discussion...
  check = check && prop_setter_stack_is_empty();

  if (check) {
    static SEXP validate = NULL;
    if (validate == NULL)
      validate = Rf_findVarInFrame(ns_S7, Rf_install("validate"));

    Rf_eval(Rf_lang4(validate, object,
                     /* recursive = */ Rf_ScalarLogical(TRUE),
                     /* properties =*/ Rf_ScalarLogical(FALSE)),
            ns_S7);
  }
  UNPROTECT(1);
  return object;
}
